#include "DatadogLogShipper.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>

#include <curl/curl.h>
#include <jansson.h>

namespace {

// Datadog rejects batches over 5MB and over 1000 entries. Stay well under.
constexpr size_t kMaxBatchLines = 200;
constexpr size_t kMaxBufferedLines = 4000;
constexpr auto kFlushInterval = std::chrono::seconds(10);
constexpr long kPostTimeoutSeconds = 10;

/**
 * Guards against the shipper feeding itself.
 *
 * borealis fires the log event from inside Logger::log() while logMtx is held,
 * so anything on this path that logs would deadlock on a non recursive mutex.
 * The worker thread is separate and would not deadlock, but a failing POST
 * that logged its own failure would generate the line that causes the next
 * failure. Neither path logs, and this flag makes that structural rather than
 * a rule someone has to remember.
 */
thread_local bool t_insideShipper = false;

struct ShipperScope {
    ShipperScope() { t_insideShipper = true; }
    ~ShipperScope() { t_insideShipper = false; }
};

std::string trim(const std::string& value) {
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

std::string readFirstLine(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    std::string line;
    std::getline(file, line);
    return trim(line);
}

const char* levelToStatus(brls::LogLevel level) {
    switch (level) {
    case brls::LogLevel::LOG_ERROR:
        return "error";
    case brls::LogLevel::LOG_WARNING:
        return "warn";
    case brls::LogLevel::LOG_DEBUG:
    case brls::LogLevel::LOG_VERBOSE:
        return "debug";
    case brls::LogLevel::LOG_INFO:
    default:
        return "info";
    }
}

size_t discardResponse(void*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

} // namespace

DatadogLogShipper& DatadogLogShipper::instance() {
    static DatadogLogShipper shipper;
    return shipper;
}

DatadogLogShipper::~DatadogLogShipper() { stop(); }

bool DatadogLogShipper::start(const std::string& workingDir) {
    if (m_enabled) {
        return true;
    }

    const std::string key = readFirstLine(workingDir + "/datadog.key");
    if (key.empty()) {
        // No key file is the normal case. Stay completely inert.
        return false;
    }

    // Optional, so EU and US3/US5 accounts work without a rebuild.
    std::string site = readFirstLine(workingDir + "/datadog.site");
    if (site.empty()) {
        site = "datadoghq.com";
    }

    std::string tags = readFirstLine(workingDir + "/datadog.tags");
    if (tags.empty()) {
        tags = "app:moonlight-switch,platform:switch";
    }

    m_apiKey = key;
    m_endpoint = "https://http-intake.logs." + site + "/api/v2/logs";
    m_tags = tags;
    m_enabled = true;
    m_stopping = false;

    m_subscription = brls::Logger::getLogEvent()->subscribe(
        [this](brls::Logger::TimePoint, brls::LogLevel level,
               const std::string& line) { this->onLogLine(level, line); });
    m_subscribed = true;

    m_thread = std::thread([this] { this->worker(); });

    return true;
}

void DatadogLogShipper::stop() {
    if (!m_enabled) {
        return;
    }

    if (m_subscribed) {
        brls::Logger::getLogEvent()->unsubscribe(m_subscription);
        m_subscribed = false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_enabled = false;
    // Do not keep the key in memory once shipping is off.
    m_apiKey.clear();
}

void DatadogLogShipper::onLogLine(brls::LogLevel level,
                                  const std::string& line) {
    if (t_insideShipper) {
        return;
    }

    bool overflowed = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_lines.size() >= kMaxBufferedLines) {
            m_lines.pop_front();
            m_levels.pop_front();
            overflowed = true;
        }
        m_lines.push_back(line);
        m_levels.emplace_back(levelToStatus(level));
    }

    // m_statsMutex is never taken while m_mutex is held. The worker needs both
    // as well, and taking them in opposite orders on the two threads would
    // deadlock.
    {
        std::lock_guard<std::mutex> statsLock(m_statsMutex);
        m_stats.accepted++;
        if (overflowed) {
            m_stats.droppedOverflow++;
        }
    }

    // Do not wake the worker per line. It flushes on its own interval, which
    // keeps a 60fps stream from turning into a POST per frame.
}

void DatadogLogShipper::worker() {
    ShipperScope scope;

    for (;;) {
        std::deque<std::string> batch;
        std::deque<std::string> levels;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, kFlushInterval,
                            [this] { return m_stopping.load(); });

            const size_t take = std::min(m_lines.size(), kMaxBatchLines);
            for (size_t i = 0; i < take; i++) {
                batch.push_back(std::move(m_lines.front()));
                levels.push_back(std::move(m_levels.front()));
                m_lines.pop_front();
                m_levels.pop_front();
            }

            if (batch.empty() && m_stopping) {
                return;
            }
        }

        if (batch.empty()) {
            continue;
        }

        const std::string payload = buildPayload(batch, levels);
        const bool ok = !payload.empty() && post(payload);

        // Scoped so m_statsMutex is released before m_mutex is taken below.
        // onLogLine() takes m_mutex first, so holding both here in the other
        // order would deadlock against it.
        {
            std::lock_guard<std::mutex> statsLock(m_statsMutex);
            if (ok) {
                m_stats.sent += batch.size();
            } else {
                // Dropped rather than requeued. Requeuing a batch that failed
                // because the network is down grows the buffer without bound
                // and starves the newer lines describing what happened.
                m_stats.droppedFailed += batch.size();
                m_stats.postFailures++;
            }
        }

        if (m_stopping) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_lines.empty()) {
                return;
            }
        }
    }
}

std::string DatadogLogShipper::buildPayload(
    const std::deque<std::string>& batch,
    const std::deque<std::string>& levels) const {
    json_t* array = json_array();
    if (!array) {
        return "";
    }

    for (size_t i = 0; i < batch.size(); i++) {
        json_t* entry = json_object();
        if (!entry) {
            continue;
        }
        json_object_set_new(entry, "ddsource", json_string("moonlight-switch"));
        json_object_set_new(entry, "service", json_string("moonlight-switch"));
        json_object_set_new(entry, "hostname", json_string("nintendo-switch"));
        json_object_set_new(entry, "ddtags", json_string(m_tags.c_str()));
        json_object_set_new(entry, "status",
                            json_string(i < levels.size() ? levels[i].c_str()
                                                          : "info"));
        json_object_set_new(entry, "message", json_string(batch[i].c_str()));
        json_array_append_new(array, entry);
    }

    char* dumped = json_dumps(array, JSON_COMPACT);
    json_decref(array);

    if (!dumped) {
        return "";
    }

    std::string payload(dumped);
    free(dumped);
    return payload;
}

bool DatadogLogShipper::post(const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    // The key goes on the wire and nowhere else: not into a log line, not
    // into the payload, not into the stats.
    const std::string keyHeader = "DD-API-KEY: " + m_apiKey;
    headers = curl_slist_append(headers, keyHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, m_endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kPostTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kPostTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return result == CURLE_OK && status >= 200 && status < 300;
}

DatadogLogShipper::Stats DatadogLogShipper::stats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}
