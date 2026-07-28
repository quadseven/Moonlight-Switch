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

    [[nodiscard]] bool enabled() const { return m_enabled; }

    /** Resolved endpoint, for logging. Contains no credentials. */
    [[nodiscard]] std::string endpoint() const { return m_endpoint; }

    struct Stats {
        size_t accepted;  // records taken from the log event
        size_t sent;      // records the endpoint accepted
        size_t droppedOverflow;
        size_t droppedFailed;
        size_t postFailures;
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
    };

    void onLogLine(brls::Logger::TimePoint when, brls::LogLevel level,
                   const std::string& line);
    void worker();
    bool post(const std::string& body);
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

    std::thread m_thread;
    brls::Event<brls::Logger::TimePoint, brls::LogLevel,
                std::string>::Subscription m_subscription;
    bool m_subscribed = false;

    mutable std::mutex m_statsMutex;
    Stats m_stats {};
    std::string m_lastError;
};
