#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
 * OTLP span exporter.
 *
 * Why this exists when there is already a log exporter
 * ---------------------------------------------------
 * A log line says something happened. It cannot say what was happening at the
 * same time, on which thread, or for how long, and those three questions are
 * the whole of a race condition.
 *
 * The crash this was written for looks like this in the log:
 *
 *     21:54:06.904  Connection terminated with code: 115
 *     21:54:06.907  window focus gained
 *     21:54:07.803  Received first audio p     <- log ends mid word
 *
 * Three milliseconds between a termination callback arriving on a detached
 * thread and the app resuming. From lines alone there is no way to tell
 * whether those overlapped, which one was still running when the other
 * started, or which thread each belonged to. As spans, with start and end
 * times and a thread id, the overlap is simply visible.
 *
 * Sent as OTLP/HTTP JSON to /v1/traces. Datadog ingests that natively and
 * renders it as APM traces, so no vendor SDK is imported and pointing this at
 * a different backend is a config change rather than a rewrite.
 *
 * Timing
 * ------
 * Durations come from armGetSystemTick, a monotonic counter, not from the
 * wall clock, which on this console only advances in whole seconds. The
 * absolute position of a trace is anchored to time() once; everything within
 * it is tick-derived and correctly ordered.
 *
 * Threading
 * ---------
 * Spans may be started and ended on any thread, including the detached one
 * that delivers connection_terminated. Completed spans go into a bounded
 * buffer under a mutex and are posted by the log exporter's existing worker,
 * so this adds no thread of its own.
 *
 * No thread_local anywhere, for the reason OtlpLogExporter.cpp documents at
 * length: a thread_local store faults on a thread spawned on this platform.
 * That is also why a span carries its own ids rather than relying on an
 * implicit ambient current-span.
 */
class OtlpTraceExporter {
  public:
    static OtlpTraceExporter& instance();

    /** Reads config from workingDir. Inert unless otel-endpoint exists. */
    bool start(const std::string& workingDir);
    void stop();

    [[nodiscard]] bool enabled() const { return m_enabled; }

    /**
     * A span in flight.
     *
     * Deliberately a value type the caller holds, rather than an ambient
     * "current span" kept in thread_local storage, which is unavailable here.
     * Pass one to a child to nest, or default-construct for a root.
     */
    struct Span {
        uint64_t traceHi = 0;
        uint64_t traceLo = 0;
        uint64_t spanId = 0;
        uint64_t parentId = 0;
        uint64_t startTick = 0;
        bool valid = false;
    };

    /**
     * Begins a span. Cheap and safe when disabled: returns an invalid span
     * that end() ignores.
     */
    Span begin(const char* name, const Span* parent = nullptr);

    /**
     * Opens a session-wide trace that later spans can hang from.
     *
     * Without this, a span started on the main thread and a span started on
     * the detached thread that delivers connection_terminated get separate
     * trace ids, and a backend shows them as two unrelated things that
     * happened to occur at similar times. The entire reason for tracing this
     * app is to see that they OVERLAP, which requires them in one trace.
     *
     * The root is deliberately not ended until the session does, so it may
     * be open for hours. That is normal for a session-scoped trace and is
     * what makes the flamegraph span the sleep.
     */
    void beginSession(const char* name);
    void endSession();

    /**
     * The current session root, or an invalid span if none is open.
     *
     * Safe to call from any thread, including the detached termination
     * callback, which is the caller that needs it most and has no other way
     * to reach the main thread's span.
     */
    [[nodiscard]] Span sessionRoot() const;

    /**
     * Ends a span and buffers it. @attrs are flat key/value pairs, added as
     * span attributes alongside thread.id.
     */
    void end(const Span& span, const char* name,
             const std::vector<std::pair<std::string, std::string>>& attrs = {});

    /**
     * Marks a span as failed, which Datadog surfaces as an errored span.
     */
    void endError(const Span& span, const char* name, const std::string& message,
                  const std::vector<std::pair<std::string, std::string>>& attrs = {});

    /** Drains buffered spans into an OTLP payload, or "" if none. */
    [[nodiscard]] std::string takePayload();

    /** Resolved traces endpoint, for logging. Carries no credential. */
    [[nodiscard]] std::string endpoint() const { return m_endpoint; }

    struct Stats {
        size_t started;
        size_t ended;
        size_t droppedOverflow;
    };
    [[nodiscard]] Stats stats() const;

  private:
    OtlpTraceExporter() = default;

    OtlpTraceExporter(const OtlpTraceExporter&) = delete;
    OtlpTraceExporter& operator=(const OtlpTraceExporter&) = delete;

    bool m_enabled = false;
    std::string m_endpoint;
};

/**
 * Opens a window in which per-frame spans are worth recording.
 *
 * The journal fflush()es every span as it completes, which is what makes it
 * survive a hang, and is affordable only while spans are lifecycle events a
 * handful per session. A span on the render path is 60 a second, so recording
 * it unconditionally means 60 SD card flushes a second, a journal growing by
 * megabytes a minute, and the lifecycle spans being evicted from the export
 * buffer by frames. Worse, it perturbs the timing of the race it was added to
 * observe.
 *
 * The frames only mean anything next to a decoder teardown: one drawing while
 * the other frees is the whole question, and a frame drawn with no teardown in
 * progress answers nothing. So decoder.cleanup opens this window and the render
 * path records a span only while it is open. Costs an atomic load per frame in
 * the normal case and yields exactly the frames that could overlap.
 */
class OtlpSpanWindow {
public:
    OtlpSpanWindow();
    ~OtlpSpanWindow();

    OtlpSpanWindow(const OtlpSpanWindow&) = delete;
    OtlpSpanWindow& operator=(const OtlpSpanWindow&) = delete;
};

/** Whether any OtlpSpanWindow is currently open, on any thread. */
[[nodiscard]] bool otlp_span_window_open();

/**
 * Writes a one-line breadcrumb to the span journal immediately.
 *
 * Spans are only journalled once they end, so the last thing a span can tell
 * you is that it finished. If the process dies partway through one, the journal
 * is silent about it, and a teardown that died halfway looks exactly like a
 * process killed before teardown began.
 *
 * Marks close that gap: reaching one and not the next places the failure
 * between them. Intended for the shutdown path, which is a straight line of
 * destructors with no natural scopes to wrap.
 *
 * Safe to call while unwinding; takes no exporter lock.
 */
void otlp_trace_mark(const char* name);

/**
 * Scope guard, so a span cannot be left unclosed on an early return.
 *
 * Names are string literals held by pointer: a span name is a constant in
 * every use here, and copying a std::string per span on the video path is
 * exactly the overhead this must not add.
 */
class OtlpSpanScope {
  public:
    explicit OtlpSpanScope(const char* name,
                           const OtlpTraceExporter::Span* parent = nullptr);
    ~OtlpSpanScope();

    OtlpSpanScope(const OtlpSpanScope&) = delete;
    OtlpSpanScope& operator=(const OtlpSpanScope&) = delete;

    /** Adds an attribute recorded when the scope closes. */
    void attr(const std::string& key, const std::string& value);

    /** Marks the span errored; still closed by the destructor. */
    void fail(const std::string& message);

    [[nodiscard]] const OtlpTraceExporter::Span& span() const { return m_span; }

  private:
    const char* m_name;
    OtlpTraceExporter::Span m_span;
    std::vector<std::pair<std::string, std::string>> m_attrs;
    std::string m_error;
    bool m_failed = false;
};
