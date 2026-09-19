#include "OtlpTraceExporter.hpp"

#include "OtlpMetricsExporter.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>

#ifdef __SWITCH__
#include <switch.h>
#include <unistd.h>   /* fsync: fflush reaches the fs sysmodule, only this reaches the card */
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
/* How many entries at the front of g_spans were included in the payload
 * takePayload() most recently rendered, and are still awaiting a call to
 * confirmDelivered() before they may be erased. Zero when nothing is
 * pending. Plain size_t, not atomic: only ever touched under g_mutex. */
size_t g_pendingSentCount = 0;

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
/* Path g_journal was opened from, so a repeat call for the same directory is
 * a no-op rather than a second rotation. */
std::string g_journalPath;

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
    /*
     * Trace and span ids come from here, and they are generated on whichever
     * thread happens to open a span, so the host fallback has to tolerate that
     * as much as the console one does. The obvious static xorshift does not:
     * ThreadSanitizer flags the read-modify-write immediately, and two threads
     * interleaving in it can return the same value, which for a span id means
     * two different spans claiming to be one.
     *
     * Per-thread state rather than a lock. These are identifiers, not a
     * sequence anyone depends on, so threads advancing independently is fine,
     * and it keeps span creation off any shared cache line. Seeded from the
     * thread's own address so two threads do not start from the same value.
     */
    static thread_local uint64_t x = [] {
        uint64_t seed = 0x2545F4914F6CDD1DULL;
        seed ^= reinterpret_cast<uintptr_t>(&errno) * 0x9E3779B97F4A7C15ULL;
        seed ^= static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        return seed ? seed : 0x2545F4914F6CDD1DULL;
    }();
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

/*
 * Guards the journal FILE* only.
 *
 * Deliberately not g_mutex. Writing a span used to happen while holding the one
 * lock that also guards begin(), sessionRoot(), stats() and takePayload(), so
 * an SD write on the teardown thread stalled every other thread that touched a
 * span, including the render thread during the decoder.cleanup window. That is
 * the window under investigation, and the measurement was extending it: frame
 * durations read off the trace included time spent blocked on another thread's
 * card write, in the direction that makes the overlap look worse than it is.
 */
std::mutex g_journalMutex;
/* Latched on the first failed write. A full or read-only card otherwise makes
 * the journal silently stop growing, and a journal that stops is read as a
 * process that stopped, which turns lost evidence into a wrong diagnosis. */
bool g_journalWriteFailed = false;

void writeJournalLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_journalMutex);
    if (!g_journal) {
        return;
    }
    if (std::fwrite(line.data(), 1, line.size(), g_journal) != line.size()) {
        g_journalWriteFailed = true;
        return;
    }
    /* fflush pushes stdio's buffer into the fs sysmodule. It does NOT commit
     * to the card: only fsync does, which reaches fsFileFlush through fsdev.
     * The distinction is invisible until the case this file exists for, a hang
     * followed by a hard power off, where the uncommitted tail is exactly the
     * part describing the hang. */
    if (std::fflush(g_journal) != 0) {
        g_journalWriteFailed = true;
        return;
    }
#ifdef __SWITCH__
    if (fsync(fileno(g_journal)) != 0) {
        g_journalWriteFailed = true;
    }
#endif
}

std::string formatSpanLine(const FinishedSpan& f) {
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
    return line;
}

}  // namespace

namespace {
/* Opened and closed from the decoder teardown thread, read from the render
 * thread every frame, so it has to be atomic rather than a plain int. A count
 * rather than a flag because nothing guarantees one teardown at a time, and a
 * bool would let the first one to finish close the window on the others. */
std::atomic<int> g_spanWindows{0};
}  // namespace

OtlpSpanWindow::OtlpSpanWindow() {
    g_spanWindows.fetch_add(1, std::memory_order_release);
}

OtlpSpanWindow::~OtlpSpanWindow() {
    g_spanWindows.fetch_sub(1, std::memory_order_release);
}

bool otlp_span_window_open() {
    return g_spanWindows.load(std::memory_order_acquire) > 0;
}

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
    /* Gauges ride along. They otherwise leave only over the network, on a
     * worker that parks when the console loses focus and dies with the
     * process, so at the moment a breadcrumb is worth reading they are
     * unreadable. listop_inflight above 1 is the defect itself; this is what
     * lets it be read off the card with no working network at any point. */
    line += otlp_metrics_snapshot();
    line += "}\n";
    writeJournalLine(line);
}

OtlpTraceExporter& OtlpTraceExporter::instance() {
    static OtlpTraceExporter e;
    return e;
}

bool OtlpTraceExporter::start(const std::string& workingDir) {
    /*
     * The journal is opened before any of this, and regardless of how it goes.
     *
     * Recording and shipping used to be one decision: no otel-endpoint file
     * meant an early return here, which meant no spans.jsonl, no marks, and no
     * log line saying so. The card is the only store that survives a hang, and
     * it was switched off by the absence of a network setting. Worse, the
     * result was indistinguishable from the failure being investigated: a
     * perfectly clean run would produce an empty journal, which reads as having
     * died before the first breadcrumb.
     *
     * Shipping over the network is allowed to fail. Writing to the card the
     * console is already running from is not the same kind of risk, and it is
     * the half that matters when everything else is gone.
     */
    openJournal(workingDir);

    std::string endpoint;
    {
        std::ifstream f(workingDir + "/otel-endpoint");
        if (f.good()) {
            std::getline(f, endpoint);
        }
    }

    const auto begin = endpoint.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        /* No transport. Still recording: the journal is open, spans and marks
         * land on the card, and takePayload simply has nowhere to send them.
         * Returning false so the caller can say so rather than claim export
         * is enabled. */
        m_enabled = true;
        m_endpoint.clear();
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

    m_endpoint = endpoint;
    m_enabled = true;
    return true;
}

void OtlpTraceExporter::openJournal(const std::string& workingDir) {
    /*
     * Idempotent for the same directory, because start() calls this and so does
     * main before it, and rotating twice on one launch would throw away the
     * previous run for nothing.
     *
     * A different directory reopens. That does not happen on the console, where
     * this runs once during startup before any other thread exists, which is
     * also why the swap is safe: otlp_trace_mark reads g_journal without the
     * lock, deliberately, and that is only sound while nothing reopens it
     * underneath a running app.
     */
    const std::string journalPath = workingDir + "/spans.jsonl";
    if (g_journal) {
        if (g_journalPath == journalPath) {
            return;
        }
        std::FILE* old = g_journal;
        g_journal = nullptr;
        std::fclose(old);
    }
    g_journalPath = journalPath;

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
    const std::string previousPath = journalPath + ".prev";

    /* Rotate only if there is something to rotate. Removing .prev first and
     * then renaming a file that is not there deletes a generation and puts
     * nothing in its place, which is a net loss of evidence for no reason.
     * That happens whenever the last run never opened a journal, or the
     * operator pulled spans.jsonl off the card to read it. */
    std::ifstream existing(journalPath);
    if (existing.good()) {
        existing.close();
        std::remove(previousPath.c_str());
        std::rename(journalPath.c_str(), previousPath.c_str());
    }

    g_journal = std::fopen(journalPath.c_str(), "w");

    /*
     * A header line, so a journal can be identified later.
     *
     * A crash report names offsets into a build, and a journal describes a run
     * of one, and until now nothing tied either to the other. The anchor
     * matters too: every timestamp in this file is derived from it, and
     * without it a reader cannot re-derive when anything happened if the clock
     * moved.
     */
    if (g_journal) {
        std::string line = "{\"run\":\"start\",\"anchorUnixNano\":";
        line += std::to_string(g_anchorUnixNano);
        line += ",\"anchorTick\":";
        line += std::to_string(g_anchorTick);
        line += "}\n";
        std::fwrite(line.data(), 1, line.size(), g_journal);
        std::fflush(g_journal);
    }
    g_journalWriteFailed = false;
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

        /*
         * The session root is a trace id, not a parent.
         *
         * It spans the whole run, so it only ends when the run does, which
         * means it is the last span to ship and on a crash it never ships at
         * all. Datadog drops a span whose parent it has not received: posting
         * one with a dangling parentSpanId returns 200 and then the span is
         * simply never queryable. Verified against the intake directly, an
         * otherwise identical span with no parent arrives and one naming an
         * unsent parent does not.
         *
         * Every span here was parented to that root, so the entire trace was
         * being discarded on arrival, on clean exits as much as on crashes.
         * Inheriting the trace id still groups a session together; dropping
         * the link makes each top level span a root that stands on its own.
         *
         * Real nesting below the root keeps its parent, since those parents
         * are ordinary spans that end during the run.
         */
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_sessionRoot.valid && s.parentId == g_sessionRoot.spanId) {
                s.parentId = 0;
            }
        }
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

    /* Formatted before the lock and written after it. Only the buffer needs
     * g_mutex; the card write does not, and holding it across an SD flush
     * stalled every other thread that touches a span. */
    const std::string line = formatSpanLine(f);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_spans.size() >= kMaxBufferedSpans) {
            g_spans.pop_front();
            g_dropped++;
        }
        g_spans.push_back(std::move(f));
        g_ended++;
    }
    writeJournalLine(line);
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

    /* Formatted before the lock and written after it. Only the buffer needs
     * g_mutex; the card write does not, and holding it across an SD flush
     * stalled every other thread that touches a span. */
    const std::string line = formatSpanLine(f);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_spans.size() >= kMaxBufferedSpans) {
            g_spans.pop_front();
            g_dropped++;
        }
        g_spans.push_back(std::move(f));
        g_ended++;
    }
    writeJournalLine(line);
}

std::string OtlpTraceExporter::takePayload() {
    std::deque<FinishedSpan> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_spans.empty()) {
            g_pendingSentCount = 0;
            return "";
        }
        /* Copy rather than swap. A swap used to empty g_spans right here, so
         * a single failed POST discarded every span in the batch with no way
         * to try again -- exactly the evidence a 502 during the crash under
         * investigation would take with it. The count is recorded so
         * confirmDelivered() removes exactly what got rendered into this
         * payload, not whatever g_spans holds by the time the POST returns:
         * other threads keep calling end()/endError() while curl runs. */
        snapshot.assign(g_spans.begin(), g_spans.end());
        g_pendingSentCount = snapshot.size();
    }

    std::string out;
    out.reserve(4096);
    out += "{\"resourceSpans\":[{\"resource\":{\"attributes\":[";
    appendStringAttr(out, "service.name", "moonlight-switch");
    out += ",";
    appendStringAttr(out, "host.name", "nintendo-switch");
    out += "]},\"scopeSpans\":[{\"spans\":[";

    bool first = true;
    for (const FinishedSpan& s : snapshot) {
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

void OtlpTraceExporter::confirmDelivered() {
    std::lock_guard<std::mutex> lock(g_mutex);
    const size_t n = std::min(g_pendingSentCount, g_spans.size());
    for (size_t i = 0; i < n; i++) {
        g_spans.pop_front();
    }
    g_pendingSentCount = 0;
}

int OtlpTraceExporter::journalFd() const {
    std::lock_guard<std::mutex> lock(g_journalMutex);
    return g_journal ? fileno(g_journal) : -1;
}

bool OtlpTraceExporter::journalWriteFailed() const {
    std::lock_guard<std::mutex> lock(g_journalMutex);
    return g_journalWriteFailed;
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
