#pragma once

#include <cstdint>
#include <string>

/**
 * OTLP metrics exporter.
 *
 * What this is for that logs and spans are not
 * --------------------------------------------
 * Some defects produce no log line and no span, because nothing went wrong
 * from the code's point of view. A subscription created twice where one was
 * intended is a working program right up until the moment it corrupts memory,
 * and it is invisible to anything that only records events.
 *
 * What catches that is a number with a known correct answer, emitted
 * continuously and watched. The focus subscription count should be 1 while a
 * stream is up and 0 otherwise. When it reads 5, the bug is not inferred from
 * a stack trace, it is simply visible, and it was visible for however long it
 * took someone to look.
 *
 * That is not a hypothetical example. A reviewer found exactly that leak in
 * this app with gdb, by opening and closing the overlay repeatedly and
 * counting subscriptions by hand. A gauge would have shown it without anyone
 * attaching a debugger.
 *
 * Design
 * ------
 * Deliberately small. A fixed table of named series, no allocation on the
 * update path, updated from any thread under one lock.
 *
 * Counters are exported as DELTA sums: each point is the change since the
 * previous export, not a running total. Cumulative is the more usual choice
 * and is what this reached for first; Datadog's OTLP intake rejects it
 * outright, and the rejection is silent from in here, so nothing arrived for a
 * whole evening before anyone noticed.
 *
 * Gauges report the last value written, AND a peak alongside it. The peak is
 * not decoration. A gauge is sampled once per export and the interval is ten
 * seconds, while the counts worth watching here are held for a hundred
 * milliseconds or less, so an instantaneous read is zero almost every time it
 * is taken. Reporting only that would answer "was there ever a collision" with
 * a confident no.
 *
 * Sent to /v1/metrics by the log exporter's worker, for the same reason spans
 * are: that loop already knows not to touch the network while the console is
 * suspended. It is also the only way out, which is why otlp_metrics_snapshot()
 * below exists to put these same numbers on the SD card.
 */
class OtlpMetricsExporter {
  public:
    static OtlpMetricsExporter& instance();

    bool start(const std::string& workingDir);
    void stop();

    [[nodiscard]] bool enabled() const { return m_enabled; }
    [[nodiscard]] std::string endpoint() const { return m_endpoint; }

    /**
     * Sets a gauge. Use for anything with a correct answer at any instant:
     * how many subscriptions are held, how many sessions are active, how many
     * frame mappings exist.
     */
    void gauge(const char* name, int64_t value);

    /**
     * Adds to a counter. Use for things that only ever happen, never unhappen.
     *
     * The running total is kept here; what leaves is the change since the last
     * export, because that is the only temporality the intake accepts. Callers
     * do not need to care, and should not: pass what happened, not a total.
     */
    void add(const char* name, int64_t delta);

    /** Convenience for the common case of counting one occurrence. */
    void increment(const char* name) { add(name, 1); }

    /**
     * Renders everything currently held, or "" when there is nothing.
     *
     * Does not commit: a counter's exported baseline and a gauge's peak stay
     * where they were until commitPayload() confirms this exact payload was
     * delivered. Call commitPayload() only after a successful POST; on a
     * failed one, do nothing and the same deltas render again next time
     * instead of being silently discarded.
     */
    [[nodiscard]] std::string takePayload();

    /** Commits the deltas rendered by the most recent takePayload() call. */
    void commitPayload();

  private:
    OtlpMetricsExporter() = default;

    OtlpMetricsExporter(const OtlpMetricsExporter&) = delete;
    OtlpMetricsExporter& operator=(const OtlpMetricsExporter&) = delete;

    bool m_enabled = false;
    std::string m_endpoint;
};

/**
 * Ties a gauge to a scope, so the count cannot drift when a path returns
 * early or throws.
 *
 * Incrementing on construction and decrementing on destruction is the whole
 * point: a leak shows up as a number that never comes back down, which is
 * precisely the failure this is here to make visible.
 */
class OtlpGaugeScope {
  public:
    explicit OtlpGaugeScope(const char* name);
    ~OtlpGaugeScope();

    OtlpGaugeScope(const OtlpGaugeScope&) = delete;
    OtlpGaugeScope& operator=(const OtlpGaugeScope&) = delete;

  private:
    const char* m_name;
};

/**
 * Adjusts a live count that outlives any one scope, for things owned by an
 * object rather than a block: a subscription held for the life of a view, a
 * session that exists until it is torn down.
 */
void otlp_metric_count_add(const char* name, int64_t delta);

/**
 * Every gauge's current value, and its peak, as JSON fields for the span
 * journal.
 *
 * Metrics leave this process only over the network, on a worker that parks
 * when the console loses focus and dies with the process. During a sleep and
 * at the moment of a crash the gauges are unreadable, which is precisely when
 * they are wanted. The journal on the card survives both; this is what lets a
 * breadcrumb carry them.
 *
 * Each field is prefixed "m." and begins with a comma, so it appends directly
 * inside an existing JSON object.
 */
[[nodiscard]] std::string otlp_metrics_snapshot();
