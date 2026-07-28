#pragma once

#include <atomic>
#include <borealis.hpp>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

/**
 * Ships borealis log lines to the Datadog logs HTTP intake.
 *
 * Opt in by placing an API key in <working_dir>/datadog.key. With no key file
 * this does nothing at all, costs nothing, and is not wired up.
 *
 * Why an intake client and not the Datadog agent: the agent is Go, and Go has
 * no Horizon target, so there is nothing to build. The intake is just an HTTPS
 * POST, and the app already links curl built against mbedtls.
 *
 * This cannot capture a fault that takes the process down with it, because it
 * ships from inside that process. It is for everything that leaves the app
 * alive, which since the sleep/resume fix includes the whole suspend and
 * resume window: wifi is off while the console sleeps, so lines buffer and go
 * out when it comes back.
 */
class DatadogLogShipper {
  public:
    static DatadogLogShipper& instance();

    /**
     * Reads the key file and, if present, subscribes to the log event and
     * starts the worker. Returns whether shipping is on. Safe to call when no
     * key exists, which is the normal case.
     */
    bool start(const std::string& workingDir);

    /** Stops the worker and makes a final attempt to flush what is buffered. */
    void stop();

    [[nodiscard]] bool enabled() const { return m_enabled; }

    struct Stats {
        size_t accepted;  // lines taken from the log event
        size_t sent;      // lines the intake accepted
        size_t droppedOverflow;
        size_t droppedFailed;
        size_t postFailures;
    };

    /**
     * Last transport failure, or empty. Recorded rather than logged: nothing
     * on the worker path may call brls::Logger. Read it from the main thread.
     */
    [[nodiscard]] std::string lastError() const;

    [[nodiscard]] Stats stats() const;

  private:
    DatadogLogShipper() = default;
    ~DatadogLogShipper();

    DatadogLogShipper(const DatadogLogShipper&) = delete;
    DatadogLogShipper& operator=(const DatadogLogShipper&) = delete;

    void onLogLine(brls::LogLevel level, const std::string& line);
    void worker();
    bool post(const std::string& body);
    std::string buildPayload(const std::deque<std::string>& batch,
                             const std::deque<std::string>& levels) const;

    bool m_enabled = false;

    // Never logged, never included in any payload, never returned.
    std::string m_apiKey;
    std::string m_endpoint;
    std::string m_tags;
    std::string m_caBundlePath;

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::string> m_lines;
    std::deque<std::string> m_levels;
    std::atomic<bool> m_stopping { false };

    std::thread m_thread;
    brls::Event<brls::Logger::TimePoint, brls::LogLevel,
                std::string>::Subscription m_subscription;
    bool m_subscribed = false;

    mutable std::mutex m_statsMutex;
    Stats m_stats {};
    std::string m_lastError;
};
