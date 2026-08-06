#pragma once

#include <atomic>
#include <borealis.hpp>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * Exports borealis log lines over OTLP/HTTP so a console can be diagnosed
 * without pulling the SD card out on every iteration.
 *
 * Deliberately vendor neutral. OTLP is just JSON over HTTPS, so this talks to
 * any OTLP log endpoint: an OpenTelemetry Collector on your own network, a
 * hosted backend, or a vendor's direct intake. Nothing here knows or cares
 * which. Configuration follows the OpenTelemetry environment variable
 * conventions, read from files because a Switch has no environment to set.
 *
 *   <working dir>/otel-endpoint             OTEL_EXPORTER_OTLP_ENDPOINT
 *   <working dir>/otel-headers              OTEL_EXPORTER_OTLP_HEADERS
 *   <working dir>/otel-resource-attributes  OTEL_RESOURCE_ATTRIBUTES
 *   <working dir>/cacert.pem                CA bundle, required
 *
 * With no endpoint file this does nothing at all: no thread, no subscription,
 * no allocation.
 *
 * This cannot capture a fault that takes the process down with it, because it
 * exports from inside that process. It is for everything that leaves the app
 * alive, which since the sleep/resume fix includes the whole suspend and
 * resume window: the network is down while the console sleeps, so records
 * buffer and go out on wake.
 */
class OtlpLogExporter {
  public:
    static OtlpLogExporter& instance();

    /**
     * Reads configuration and, if an endpoint is set, subscribes to the log
     * event and starts the worker. Returns whether exporting is on. Safe to
     * call when nothing is configured, which is the normal case.
     */
    bool start(const std::string& workingDir);

    /** Stops the worker and makes a final attempt to flush what is buffered. */
    void stop();

    /**
     * Stops and restarts network activity around a console suspend.
     *
     * Nothing here may touch a socket while the app is out of focus. On this
     * platform losing focus means the console is going to sleep or the HOME
     * menu has taken over, and the OS tears the network stack down underneath
     * a running process. A POST already inside curl then sits in bsdsocket
     * across the suspend and resume boundary with a 15 second timeout, and
     * the process does not survive it.
     *
     * Found by testing: identical binary, endpoint file removed so the
     * exporter never enabled, survived sleep and resume three times out of
     * three. With the exporter enabled it faulted on the first attempt, and
     * the invalidation that runs on the first resumed frame never got to run
     * at all.
     *
     * Records keep accumulating while suspended and ship after resume; the
     * buffer is bounded and drops oldest, which is the right trade for a
     * sleep that lasts longer than the buffer.
     */
    void setSuspended(bool suspended);

    [[nodiscard]] bool enabled() const { return m_enabled; }

    /**
     * Buffers one line that did not come from brls::Logger.
     *
     * This is how StdoutCapture hands over raw writes to stdout and stderr:
     * printf from moonlight-common-c, ffmpeg and libnx, none of which go
     * anywhere near the borealis log event, and all of which are invisible to
     * this exporter otherwise. It is what nxlink shows and we do not.
     *
     * Recorded at INFO with a source attribute distinguishing it, because a
     * raw write carries no level. Deliberately NOT routed through
     * brls::Logger: borealis writes the line to logOut before firing the
     * event, logOut is stdout by default, and the capture would feed itself.
     * It also fires that event holding logMtx, so a call back into the logger
     * from here would deadlock on a non recursive mutex.
     *
     * Nothing this reaches may write to stdout or stderr. The invariant is
     * the same one the worker path holds, and for the same reason.
     */
    void logRaw(const std::string& line, bool fromStderr);

    /** Resolved endpoint, for logging. Contains no credentials. */
    [[nodiscard]] std::string endpoint() const { return m_endpoint; }

    struct Stats {
        size_t accepted;  // records taken from the log event
        size_t sent;      // records the endpoint accepted
        size_t droppedOverflow;
        size_t droppedFailed;
        size_t postFailures;
        // Spans and metrics ship on this worker too, and their POST results
        // used to be discarded. A rejected payload then looked exactly like an
        // idle app: nothing arrives, nothing complains. Counted so the exit
        // summary can say the transport was refused rather than stay silent.
        size_t tracePostFailures;
        size_t metricPostFailures;
    };

    [[nodiscard]] Stats stats() const;

    /**
     * Last transport failure, or empty. Recorded rather than logged: nothing
     * on the worker path may call brls::Logger. Read it from the main thread.
     */
    [[nodiscard]] std::string lastError() const;

  private:
    OtlpLogExporter() = default;
    ~OtlpLogExporter();

    OtlpLogExporter(const OtlpLogExporter&) = delete;
    OtlpLogExporter& operator=(const OtlpLogExporter&) = delete;

    struct Record {
        uint64_t timeUnixNano;
        int severityNumber;
        std::string severityText;
        std::string body;
        // Empty for anything that came through the borealis log event, which
        // is the common case. Set to "stdout" or "stderr" for a raw write
        // picked up by StdoutCapture, and emitted as a log.source attribute
        // so the two can be told apart in a query. Worth distinguishing:
        // borealis lines carry a real severity and these do not.
        std::string source;
        // Which thread emitted the line. Without it, two events milliseconds
        // apart cannot be told apart from one thread doing two things and two
        // threads racing, which is usually the entire question. The spans
        // carry the same attribute, so a log line and a span from the same
        // moment can be joined on it.
        uint64_t threadId;
    };

    /** Shared tail of onLogLine and logRaw: bounded push plus stats. */
    void enqueue(Record record);

    void onLogLine(brls::Logger::TimePoint when, brls::LogLevel level,
                   const std::string& line);
    void worker();
    /* @url so the same transport carries logs and spans: they go to
     * different OTLP signal paths but share the endpoint, headers and CA
     * bundle, and there is no reason to stand up a second curl for it. */
    bool post(const std::string& url, const std::string& body);
    [[nodiscard]] std::string buildPayload(const std::deque<Record>& batch) const;

    bool m_enabled = false;

    std::string m_endpoint;
    std::string m_caBundlePath;
    // Header values may carry credentials. Never logged, never returned.
    std::vector<std::pair<std::string, std::string>> m_headers;
    std::vector<std::pair<std::string, std::string>> m_resourceAttributes;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<Record> m_records;
    std::atomic<bool> m_stopping { false };
    std::atomic<bool> m_suspended { false };

    std::thread m_thread;
    brls::Event<brls::Logger::TimePoint, brls::LogLevel,
                std::string>::Subscription m_subscription;
    bool m_subscribed = false;

    mutable std::mutex m_statsMutex;
    Stats m_stats {};
    std::string m_lastError;

    /* Set once the worker has made its final pass with m_stopping set, so the
     * pass that drains spans and metrics happens exactly once and the loop
     * still terminates. */
    bool m_finalFlushDone = false;
};
