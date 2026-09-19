#include "OtlpLogExporter.hpp"
#include "OtlpTraceExporter.hpp"
#include "OtlpMetricsExporter.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>

#include <curl/curl.h>

#ifdef __SWITCH__
#include <switch.h>
#endif
#include <jansson.h>

namespace {

// The OTLP spec caps nothing itself, but backends do. Datadog's direct log
// intake rejects bodies over 5.1MiB, so stay well under on record count.
constexpr size_t kMaxBatchRecords = 200;
constexpr size_t kMaxBufferedRecords = 4000;
constexpr auto kFlushInterval = std::chrono::seconds(10);
constexpr long kPostTimeoutSeconds = 15;

/*
 * There is deliberately no thread_local anything on this path.
 *
 * An earlier version marked the worker thread with a `thread_local bool` so
 * that if the exporter ever logged it would not buffer its own output. On this
 * platform that crashed the process the instant the worker started: the
 * generated code reads the thread pointer and offsets into it,
 *
 *     mrs  x0, TPIDR_EL0
 *     add  x0, x0, #0x10
 *     strb w1, [x0]        <- data abort, TPIDR_EL0 was 0
 *
 * TPIDR_EL0 is zero on a thread spawned this way, so the very first statement
 * in worker() stored to null + 0x10. Do not reintroduce thread_local here.
 *
 * The invariant it guarded still holds, and holds by construction: nothing
 * reachable from onLogLine() or worker() calls brls::Logger. It has to stay
 * that way. borealis fires the log event from inside Logger::log() while
 * logMtx is held, so logging from the callback would deadlock on a non
 * recursive mutex, and a failed export that logged its own failure would
 * generate the record that causes the next failure. Counters only, read by
 * the main thread.
 */

/* No thread_local anywhere near this, for the reason documented above: a
 * thread_local store faults on a thread spawned on this platform. A syscall
 * per record is cheap and cannot go wrong the same way. */
uint64_t currentThreadId() {
#ifdef __SWITCH__
    u64 tid = 0;
    if (R_FAILED(svcGetThreadId(&tid, threadGetCurHandle()))) {
        return 0;
    }
    return tid;
#else
    return 0;
#endif
}

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

/** Parses the OTEL_EXPORTER_OTLP_HEADERS / OTEL_RESOURCE_ATTRIBUTES form. */
std::vector<std::pair<std::string, std::string>>
parseKeyValueList(const std::string& text) {
    std::vector<std::pair<std::string, std::string>> pairs;
    size_t position = 0;

    while (position <= text.size()) {
        const auto comma = text.find(',', position);
        const std::string item = trim(text.substr(
            position, comma == std::string::npos ? std::string::npos
                                                 : comma - position));
        if (!item.empty()) {
            const auto equals = item.find('=');
            if (equals != std::string::npos && equals > 0) {
                pairs.emplace_back(trim(item.substr(0, equals)),
                                   trim(item.substr(equals + 1)));
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        position = comma + 1;
    }

    return pairs;
}

/** Maps a borealis level onto the OpenTelemetry severity number scale. */
int severityNumber(brls::LogLevel level) {
    switch (level) {
    case brls::LogLevel::LOG_ERROR:
        return 17; // ERROR
    case brls::LogLevel::LOG_WARNING:
        return 13; // WARN
    case brls::LogLevel::LOG_DEBUG:
        return 5; // DEBUG
    case brls::LogLevel::LOG_VERBOSE:
        return 1; // TRACE
    case brls::LogLevel::LOG_INFO:
    default:
        return 9; // INFO
    }
}

const char* severityText(brls::LogLevel level) {
    switch (level) {
    case brls::LogLevel::LOG_ERROR:
        return "ERROR";
    case brls::LogLevel::LOG_WARNING:
        return "WARN";
    case brls::LogLevel::LOG_DEBUG:
        return "DEBUG";
    case brls::LogLevel::LOG_VERBOSE:
        return "TRACE";
    case brls::LogLevel::LOG_INFO:
    default:
        return "INFO";
    }
}

/** Builds one OTLP AnyValue-shaped string attribute. */
json_t* stringAttribute(const std::string& key, const std::string& value) {
    json_t* attribute = json_object();
    if (!attribute) {
        return nullptr;
    }
    json_t* anyValue = json_object();
    if (!anyValue) {
        json_decref(attribute);
        return nullptr;
    }
    json_object_set_new(anyValue, "stringValue", json_string(value.c_str()));
    json_object_set_new(attribute, "key", json_string(key.c_str()));
    json_object_set_new(attribute, "value", anyValue);
    return attribute;
}

size_t discardResponse(void*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

} // namespace

OtlpLogExporter& OtlpLogExporter::instance() {
    static OtlpLogExporter exporter;
    return exporter;
}

OtlpLogExporter::~OtlpLogExporter() { stop(); }

bool OtlpLogExporter::start(const std::string& workingDir) {
    if (m_enabled) {
        return true;
    }

    std::string endpoint = readFirstLine(workingDir + "/otel-endpoint");
    if (endpoint.empty()) {
        // Nothing configured is the normal case. Stay completely inert.
        return false;
    }

    // OTEL_EXPORTER_OTLP_ENDPOINT is a base that the signal path is appended
    // to. Accept a full logs URL too, so either form works.
    while (!endpoint.empty() && endpoint.back() == '/') {
        endpoint.pop_back();
    }
    if (endpoint.size() < 8 || endpoint.compare(0, 8, "https://") != 0) {
        // Refuse plaintext. Headers routinely carry a credential.
        return false;
    }
    if (endpoint.size() < 8 || endpoint.compare(endpoint.size() - 8, 8,
                                                "/v1/logs") != 0) {
        endpoint += "/v1/logs";
    }

    // A CA bundle is mandatory, not optional.
    //
    // libgamestream sets CURLOPT_SSL_VERIFYPEER 0 because GameStream hosts use
    // self signed certs, so the app ships no CA chain at all. That is fine for
    // a host on your own LAN. It is not fine here: these requests carry
    // credentials to an endpoint off the console, and without verification
    // anyone on the path could present their own certificate and collect them.
    // Require a bundle and stay inert without one rather than downgrading.
    const std::string caBundlePath = workingDir + "/cacert.pem";
    {
        std::ifstream caBundle(caBundlePath);
        if (!caBundle.is_open()) {
            return false;
        }
    }

    // Initialise curl here, on the main thread, before the worker exists.
    // curl_easy_init() lazily calls curl_global_init(), which is documented as
    // not thread safe, and http_init() in libgamestream only runs when
    // connecting to a host. Without this the worker could race it, or win and
    // leave curl on a TLS backend that libgamestream's curl_global_sslset() is
    // then too late to change. Picking the same backend keeps the end state
    // identical whichever gets there first.
#if LIBCURL_VERSION_NUM >= 0x075600
#ifdef USE_OPENSSL_CRYPTO
    curl_global_sslset(CURLSSLBACKEND_OPENSSL, NULL, NULL);
#elif USE_MBEDTLS_CRYPTO
    curl_global_sslset(CURLSSLBACKEND_MBEDTLS, NULL, NULL);
#endif
#endif
    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        return false;
    }

    m_endpoint = endpoint;
    m_caBundlePath = caBundlePath;
    m_headers = parseKeyValueList(readFirstLine(workingDir + "/otel-headers"));

    m_resourceAttributes =
        parseKeyValueList(readFirstLine(workingDir + "/otel-resource-attributes"));
    const bool hasServiceName =
        std::any_of(m_resourceAttributes.begin(), m_resourceAttributes.end(),
                    [](const std::pair<std::string, std::string>& attribute) {
                        return attribute.first == "service.name";
                    });
    if (!hasServiceName) {
        // service.name is the one resource attribute the spec requires.
        m_resourceAttributes.emplace_back("service.name", "moonlight-switch");
    }

    m_enabled = true;
    m_stopping = false;

    m_subscription = brls::Logger::getLogEvent()->subscribe(
        [this](brls::Logger::TimePoint when, brls::LogLevel level,
               const std::string& line) {
            // Checked here, not just relied on elsewhere: once stop() has
            // run there is nothing this should do, and the check has to
            // happen before touching `this` for anything else, since a
            // stopped exporter is exactly the state stop() leaves this
            // object in for the remainder of the process.
            if (!m_subscribed.load(std::memory_order_relaxed)) {
                return;
            }
            this->onLogLine(when, level, line);
        });
    m_subscribed = true;

    m_thread = std::thread([this] { this->worker(); });

    return true;
}

void OtlpLogExporter::setSuspended(bool suspended) {
    if (!m_enabled) {
        return;
    }

    const bool was = m_suspended.exchange(suspended);
    if (was == suspended) {
        return;
    }

    // Waking on resume matters: the worker may be part way through a ten
    // second wait, and the records buffered during the sleep should not sit
    // there for the remainder of it.
    if (!suspended) {
        m_wake.notify_all();
    }
}

void OtlpLogExporter::stop() {
    if (!m_enabled) {
        return;
    }

    /*
     * Deliberately does NOT call brls::Logger::getLogEvent()->unsubscribe()
     * here anymore. brls::Event keeps its callbacks in a plain std::list
     * with no lock of its own (extern/borealis/library/include/borealis/
     * core/event.hpp): unsubscribe() erases an iterator from that list with
     * nothing guarding it. brls::Logger::log() fires the same event while
     * holding its own private logMtx (extern/borealis/.../core/logger.hpp),
     * but logMtx is private to Logger -- there is no way to take the same
     * lock from out here. Any thread logging at the moment stop() ran
     * (the detached termination thread this exporter exists to diagnose is
     * exactly such a thread) could be mid-iteration over that list while
     * this erased out from under it: a teardown-time crash indistinguishable
     * from the one under investigation, possibly caused by the tooling
     * added to diagnose it.
     *
     * Instead the subscription is left in place for the rest of the
     * process, and the lambda in start() checks m_subscribed before doing
     * anything. onLogLine()/enqueue() stay safe to call after stop(): this
     * is a process-lifetime singleton, m_mutex is still valid, and a record
     * pushed after stop() just sits unread in a bounded buffer until the
     * process exits, which costs nothing next to corrupting brls::Event's
     * internal list.
     */
    m_subscribed = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stopping = true;
    }
    m_wake.notify_all();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    m_enabled = false;
    // Do not keep credentials in memory once exporting is off.
    m_headers.clear();

    // Matches the curl_global_init() in start(). curl refcounts these, so
    // libgamestream's own init is unaffected by this one going away.
    curl_global_cleanup();
}

void OtlpLogExporter::enqueue(Record record) {
    bool overflowed = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_records.size() >= kMaxBufferedRecords) {
            m_records.pop_front();
            overflowed = true;
        }
        m_records.push_back(std::move(record));
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

    // Do not wake the worker per record. It flushes on its own interval, which
    // keeps a 60fps stream from turning into a request per frame.
}

void OtlpLogExporter::onLogLine(brls::Logger::TimePoint when,
                                brls::LogLevel level, const std::string& line) {
    Record record;
    record.timeUnixNano = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            when.time_since_epoch())
            .count());
    record.severityNumber = severityNumber(level);
    record.severityText = severityText(level);
    record.body = line;
    record.threadId = currentThreadId();
    enqueue(std::move(record));
}

void OtlpLogExporter::logRaw(const std::string& line, bool fromStderr) {
    // Dropped rather than buffered when export is off, so a build with no
    // endpoint configured pays nothing for having capture compiled in.
    if (!m_enabled) {
        return;
    }

    Record record;
    record.timeUnixNano = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    // A raw write carries no severity. stderr is reported one step up from
    // stdout because callers overwhelmingly use it for failures, but neither
    // is a real level and log.source says where the line actually came from.
    record.severityNumber = fromStderr ? 13 : 9;
    record.severityText = fromStderr ? "WARN" : "INFO";
    record.body = line;
    record.source = fromStderr ? "stderr" : "stdout";
    record.threadId = currentThreadId();
    enqueue(std::move(record));
}

void OtlpLogExporter::worker() {
    for (;;) {
        std::deque<Record> batch;

        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_wake.wait_for(lock, kFlushInterval,
                            [this] { return m_stopping.load(); });

            // Park while the console is suspended rather than taking a batch
            // we would then try to post over a network the OS is dismantling.
            // Checked here, inside the lock, so a resume that arrives while
            // we are waiting is seen immediately.
            if (m_suspended && !m_stopping) {
                continue;
            }

            const size_t take = std::min(m_records.size(), kMaxBatchRecords);
            for (size_t i = 0; i < take; i++) {
                batch.push_back(std::move(m_records.front()));
                m_records.pop_front();
            }

            /*
             * Leaving on an empty log batch here discards whatever spans and
             * metrics are still buffered, and this is the last pass before the
             * worker exits.
             *
             * Same mistake as shipping spans after the empty-batch check
             * further down, one level up and with worse consequences. Set
             * otel-log-level to error, have a session that logs no errors, and
             * every span produced during teardown is dropped on the way out:
             * the journal on the card holds them and the backend never sees
             * them. It also fires on an ordinary shutdown, where the final log
             * batch ships, the loop re-enters, finds nothing, and leaves
             * without flushing what teardown produced after it.
             *
             * The flag is cleared below once the other two signals have had
             * their turn, so this still terminates after exactly one more pass.
             */
            if (batch.empty() && m_stopping) {
                if (m_finalFlushDone) {
                    return;
                }
                m_finalFlushDone = true;
            }
        }

        if (m_suspended && !m_stopping) {
            // Focus was lost after the batch was taken. Put it back rather
            // than posting into a network that is going away; the records are
            // still wanted, just not now. Spans and metrics are skipped for
            // the same reason, by falling through to the next wait.
            std::lock_guard<std::mutex> lock(m_mutex);
            while (!batch.empty()) {
                m_records.push_front(std::move(batch.back()));
                batch.pop_back();
            }
            continue;
        }

        /* Spans ride the same worker rather than getting a thread of their
         * own. That is not just thrift: this loop already parks while the
         * console is suspended, and a trace POST left in curl across a sleep
         * would hang the process exactly the way the log POST used to. One
         * place that knows when the network is safe to touch is worth more
         * than a second exporter that has to learn it again.
         *
         * Shipped before the empty-batch check below, not after. Sharing a
         * worker was meant to share the network's schedule, not to make spans
         * conditional on log traffic: with this after the check, an interval
         * where nothing logged shipped no spans and no metrics either, and a
         * quiet stretch is exactly when a stream is running normally. */
        if (OtlpTraceExporter::instance().enabled()) {
            const std::string spans = OtlpTraceExporter::instance().takePayload();
            if (!spans.empty()) {
                /* takePayload() only renders; the spans stay buffered until
                 * confirmDelivered() runs, and that only happens here, only
                 * on success. A failed POST -- a 502, same as any other --
                 * leaves them in place to render again next pass instead of
                 * being discarded with the rest of the batch that never
                 * shipped. */
                if (post(OtlpTraceExporter::instance().endpoint(), spans)) {
                    OtlpTraceExporter::instance().confirmDelivered();
                } else {
                    std::lock_guard<std::mutex> statsLock(m_statsMutex);
                    m_stats.tracePostFailures++;
                }
            }
        }

        /* Metrics on the same schedule. Unlike logs and spans these are not
         * drained: a gauge reports its current value every interval, so a
         * count that is wrong stays visible rather than appearing once. */
        if (OtlpMetricsExporter::instance().enabled()) {
            const std::string m = OtlpMetricsExporter::instance().takePayload();
            if (!m.empty()) {
                /* Same shape as the span path above: takePayload() only
                 * renders, commitPayload() moves the counter baselines and
                 * gauge peaks forward, and that only happens on a
                 * successful POST. Skipping it on failure means the same
                 * deltas render again next pass instead of vanishing. */
                if (post(OtlpMetricsExporter::instance().endpoint(), m)) {
                    OtlpMetricsExporter::instance().commitPayload();
                } else {
                    std::lock_guard<std::mutex> statsLock(m_statsMutex);
                    m_stats.metricPostFailures++;
                }
            }
        }

        if (batch.empty()) {
            continue;
        }

        const std::string payload = buildPayload(batch);
        const bool ok = !payload.empty() && post(m_endpoint, payload);

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
                // and starves the newer records describing what happened.
                m_stats.droppedFailed += batch.size();
                m_stats.postFailures++;
            }
        }

        if (m_stopping) {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_records.empty()) {
                return;
            }
        }
    }
}

std::string
OtlpLogExporter::buildPayload(const std::deque<Record>& batch) const {
    // OTLP/HTTP JSON: one resourceLogs entry, one scopeLogs entry, and the
    // batch as logRecords. Built with jansson so escaping is correct rather
    // than hand rolled.
    json_t* root = json_object();
    json_t* resourceLogsArray = json_array();
    json_t* resourceLogs = json_object();
    json_t* resource = json_object();
    json_t* resourceAttributes = json_array();
    json_t* scopeLogsArray = json_array();
    json_t* scopeLogs = json_object();
    json_t* logRecords = json_array();

    if (!root || !resourceLogsArray || !resourceLogs || !resource ||
        !resourceAttributes || !scopeLogsArray || !scopeLogs || !logRecords) {
        json_decref(root);
        json_decref(resourceLogsArray);
        json_decref(resourceLogs);
        json_decref(resource);
        json_decref(resourceAttributes);
        json_decref(scopeLogsArray);
        json_decref(scopeLogs);
        json_decref(logRecords);
        return "";
    }

    for (const auto& attribute : m_resourceAttributes) {
        if (json_t* entry = stringAttribute(attribute.first, attribute.second)) {
            json_array_append_new(resourceAttributes, entry);
        }
    }
    json_object_set_new(resource, "attributes", resourceAttributes);

    for (const Record& record : batch) {
        json_t* logRecord = json_object();
        if (!logRecord) {
            continue;
        }
        // timeUnixNano is a string in the OTLP JSON mapping: it is a uint64
        // and JSON numbers cannot carry that range safely.
        json_object_set_new(
            logRecord, "timeUnixNano",
            json_string(std::to_string(record.timeUnixNano).c_str()));
        json_object_set_new(logRecord, "severityNumber",
                            json_integer(record.severityNumber));
        json_object_set_new(logRecord, "severityText",
                            json_string(record.severityText.c_str()));

        json_t* body = json_object();
        if (body) {
            json_object_set_new(body, "stringValue",
                                json_string(record.body.c_str()));
            json_object_set_new(logRecord, "body", body);
        }

        // Only raw writes carry a source. Emitting the attribute
        // unconditionally would put an empty string on every ordinary line
        // and cost payload size on the hottest path for nothing.
        if (!record.source.empty() || record.threadId) {
            json_t* recordAttributes = json_array();
            if (recordAttributes) {
                if (!record.source.empty()) {
                    if (json_t* entry =
                            stringAttribute("log.source", record.source)) {
                        json_array_append_new(recordAttributes, entry);
                    }
                }
                if (record.threadId) {
                    // String, not a number: this is a u64 and JSON cannot
                    // carry that range without losing the low bits.
                    if (json_t* entry = stringAttribute(
                            "thread.id", std::to_string(record.threadId))) {
                        json_array_append_new(recordAttributes, entry);
                    }
                }
                if (json_array_size(recordAttributes) > 0) {
                    json_object_set_new(logRecord, "attributes",
                                        recordAttributes);
                } else {
                    json_decref(recordAttributes);
                }
            }
        }

        json_array_append_new(logRecords, logRecord);
    }

    json_object_set_new(scopeLogs, "logRecords", logRecords);
    json_array_append_new(scopeLogsArray, scopeLogs);

    json_object_set_new(resourceLogs, "resource", resource);
    json_object_set_new(resourceLogs, "scopeLogs", scopeLogsArray);
    json_array_append_new(resourceLogsArray, resourceLogs);
    json_object_set_new(root, "resourceLogs", resourceLogsArray);

    char* dumped = json_dumps(root, JSON_COMPACT);
    json_decref(root);

    if (!dumped) {
        return "";
    }

    std::string payload(dumped);
    free(dumped);
    return payload;
}

bool OtlpLogExporter::post(const std::string& url, const std::string& body) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    for (const auto& header : m_headers) {
        // Values may be credentials. They go on the wire and nowhere else.
        const std::string line = header.first + ": " + header.second;
        headers = curl_slist_append(headers, line.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kPostTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kPostTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardResponse);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, m_caBundlePath.c_str());

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    if (result == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    const bool ok = result == CURLE_OK && status >= 200 && status < 300;
    if (!ok) {
        // Recorded, never logged from here. The main thread reports it.
        std::lock_guard<std::mutex> lock(m_statsMutex);
        if (result != CURLE_OK) {
            m_lastError = std::string("curl: ") + curl_easy_strerror(result);
        } else {
            m_lastError = "endpoint returned HTTP " + std::to_string(status);
        }
    }

    return ok;
}

OtlpLogExporter::Stats OtlpLogExporter::stats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

std::string OtlpLogExporter::lastError() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_lastError;
}
