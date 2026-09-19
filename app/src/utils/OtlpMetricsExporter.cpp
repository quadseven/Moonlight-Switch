#include "OtlpMetricsExporter.hpp"

#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

namespace {

/*
 * A fixed table rather than a map. Metric names in this app are string
 * literals decided at compile time, there are a couple of dozen at most, and
 * the update path runs from the video and input threads. Allocating or
 * rehashing there to record that a counter went up would cost more than the
 * thing being measured.
 */
constexpr size_t kMaxSeries = 32;

struct Series {
    const char* name;   /* literal, compared by pointer first then by text */
    int64_t value;
    /* Counters are reported as the change since the previous export, not as
     * a running total, so the amount already sent has to be remembered. */
    int64_t exported;
    /*
     * Highest value seen since the last export.
     *
     * A gauge is sampled once per flush, and the flush interval is ten
     * seconds. The two gauges that matter most are not observable that way:
     * frames_in_flight is raised and lowered around a single frame fetch,
     * microseconds apart, and listop_inflight is held for about a hundred
     * milliseconds. Sampling either every ten seconds reads zero essentially
     * always.
     *
     * That is worse than having no metric. "listop_inflight was never above 1"
     * is the conclusion the instrument exists to support, and a point sample
     * produces exactly that reading whether or not the collision happened. The
     * peak is what actually answers the question, because it cannot be missed
     * by looking at the wrong instant.
     */
    int64_t peak;
    bool is_counter;
    bool used;
    /* Set by takePayload() for every series it renders, cleared by
     * commitPayload(). Marks that `pending_exported`/`pending_peak` below
     * hold values waiting to become the real baseline once the POST that
     * carried them is confirmed to have succeeded. */
    bool pending_commit = false;
    /* Counter: the value snapshotted at takePayload() time, i.e. what
     * `exported` becomes once commitPayload() runs. */
    int64_t pending_exported = 0;
    /* Gauge: the peak-reset target snapshotted at takePayload() time, i.e.
     * what `peak` becomes once commitPayload() runs. */
    int64_t pending_peak = 0;
};

std::mutex g_mutex;
Series g_series[kMaxSeries];

Series* findOrCreate(const char* name, bool is_counter) {
    size_t free_slot = kMaxSeries;
    for (size_t i = 0; i < kMaxSeries; i++) {
        if (!g_series[i].used) {
            if (free_slot == kMaxSeries) {
                free_slot = i;
            }
            continue;
        }
        /* Pointer equality hits for every call site using the same literal,
         * which is all of them; strcmp is the fallback for a caller that
         * built the name some other way. */
        if (g_series[i].name == name || std::strcmp(g_series[i].name, name) == 0) {
            return &g_series[i];
        }
    }
    if (free_slot == kMaxSeries) {
        /* Full. Dropping a new series is better than evicting one already
         * being watched, and the cap is far above what this app registers. */
        return nullptr;
    }
    g_series[free_slot].name = name;
    g_series[free_slot].value = 0;
    g_series[free_slot].exported = 0;
    g_series[free_slot].peak = 0;
    g_series[free_slot].is_counter = is_counter;
    g_series[free_slot].used = true;
    return &g_series[free_slot];
}

uint64_t nowUnixNano() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void appendEscaped(std::string& out, const char* v) {
    for (const char* p = v; *p; p++) {
        switch (*p) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:   out += *p;
        }
    }
}

bool g_started = false;
/* Start of the interval the next export will describe. For delta sums this
 * moves forward on every flush, because each export covers only the window
 * since the last one rather than all of time. Advanced only by
 * commitPayload(), not by takePayload(): see the comment there. */
uint64_t g_intervalStartUnixNano = 0;
/* End of the interval takePayload() most recently rendered -- what
 * g_intervalStartUnixNano becomes once commitPayload() confirms that
 * payload was delivered. */
uint64_t g_pendingIntervalEndUnixNano = 0;

}  // namespace

OtlpMetricsExporter& OtlpMetricsExporter::instance() {
    static OtlpMetricsExporter e;
    return e;
}

bool OtlpMetricsExporter::start(const std::string& workingDir) {
    std::ifstream f(workingDir + "/otel-endpoint");
    if (!f.good()) {
        return false;
    }
    std::string endpoint;
    std::getline(f, endpoint);

    const auto begin = endpoint.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return false;
    }
    const auto last = endpoint.find_last_not_of(" \t\r\n");
    endpoint = endpoint.substr(begin, last - begin + 1);

    const std::string logs = "/v1/logs";
    if (endpoint.size() >= logs.size() &&
        endpoint.compare(endpoint.size() - logs.size(), logs.size(), logs) == 0) {
        endpoint = endpoint.substr(0, endpoint.size() - logs.size());
    }
    const std::string metrics = "/v1/metrics";
    if (endpoint.size() < metrics.size() ||
        endpoint.compare(endpoint.size() - metrics.size(), metrics.size(),
                         metrics) != 0) {
        endpoint += metrics;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        /* Opens the first delta interval. Every export closes one and opens
         * the next, so this is the only time it is set from outside. */
        g_intervalStartUnixNano = nowUnixNano();
        g_started = true;
    }

    m_endpoint = endpoint;
    m_enabled = true;
    return true;
}

void OtlpMetricsExporter::stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    m_enabled = false;
}

void OtlpMetricsExporter::gauge(const char* name, int64_t value) {
    if (!m_enabled || !name) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Series* s = findOrCreate(name, false)) {
        s->value = value;
        if (value > s->peak) {
            s->peak = value;
        }
    }
}

void OtlpMetricsExporter::add(const char* name, int64_t delta) {
    if (!m_enabled || !name) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Series* s = findOrCreate(name, true)) {
        s->value += delta;
    }
}

std::string OtlpMetricsExporter::takePayload() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_started) {
        return "";
    }

    bool any = false;
    for (size_t i = 0; i < kMaxSeries; i++) {
        if (g_series[i].used) {
            any = true;
            break;
        }
    }
    if (!any) {
        return "";
    }

    const uint64_t now = nowUnixNano();
    std::string out;
    out.reserve(2048);
    out += "{\"resourceMetrics\":[{\"resource\":{\"attributes\":[";
    out += "{\"key\":\"service.name\",\"value\":{\"stringValue\":\"moonlight-switch\"}},";
    out += "{\"key\":\"host.name\",\"value\":{\"stringValue\":\"nintendo-switch\"}}";
    out += "]},\"scopeMetrics\":[{\"metrics\":[";

    bool first = true;
    for (size_t i = 0; i < kMaxSeries; i++) {
        const Series& s = g_series[i];
        if (!s.used) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;

        out += "{\"name\":\"";
        appendEscaped(out, s.name);
        out += "\",\"unit\":\"1\",";

        /* Values as strings: they are int64 and JSON numbers cannot carry
         * that range without losing the low bits, the same reason the
         * timestamps are strings. */
        if (s.is_counter) {
            /*
             * aggregationTemporality 1 is DELTA: this data point is the change
             * since the previous export, and the interval is start..time.
             *
             * Cumulative would be the more usual choice and is what I reached
             * for first. Datadog's OTLP metrics intake rejects it outright,
             * which is silent from here because a rejected POST only shows up
             * in the exporter's own error counter. Nothing arrived for a whole
             * evening because of it.
             */
            out += "\"sum\":{\"aggregationTemporality\":1,\"isMonotonic\":true,";
            out += "\"dataPoints\":[{\"asInt\":\"";
            out += std::to_string(s.value - s.exported);
            out += "\",\"startTimeUnixNano\":\"";
            out += std::to_string(g_intervalStartUnixNano);
            out += "\",\"timeUnixNano\":\"";
            out += std::to_string(now);
            out += "\"}]}}";
        } else {
            out += "\"gauge\":{\"dataPoints\":[{\"asInt\":\"";
            out += std::to_string(s.value);
            out += "\",\"timeUnixNano\":\"";
            out += std::to_string(now);
            out += "\"}]}}";

            /* And the peak, as its own series. The instantaneous value answers
             * "what is it now", which for a count held across a hundred
             * millisecond teardown is almost always zero when the ten second
             * flush happens to look. The peak answers "did it ever", which is
             * the question every one of these gauges was added to settle. */
            out += ",{\"name\":\"";
            appendEscaped(out, s.name);
            out += ".peak\",\"unit\":\"1\",\"gauge\":{\"dataPoints\":[{\"asInt\":\"";
            out += std::to_string(s.peak);
            out += "\",\"timeUnixNano\":\"";
            out += std::to_string(now);
            out += "\"}]}}";
        }
    }

    out += "]}]}]}";

    /*
     * Recorded, not committed. This used to advance `exported` and `peak`
     * right here, before the caller had even tried to POST the string just
     * built -- so a single rejected POST (a 502, same as any other) spent
     * the delta anyway, and the next takePayload() computed against the new
     * baseline as if this one had shipped. The counter increments in it were
     * gone for good, with nothing in any log to say so.
     *
     * The fix is to only ever move the baseline in commitPayload(), which
     * the caller must call after confirming the POST for this exact payload
     * succeeded. Until then the pending values sit here and a retried
     * takePayload() (there was no confirm, so nothing has changed) renders
     * the identical deltas again rather than silently dropping them.
     */
    for (size_t i = 0; i < kMaxSeries; i++) {
        if (!g_series[i].used) {
            continue;
        }
        if (g_series[i].is_counter) {
            g_series[i].pending_exported = g_series[i].value;
        }
        /* The peak describes one interval, so its reset target is wherever
         * the value actually is rather than zero. Resetting to zero would
         * report a peak below the current value for anything still held open
         * across a flush, which is the normal state of a live count. */
        g_series[i].pending_peak = g_series[i].value;
        g_series[i].pending_commit = true;
    }
    g_pendingIntervalEndUnixNano = now;

    return out;
}

void OtlpMetricsExporter::commitPayload() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (size_t i = 0; i < kMaxSeries; i++) {
        Series& s = g_series[i];
        if (!s.used || !s.pending_commit) {
            continue;
        }
        if (s.is_counter) {
            s.exported = s.pending_exported;
        }
        s.peak = s.pending_peak;
        s.pending_commit = false;
    }
    g_intervalStartUnixNano = g_pendingIntervalEndUnixNano;
}

std::string otlp_metrics_snapshot() {
    /*
     * Every gauge, as JSON fields, for embedding in a line of the span journal.
     *
     * Metrics only leave this process over the network, on a worker that parks
     * itself the moment the console loses focus and dies with the process. So
     * during a sleep, and at the moment of a crash, every gauge is unreadable:
     * the numbers built to identify the defect are dark exactly when it fires.
     *
     * The journal is the one store that survives. Writing the gauges into a
     * breadcrumb costs a few dozen bytes and makes listop_inflight readable
     * from the card afterwards, with no working network at any point.
     */
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string out;
    for (size_t i = 0; i < kMaxSeries; i++) {
        const Series& s = g_series[i];
        if (!s.used) {
            continue;
        }
        out += ",\"m.";
        appendEscaped(out, s.name);
        out += "\":";
        out += std::to_string(s.value);
        if (!s.is_counter && s.peak != s.value) {
            out += ",\"m.";
            appendEscaped(out, s.name);
            out += ".peak\":";
            out += std::to_string(s.peak);
        }
    }
    return out;
}

OtlpGaugeScope::OtlpGaugeScope(const char* name) : m_name(name) {
    otlp_metric_count_add(m_name, 1);
}

OtlpGaugeScope::~OtlpGaugeScope() { otlp_metric_count_add(m_name, -1); }

void otlp_metric_count_add(const char* name, int64_t delta) {
    if (!OtlpMetricsExporter::instance().enabled() || !name) {
        return;
    }
    /* A live count is a gauge, not a counter: it goes down as well as up, and
     * reporting it as monotonic would make every release look like a restart.
     * Held in the same table, so the read-modify-write is under one lock. */
    std::lock_guard<std::mutex> lock(g_mutex);
    if (Series* s = findOrCreate(name, false)) {
        s->value += delta;
        if (s->value > s->peak) {
            s->peak = s->value;
        }
    }
}
