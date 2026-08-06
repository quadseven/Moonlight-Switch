#include "OtlpTraceExporter.hpp"

#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {

/* Bounded like the log ring, and for the same reason: a console has no swap
 * and a stuck exporter must not grow without limit. Spans are larger than log
 * records, so the cap is lower. */
constexpr size_t kMaxBufferedSpans = 1024;

struct FinishedSpan {
    uint64_t traceHi;
    uint64_t traceLo;
    uint64_t spanId;
    uint64_t parentId;
    uint64_t startUnixNano;
    uint64_t endUnixNano;
    uint64_t threadId;
    std::string name;
    std::string error;
    std::vector<std::pair<std::string, std::string>> attrs;
};

std::mutex g_mutex;
std::deque<FinishedSpan> g_spans;

/* The session-wide root. Read from any thread, including the detached one
 * that delivers connection_terminated, so it is guarded by g_mutex rather
 * than assumed to be main-thread-only. */
OtlpTraceExporter::Span g_sessionRoot;
const char* g_sessionName = nullptr;
size_t g_started = 0;
size_t g_ended = 0;
size_t g_dropped = 0;

/*
 * The wall clock here only advances in whole seconds, so it is anchored once
 * and everything after is derived from the monotonic tick counter. Re-taking
 * the anchor would let spans move relative to each other if the RTC is
 * adjusted, so it is written once and only read afterwards.
 */
uint64_t g_anchorUnixNano = 0;
uint64_t g_anchorTick = 0;

/*
 * Spans are also appended to a file on the SD card as they complete.
 *
 * This is not redundancy for its own sake. The crash this exists for took the
 * whole console down, not just the process: nothing was written to
 * crash_reports or fatal_errors, and the exporter died with up to a full flush
 * interval of spans still in memory. Nothing that leaves over the network can
 * survive that.
 *
 * The log file is NOT a reliable comparison, whatever an earlier version of
 * this comment claimed: borealis only fflush()es per line under __MINGW32__,
 * so on this platform its tail is a stdio buffer boundary rather than the
 * moment the process stopped. main.cpp now sets _IOLBF to make it honest.
 *
 * This journal does not depend on that. One JSON object per line, fwrite then
 * fflush, on completion of every span.
 * After a hang the card still has the trace right up to the moment the system
 * stopped, which is the only part anyone wants.
 *
 * This is affordable only because the instrumented spans are lifecycle events,
 * a handful per session. Do not put a span on a per-frame path without turning
 * this off first; an fflush per frame would be a bottleneck rather than a
 * diagnostic.
 */
std::FILE* g_journal = nullptr;

uint64_t nowTick() {
#ifdef __SWITCH__
    return armGetSystemTick();
#else
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
#endif
}

uint64_t ticksToNs(uint64_t ticks) {
#ifdef __SWITCH__
    return armTicksToNs(ticks);
#else
    return ticks;
#endif
}

uint64_t tickToUnixNano(uint64_t tick) {
    if (!g_anchorTick) {
        return 0;
    }
    if (tick < g_anchorTick) {
        return g_anchorUnixNano;
    }
    return g_anchorUnixNano + ticksToNs(tick - g_anchorTick);
}

uint64_t randomU64() {
#ifdef __SWITCH__
    return randomGet64();
#else
    static uint64_t x = 0x2545F4914F6CDD1DULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return x;
#endif
}

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

void appendHex64(std::string& out, uint64_t v) {
    static const char* digits = "0123456789abcdef";
    for (int shift = 60; shift >= 0; shift -= 4) {
        out += digits[(v >> shift) & 0xF];
    }
}

void appendEscaped(std::string& out, const std::string& v) {
    for (char c : v) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
}

void appendStringAttr(std::string& out, const std::string& k,
                      const std::string& v) {
    out += "{\"key\":\"";
    appendEscaped(out, k);
    out += "\",\"value\":{\"stringValue\":\"";
    appendEscaped(out, v);
    out += "\"}}";
}

void journalSpan(const FinishedSpan& f) {
    if (!g_journal) {
        return;
    }
    std::string line = "{\"name\":\"";
    appendEscaped(line, f.name);
    line += "\",\"start\":";
    line += std::to_string(f.startUnixNano);
    line += ",\"end\":";
    line += std::to_string(f.endUnixNano);
    line += ",\"thread\":";
    line += std::to_string(f.threadId);
    line += ",\"trace\":\"";
    appendHex64(line, f.traceHi);
    appendHex64(line, f.traceLo);
    line += "\",\"span\":\"";
    appendHex64(line, f.spanId);
    line += "\"";
    if (f.parentId) {
        line += ",\"parent\":\"";
        appendHex64(line, f.parentId);
        line += "\"";
    }
    if (!f.error.empty()) {
        line += ",\"error\":\"";
        appendEscaped(line, f.error);
        line += "\"";
    }
    for (const auto& kv : f.attrs) {
        line += ",\"";
        appendEscaped(line, kv.first);
        line += "\":\"";
        appendEscaped(line, kv.second);
        line += "\"";
    }
    line += "}\n";

    std::fwrite(line.data(), 1, line.size(), g_journal);
    /* The whole point. Without this the tail of the trace sits in a stdio
     * buffer and dies with the console. */
    std::fflush(g_journal);
}

}  // namespace

void otlp_trace_mark(const char* name) {
    /*
     * A span is only journalled when it ends, so a span the process dies inside
     * writes nothing at all. That makes "died halfway through teardown" and
     * "was killed from outside before teardown started" produce byte-identical
     * journals, and those two want opposite investigations.
     *
     * A mark is written the moment it is reached. Passing one and never
     * reaching the next is the evidence a span cannot give.
     *
     * Deliberately not taking the exporter lock: this runs on paths that are
     * already unwinding, possibly after another thread has been killed while
     * holding it, and a breadcrumb that can deadlock is worse than none. The
     * write is a single fwrite of one line, which stdio serialises internally.
     */
    if (!g_journal || !name) {
        return;
    }
    std::string line = "{\"mark\":\"";
    appendEscaped(line, name);
    line += "\",\"time\":";
    line += std::to_string(tickToUnixNano(nowTick()));
    line += ",\"thread\":";
    line += std::to_string(currentThreadId());
    line += "}\n";
    std::fwrite(line.data(), 1, line.size(), g_journal);
    std::fflush(g_journal);
}

OtlpTraceExporter& OtlpTraceExporter::instance() {
    static OtlpTraceExporter e;
    return e;
}

bool OtlpTraceExporter::start(const std::string& workingDir) {
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

    /* Same base as the logs, different signal path. A caller may configure
     * either a base URL or a full logs URL, so a trailing /v1/logs is
     * rewritten rather than appended to. */
    const std::string logsSuffix = "/v1/logs";
    if (endpoint.size() >= logsSuffix.size() &&
        endpoint.compare(endpoint.size() - logsSuffix.size(),
                         logsSuffix.size(), logsSuffix) == 0) {
        endpoint = endpoint.substr(0, endpoint.size() - logsSuffix.size());
    }
    const std::string tracesSuffix = "/v1/traces";
    if (endpoint.size() < tracesSuffix.size() ||
        endpoint.compare(endpoint.size() - tracesSuffix.size(),
                         tracesSuffix.size(), tracesSuffix) != 0) {
        endpoint += tracesSuffix;
    }

    g_anchorUnixNano = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    g_anchorTick = nowTick();

    /*
     * One generation of history, then truncate.
     *
     * Truncating outright loses the crash you are chasing the moment you
     * relaunch to look at it, which is exactly what happened here: an app
     * hung, the console was restarted, and the journal describing the hang
     * was destroyed by the run that went looking for it. Appending forever is
     * not the answer either, on a card that has to last.
     *
     * So the previous run is kept as spans.jsonl.prev. After a crash the
     * evidence survives one relaunch, which is all it needs to survive.
     */
    const std::string journalPath = workingDir + "/spans.jsonl";
    const std::string previousPath = journalPath + ".prev";
    std::remove(previousPath.c_str());
    std::rename(journalPath.c_str(), previousPath.c_str());

    g_journal = std::fopen(journalPath.c_str(), "w");

    m_endpoint = endpoint;
    m_enabled = true;
    return true;
}

void OtlpTraceExporter::stop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    m_enabled = false;
}

OtlpTraceExporter::Span OtlpTraceExporter::begin(const char* name,
                                                 const Span* parent) {
    (void)name;
    Span s;
    if (!m_enabled) {
        return s;
    }

    if (parent && parent->valid) {
        s.traceHi = parent->traceHi;
        s.traceLo = parent->traceLo;
        s.parentId = parent->spanId;
    } else {
        s.traceHi = randomU64();
        s.traceLo = randomU64();
        s.parentId = 0;
    }
    s.spanId = randomU64();
    s.startTick = nowTick();
    s.valid = true;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_started++;
    }
    return s;
}

void OtlpTraceExporter::beginSession(const char* name) {
    if (!m_enabled) {
        return;
    }
    Span root = begin(name, nullptr);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sessionRoot = root;
    g_sessionName = name;
}

void OtlpTraceExporter::endSession() {
    Span root;
    const char* name = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        root = g_sessionRoot;
        name = g_sessionName;
        g_sessionRoot = Span{};
        g_sessionName = nullptr;
    }
    if (root.valid) {
        end(root, name ? name : "session");
    }
}

OtlpTraceExporter::Span OtlpTraceExporter::sessionRoot() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sessionRoot;
}

void OtlpTraceExporter::end(
    const Span& span, const char* name,
    const std::vector<std::pair<std::string, std::string>>& attrs) {
    if (!span.valid || !m_enabled) {
        return;
    }

    FinishedSpan f;
    f.traceHi = span.traceHi;
    f.traceLo = span.traceLo;
    f.spanId = span.spanId;
    f.parentId = span.parentId;
    f.startUnixNano = tickToUnixNano(span.startTick);
    f.endUnixNano = tickToUnixNano(nowTick());
    f.threadId = currentThreadId();
    f.name = name ? name : "span";
    f.attrs = attrs;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_spans.size() >= kMaxBufferedSpans) {
        g_spans.pop_front();
        g_dropped++;
    }
    journalSpan(f);
    g_spans.push_back(std::move(f));
    g_ended++;
}

void OtlpTraceExporter::endError(
    const Span& span, const char* name, const std::string& message,
    const std::vector<std::pair<std::string, std::string>>& attrs) {
    if (!span.valid || !m_enabled) {
        return;
    }

    FinishedSpan f;
    f.traceHi = span.traceHi;
    f.traceLo = span.traceLo;
    f.spanId = span.spanId;
    f.parentId = span.parentId;
    f.startUnixNano = tickToUnixNano(span.startTick);
    f.endUnixNano = tickToUnixNano(nowTick());
    f.threadId = currentThreadId();
    f.name = name ? name : "span";
    f.error = message;
    f.attrs = attrs;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_spans.size() >= kMaxBufferedSpans) {
        g_spans.pop_front();
        g_dropped++;
    }
    journalSpan(f);
    g_spans.push_back(std::move(f));
    g_ended++;
}

std::string OtlpTraceExporter::takePayload() {
    std::deque<FinishedSpan> batch;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_spans.empty()) {
            return "";
        }
        batch.swap(g_spans);
    }

    std::string out;
    out.reserve(4096);
    out += "{\"resourceSpans\":[{\"resource\":{\"attributes\":[";
    appendStringAttr(out, "service.name", "moonlight-switch");
    out += ",";
    appendStringAttr(out, "host.name", "nintendo-switch");
    out += "]},\"scopeSpans\":[{\"spans\":[";

    bool first = true;
    for (const FinishedSpan& s : batch) {
        if (!first) {
            out += ",";
        }
        first = false;

        out += "{\"traceId\":\"";
        appendHex64(out, s.traceHi);
        appendHex64(out, s.traceLo);
        out += "\",\"spanId\":\"";
        appendHex64(out, s.spanId);
        out += "\"";
        if (s.parentId) {
            out += ",\"parentSpanId\":\"";
            appendHex64(out, s.parentId);
            out += "\"";
        }
        out += ",\"name\":\"";
        appendEscaped(out, s.name);
        /* Times are strings in the OTLP JSON mapping: they are uint64 and
         * JSON numbers cannot carry that range without losing precision. */
        out += "\",\"kind\":1,\"startTimeUnixNano\":\"";
        out += std::to_string(s.startUnixNano);
        out += "\",\"endTimeUnixNano\":\"";
        out += std::to_string(s.endUnixNano);
        out += "\",\"attributes\":[";
        appendStringAttr(out, "thread.id", std::to_string(s.threadId));
        for (const auto& kv : s.attrs) {
            out += ",";
            appendStringAttr(out, kv.first, kv.second);
        }
        out += "]";
        if (!s.error.empty()) {
            /* status code 2 is ERROR in the OTLP mapping, which is what makes
             * Datadog mark the span as failed rather than merely slow. */
            out += ",\"status\":{\"code\":2,\"message\":\"";
            appendEscaped(out, s.error);
            out += "\"}";
        }
        out += "}";
    }

    out += "]}]}]}";
    return out;
}

OtlpTraceExporter::Stats OtlpTraceExporter::stats() const {
    std::lock_guard<std::mutex> lock(g_mutex);
    return Stats{g_started, g_ended, g_dropped};
}

OtlpSpanScope::OtlpSpanScope(const char* name,
                             const OtlpTraceExporter::Span* parent)
    : m_name(name) {
    /* Default to the session root rather than starting a fresh trace. A span
     * that silently becomes its own root is how the termination callback and
     * the resume ended up in unrelated traces, which is precisely the pair
     * this exists to show together. */
    if (parent) {
        m_span = OtlpTraceExporter::instance().begin(name, parent);
    } else {
        const OtlpTraceExporter::Span root =
            OtlpTraceExporter::instance().sessionRoot();
        m_span = OtlpTraceExporter::instance().begin(
            name, root.valid ? &root : nullptr);
    }
}

OtlpSpanScope::~OtlpSpanScope() {
    if (m_failed) {
        OtlpTraceExporter::instance().endError(m_span, m_name, m_error, m_attrs);
    } else {
        OtlpTraceExporter::instance().end(m_span, m_name, m_attrs);
    }
}

void OtlpSpanScope::attr(const std::string& key, const std::string& value) {
    m_attrs.emplace_back(key, value);
}

void OtlpSpanScope::fail(const std::string& message) {
    m_failed = true;
    m_error = message;
}
