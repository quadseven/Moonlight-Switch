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
    bool is_counter;
    bool used;
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
uint64_t g_startUnixNano = 0;

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
        /* A cumulative sum needs a start time that does not move, or a
         * backend cannot tell a restart from a counter going backwards. */
        g_startUnixNano = nowUnixNano();
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
            /* aggregationTemporality 2 is CUMULATIVE, and isMonotonic says
             * this only ever goes up, which is what lets a backend compute a
             * rate and recognise a restart rather than reporting a negative
             * spike. */
            out += "\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,";
            out += "\"dataPoints\":[{\"asInt\":\"";
            out += std::to_string(s.value);
            out += "\",\"startTimeUnixNano\":\"";
            out += std::to_string(g_startUnixNano);
            out += "\",\"timeUnixNano\":\"";
            out += std::to_string(now);
            out += "\"}]}}";
        } else {
            out += "\"gauge\":{\"dataPoints\":[{\"asInt\":\"";
            out += std::to_string(s.value);
            out += "\",\"timeUnixNano\":\"";
            out += std::to_string(now);
            out += "\"}]}}";
        }
    }

    out += "]}]}]}";
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
    }
}
