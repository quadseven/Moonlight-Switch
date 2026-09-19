/*
 * Host tests for the OTLP exporters.
 *
 * These build and run on a development machine, not on the console. The
 * exporters are ordinary C++ with their Switch-specific calls behind
 * #ifdef __SWITCH__, so everything below the transport can be exercised
 * without hardware.
 *
 * That distinction matters more than it sounds. Every exporter defect found so
 * far was found by streaming on real hardware, crashing, carrying an SD card to
 * a Mac, and reading it: a cycle measured in hours that destroys its own
 * evidence when it goes wrong. Each one of them was reachable from here in
 * milliseconds. Two were invisible on hardware by construction, because a
 * refused POST and an idle app produce identical symptoms.
 *
 * The rule for anything added here: it has to fail against the code as it was
 * before the fix. A test that passes both ways documents an opinion rather
 * than catching a defect. Where a test corresponds to a specific historical
 * bug, that bug is named.
 *
 * takePayload()/confirmDelivered()/commitPayload()
 * -------------------------------------------------
 * takePayload() on both exporters only renders. It no longer removes spans
 * from the buffer or advances a counter's delta baseline / a gauge's peak
 * -- see testFailedPostDoesNotLoseSpans and testFailedPostDoesNotLoseMetrics
 * for why. confirmDelivered() (spans) and commitPayload() (metrics) are what
 * actually retire what was just rendered, and stand in for a successful
 * POST, so every test below that takes a payload and expects the buffer
 * clear or the baseline advanced afterwards calls one of them immediately
 * after taking it -- the same as a real caller would once its POST returns
 * 200. Skipping that call is only correct in a test that is deliberately
 * exercising what a failed POST leaves behind.
 */

#include "OtlpTraceExporter.hpp"
#include "OtlpMetricsExporter.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;
std::string g_currentTest;

void check(bool cond, const std::string& what) {
    g_checks++;
    if (!cond) {
        g_failures++;
        std::cout << "  FAIL  " << g_currentTest << ": " << what << "\n";
    }
}

#define TEST(name) g_currentTest = name; std::cout << "- " << name << "\n";

std::string g_workDir;

/* The exporters take a working directory and read their configuration from
 * files in it, the same way they do from the SD card. Building that here keeps
 * the tests on the real start() path instead of a special one. */
void setupWorkDir() {
    char tmpl[] = "/tmp/otlp-host-test-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) {
        std::cerr << "mkdtemp failed\n";
        std::exit(2);
    }
    g_workDir = dir;
    std::ofstream(g_workDir + "/otel-endpoint") << "https://example.invalid\n";
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

size_t countLines(const std::string& s) {
    size_t n = 0;
    for (char c : s) {
        if (c == '\n') n++;
    }
    return n;
}

/* Deliberately not a JSON parser. These look for exact byte sequences the OTLP
 * mapping requires, so a test fails on the specific thing that was wrong rather
 * than on a parse error somewhere in a large document. */
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

size_t countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        n++;
        pos += needle.size();
    }
    return n;
}

/* Pulls every value of a "key":"value" field out of a payload. */
std::vector<std::string> fieldValues(const std::string& doc,
                                     const std::string& key) {
    std::vector<std::string> out;
    const std::string pat = "\"" + key + "\":\"";
    size_t pos = 0;
    while ((pos = doc.find(pat, pos)) != std::string::npos) {
        const size_t start = pos + pat.size();
        const size_t end = doc.find('"', start);
        if (end == std::string::npos) break;
        out.push_back(doc.substr(start, end - start));
        pos = end;
    }
    return out;
}

// ---------------------------------------------------------------------------

/*
 * The bug: every span was parented to the session root, which covers the whole
 * run and therefore ends last, or never if the process dies. Datadog drops a
 * span whose parentSpanId it has not received, so the entire trace was
 * discarded on arrival. Confirmed against the live intake: two otherwise
 * identical spans, and only the one with no parent became queryable.
 *
 * Nothing on the console could show this. The journal on the card was complete,
 * the POST returned 200, and the spans were simply never there.
 */
void testNoDanglingParents() {
    TEST("spans never name a parent that is not also exported");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);
    tr.beginSession("test.session");

    /* Top level spans, the shape every instrumented call site uses. */
    for (int i = 0; i < 3; i++) {
        const OtlpTraceExporter::Span root = tr.sessionRoot();
        OtlpTraceExporter::Span s =
            tr.begin("top.level", root.valid ? &root : nullptr);
        tr.end(s, "top.level", {});
    }

    const std::string payload = tr.takePayload();
    tr.confirmDelivered();
    check(!payload.empty(), "payload was produced");

    const std::vector<std::string> spanIds = fieldValues(payload, "spanId");
    const std::vector<std::string> parents = fieldValues(payload, "parentSpanId");
    const std::set<std::string> present(spanIds.begin(), spanIds.end());

    for (const std::string& p : parents) {
        check(present.count(p) > 0,
              "parentSpanId " + p + " is not among the exported spanIds");
    }

    /* And the specific historical shape: a top level span must not be parented
     * to the session root, because that root is not in this payload and will
     * not be in any payload until the run ends. */
    const OtlpTraceExporter::Span root = tr.sessionRoot();
    if (root.valid) {
        char rootHex[17];
        std::snprintf(rootHex, sizeof(rootHex), "%016llx",
                      static_cast<unsigned long long>(root.spanId));
        check(!contains(payload, std::string("\"parentSpanId\":\"") + rootHex),
              "no span is parented to the session root");
    }

    tr.stop();
}

/*
 * Real nesting has to survive the fix above. A child of an ordinary span keeps
 * its parent, because that parent ends during the run. Losing this would trade
 * one silent defect for another: traces would arrive, and be flat.
 */
void testRealNestingIsPreserved() {
    TEST("a child of an ordinary span keeps its parent");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);
    tr.beginSession("test.session");

    const OtlpTraceExporter::Span root = tr.sessionRoot();
    OtlpTraceExporter::Span parent =
        tr.begin("parent", root.valid ? &root : nullptr);
    OtlpTraceExporter::Span child = tr.begin("child", &parent);
    tr.end(child, "child", {});
    tr.end(parent, "parent", {});

    const std::string payload = tr.takePayload();
    tr.confirmDelivered();
    check(countOccurrences(payload, "\"parentSpanId\"") == 1,
          "exactly one span carries a parent (the child, not the top level)");

    const std::vector<std::string> parents = fieldValues(payload, "parentSpanId");
    const std::set<std::string> ids = [&] {
        const std::vector<std::string> v = fieldValues(payload, "spanId");
        return std::set<std::string>(v.begin(), v.end());
    }();
    for (const std::string& p : parents) {
        check(ids.count(p) > 0, "the child's parent is exported alongside it");
    }

    tr.stop();
}

/*
 * Spans in one session share a trace id. Dropping the parent link must not
 * scatter them into unrelated traces, which was the reason the session root
 * existed in the first place: the termination callback and the applet resume
 * landing in different traces is what hid their overlap.
 */
void testSessionSharesOneTraceId() {
    TEST("spans in a session share one trace id");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);
    tr.beginSession("test.session");

    for (int i = 0; i < 4; i++) {
        const OtlpTraceExporter::Span root = tr.sessionRoot();
        OtlpTraceExporter::Span s =
            tr.begin("evt", root.valid ? &root : nullptr);
        tr.end(s, "evt", {});
    }

    const std::string payload = tr.takePayload();
    tr.confirmDelivered();
    const std::vector<std::string> traceIds = fieldValues(payload, "traceId");
    check(traceIds.size() == 4, "four spans exported");
    const std::set<std::string> distinct(traceIds.begin(), traceIds.end());
    check(distinct.size() == 1, "all four share a single trace id");
    for (const std::string& t : traceIds) {
        check(t.size() == 32, "trace id is 32 hex characters");
        check(t.find_first_not_of("0") != std::string::npos,
              "trace id is not all zeroes");
    }

    tr.stop();
}

/*
 * The bug: counters were exported with cumulative temporality. Datadog's OTLP
 * intake accepts only delta, and rejects the rest. Nothing arrived for an
 * entire evening and the failure was invisible, because a rejected POST looked
 * exactly like an app that had not been run.
 */
void testCountersAreDelta() {
    TEST("counters export delta temporality, and a delta, not a total");

    OtlpMetricsExporter& mx = OtlpMetricsExporter::instance();
    mx.start(g_workDir);

    mx.add("test.counter", 5);
    const std::string first = mx.takePayload();
    mx.commitPayload();
    check(contains(first, "\"aggregationTemporality\":1"),
          "temporality is 1 (delta), not 2 (cumulative)");
    check(contains(first, "\"asInt\":\"5\""), "first interval reports 5");

    /* The second interval must report only what happened in it. A cumulative
     * exporter reports 8 here; a delta exporter reports 3. */
    mx.add("test.counter", 3);
    const std::string second = mx.takePayload();
    mx.commitPayload();
    check(contains(second, "\"asInt\":\"3\""),
          "second interval reports the delta 3, not the running total 8");
    check(!contains(second, "\"asInt\":\"8\""),
          "second interval does not report a cumulative total");

    mx.stop();
}

/*
 * A gauge is not consumed by being read: it reports its current value every
 * interval. A live count that stopped being reported would look like it had
 * gone to zero, which for something like decoder_alive is the opposite of the
 * truth.
 */
void testGaugesRepeatAndBalance() {
    TEST("gauges repeat every interval and a scope leaves no residue");

    OtlpMetricsExporter& mx = OtlpMetricsExporter::instance();
    mx.start(g_workDir);

    mx.gauge("test.gauge", 7);
    check(contains(mx.takePayload(), "\"asInt\":\"7\""), "gauge reports 7");
    check(contains(mx.takePayload(), "\"asInt\":\"7\""),
          "gauge still reports 7 in the next interval without being set again");

    /* OtlpGaugeScope is what counts things like listop_inflight. Balanced
     * entry and exit must return to zero, or every session would drift. */
    {
        OtlpGaugeScope a("test.inflight");
        check(contains(mx.takePayload(), "\"asInt\":\"1\""),
              "one open scope reads 1");
        {
            OtlpGaugeScope b("test.inflight");
            check(contains(mx.takePayload(), "\"asInt\":\"2\""),
                  "two concurrent scopes read 2, which is the collision signal");
        }
        check(contains(mx.takePayload(), "\"asInt\":\"1\""),
              "closing the inner scope returns the count to 1");
    }
    check(contains(mx.takePayload(), "\"asInt\":\"0\""),
          "closing both scopes returns the count to 0");

    mx.stop();
}

/*
 * The journal is the only evidence that survives a hang, so a completed span
 * has to be on disk before the next one starts, not buffered.
 */
void testJournalIsWrittenPerSpan() {
    TEST("every completed span is on disk immediately");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);
    tr.beginSession("test.session");

    const std::string path = g_workDir + "/spans.jsonl";
    for (int i = 1; i <= 3; i++) {
        const size_t before = countLines(readFile(path));
        OtlpTraceExporter::Span s = tr.begin("journaled", nullptr);
        tr.end(s, "journaled", {});
        check(countLines(readFile(path)) == before + 1,
              "span " + std::to_string(i) + " is readable without a flush");
    }

    tr.stop();
}

/*
 * A span writes nothing until it ends, so a process that dies inside one leaves
 * no trace of having been there. Marks exist for the paths with no scopes to
 * wrap, and are only useful if they land the moment they are reached.
 */
void testMarksAreImmediateAndOrdered() {
    TEST("marks land immediately and in order");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);

    const std::string path = g_workDir + "/spans.jsonl";
    const size_t before = countLines(readFile(path));
    otlp_trace_mark("first");
    check(contains(readFile(path), "\"mark\":\"first\""),
          "the mark is on disk before the next statement runs");
    otlp_trace_mark("second");

    const std::string doc = readFile(path);
    check(doc.find("\"mark\":\"first\"") < doc.find("\"mark\":\"second\""),
          "marks appear in the order they were reached");
    check(countLines(doc) == before + 2, "one line per mark");

    tr.stop();
}

/*
 * Relaunching after a crash must not destroy the crash. One generation is kept,
 * which is what the workflow needs: crash, then carry the card to a machine.
 */
void testJournalRotates() {
    TEST("a new run preserves the previous run's journal");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();

    /* Its own directory. The journal is opened once per directory, which is
     * once per launch on the console, so rotation cannot be observed by
     * calling start() twice against the same one. */
    const std::string dir = g_workDir + "/rotate";
    std::filesystem::create_directories(dir);
    std::ofstream(dir + "/otel-endpoint") << "https://example.invalid\n";

    tr.openJournal(dir);
    otlp_trace_mark("run.one");

    /* A second launch, which is a fresh open of the same path. */
    tr.openJournal(g_workDir);          // move away
    tr.openJournal(dir);                // and back, as a new run would

    otlp_trace_mark("run.two");

    check(contains(readFile(dir + "/spans.jsonl"), "run.two"),
          "the current journal holds the current run");
    check(contains(readFile(dir + "/spans.jsonl.prev"), "run.one"),
          "the previous run survives as .prev");
    check(!contains(readFile(dir + "/spans.jsonl"), "run.one"),
          "the current journal is not appended to the old one");

    /* Rotating with nothing to rotate must not destroy the generation that is
     * already there. That happens whenever a run never opened a journal, or
     * the operator pulled spans.jsonl off the card to read it. */
    std::filesystem::remove(dir + "/spans.jsonl");
    tr.openJournal(g_workDir);
    tr.openJournal(dir);
    check(contains(readFile(dir + "/spans.jsonl.prev"), "run.one"),
          "a rotation with no source leaves the existing .prev intact");

    tr.openJournal(g_workDir);
}

/*
 * The exporters are driven from the main thread, a detached termination thread,
 * brls::async workers and the render thread at once. Built with -fsanitize=thread
 * this is the only place a race between them can be caught; on the console it
 * would present as the crash being investigated.
 */
void testConcurrentUse() {
    TEST("concurrent spans, marks and metrics from many threads");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    OtlpMetricsExporter& mx = OtlpMetricsExporter::instance();
    tr.start(g_workDir);
    mx.start(g_workDir);
    tr.beginSession("test.session");

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;

    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&] {
            while (!go.load()) { std::this_thread::yield(); }
            for (int i = 0; i < kPerThread; i++) {
                const OtlpTraceExporter::Span root = tr.sessionRoot();
                OtlpTraceExporter::Span s =
                    tr.begin("concurrent", root.valid ? &root : nullptr);
                OtlpGaugeScope inflight("test.concurrent_inflight");
                mx.add("test.concurrent_counter", 1);
                if (i % 32 == 0) {
                    otlp_trace_mark("concurrent.mark");
                }
                tr.end(s, "concurrent", {});
            }
        });
    }
    /* A reader running alongside the writers: takePayload copies the buffer
     * out from under threads that are still filling it, and confirming each
     * read drains it the same way a successful POST would, which is what
     * keeps this loop exercising steady-state concurrent access under TSan
     * instead of one growing snapshot. */
    std::thread reader([&] {
        while (!go.load()) { std::this_thread::yield(); }
        for (int i = 0; i < 50; i++) {
            if (!tr.takePayload().empty()) {
                tr.confirmDelivered();
            }
            if (!mx.takePayload().empty()) {
                mx.commitPayload();
            }
            std::this_thread::yield();
        }
    });

    go.store(true);
    for (std::thread& th : threads) th.join();
    reader.join();

    const OtlpTraceExporter::Stats st = tr.stats();
    check(st.started >= kThreads * kPerThread,
          "every span that began was counted");
    check(st.ended >= kThreads * kPerThread,
          "every span that ended was counted");

    /* Balanced scopes across every thread must land on zero. A non-zero value
     * here is the shape of the listop_inflight defect: an acquire without its
     * release, or one taken twice. This reads the gauge's live value, which
     * is current-value reporting and unaffected by whether a prior payload
     * was ever confirmed, but confirm anyway so nothing is left pending for
     * whatever test runs next. */
    const std::string finalPayload = mx.takePayload();
    mx.commitPayload();
    check(contains(finalPayload, "\"asInt\":\"0\""),
          "balanced scopes across 8 threads return the count to 0");

    tr.confirmDelivered();
    tr.stop();
    mx.stop();
}

/*
 * Names and error messages reach the payload from log lines and library
 * strings, so a quote or a control character in one must not produce a
 * document the backend rejects. This is the failure that would be silent
 * again: a malformed payload is a 400, and a 400 looks like nothing happening.
 */
void testPayloadEscaping() {
    TEST("quotes, backslashes and control characters are escaped");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);

    OtlpTraceExporter::Span s = tr.begin("weird", nullptr);
    tr.endError(s, "weird", "he said \"hi\"\\ and\nthen\tstopped",
                {{"attr\"key", "val\\ue"}});

    const std::string payload = tr.takePayload();
    tr.confirmDelivered();
    check(contains(payload, "\\\""), "quotes are escaped");
    check(contains(payload, "\\\\"), "backslashes are escaped");
    check(contains(payload, "\\n"), "newlines are escaped");
    check(contains(payload, "\\t"), "tabs are escaped");
    check(!contains(payload, "\nthen"), "no raw newline survives into the JSON");

    /* Braces and brackets must still balance, which is the cheapest proof that
     * escaping did not truncate the document. */
    check(countOccurrences(payload, "{") == countOccurrences(payload, "}"),
          "braces balance");
    check(countOccurrences(payload, "[") == countOccurrences(payload, "]"),
          "brackets balance");

    tr.stop();
}

/*
 * Timestamps are uint64 nanoseconds and must be strings in the OTLP JSON
 * mapping: as numbers they lose their low bits and every span lands in the
 * wrong microsecond. They must also be plausible wall clock values rather than
 * a raw tick count, which is what made an earlier version's spans arrive
 * decades away from the logs beside them.
 */
void testTimestampsAreStringsAndPlausible() {
    TEST("timestamps are quoted strings holding plausible wall clock nanos");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);

    OtlpTraceExporter::Span s = tr.begin("timed", nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    tr.end(s, "timed", {});

    const std::string payload = tr.takePayload();
    tr.confirmDelivered();
    check(contains(payload, "\"startTimeUnixNano\":\""),
          "startTimeUnixNano is a quoted string");
    check(contains(payload, "\"endTimeUnixNano\":\""),
          "endTimeUnixNano is a quoted string");

    const std::vector<std::string> starts =
        fieldValues(payload, "startTimeUnixNano");
    const std::vector<std::string> ends = fieldValues(payload, "endTimeUnixNano");
    check(starts.size() == 1 && ends.size() == 1, "one span, one of each");
    if (starts.size() == 1 && ends.size() == 1) {
        const unsigned long long st = std::stoull(starts[0]);
        const unsigned long long en = std::stoull(ends[0]);
        check(en > st, "the span ends after it starts");
        check(en - st >= 4'000'000ULL,
              "a 5ms span measures at least 4ms, so the clock has real resolution");
        /* 2020-01-01 in nanoseconds. Anything below this is a tick counter that
         * escaped without being converted to wall clock. */
        check(st > 1'577'836'800'000'000'000ULL,
              "the timestamp is a wall clock value, not a raw tick count");
    }

    tr.stop();
}

/*
 * Nothing may be recorded before start() or after stop(). A span whose scope
 * outlives the exporter is ordinary during shutdown, and it must not write
 * through a closed journal or a torn down buffer.
 */
void testDisabledExporterIsInert() {
    TEST("recording outside the exporter's lifetime does nothing");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();

    tr.start(g_workDir);
    OtlpTraceExporter::Span s = tr.begin("straddles.stop", nullptr);
    tr.stop();

    /* After stop, with a span still open. This is the shutdown shape. */
    tr.end(s, "straddles.stop", {});
    otlp_trace_mark("after.stop");

    const std::string doc = readFile(g_workDir + "/spans.jsonl");
    check(!contains(doc, "straddles.stop"),
          "a span ended after stop() is not recorded");

    const std::string payload = tr.takePayload();
    tr.confirmDelivered();
    check(!contains(payload, "straddles.stop"),
          "a span ended after stop() is not exported");
}

/*
 * The bug: takePayload() used to swap g_spans out and empty it in the same
 * call that rendered the payload, before the caller had even tried to POST
 * the string. A rejected POST -- a 502, same as any other transient
 * failure -- then discarded every span in it for good, with nothing in any
 * log to say so. Same shape as the RetroArch exporter's drop-on-failure,
 * except that one is a documented, deliberate choice and this one was
 * neither: it was a side effect of when the buffer happened to get cleared.
 *
 * The fix: takePayload() only renders. Spans stay in the buffer until
 * confirmDelivered() retires exactly what was rendered, and that only runs
 * after a POST actually succeeds.
 */
void testFailedPostDoesNotLoseSpans() {
    TEST("an unconfirmed span renders again on retry instead of being lost "
         "to a failed POST");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    tr.start(g_workDir);

    OtlpTraceExporter::Span s = tr.begin("unconfirmed", nullptr);
    tr.end(s, "unconfirmed", {});

    /* Simulates a POST that failed: rendered, but never confirmed. */
    const std::string firstAttempt = tr.takePayload();
    check(contains(firstAttempt, "unconfirmed"),
          "the span is in the first rendered payload");

    /* A retry must render the identical span again. Against the pre-fix
     * code, the first takePayload() already emptied g_spans by swapping it
     * out, so this returns "" and the check below fails. */
    const std::string retry = tr.takePayload();
    check(contains(retry, "unconfirmed"),
          "an unconfirmed payload renders the same span again on retry, "
          "not an empty one");

    /* A span that finishes while the (simulated) POST is in flight must
     * survive confirming the earlier payload: confirmDelivered() may only
     * retire what was actually rendered into `retry`, nothing added since. */
    OtlpTraceExporter::Span s2 = tr.begin("arrived_during_retry", nullptr);
    tr.end(s2, "arrived_during_retry", {});
    tr.confirmDelivered();

    const std::string afterConfirm = tr.takePayload();
    check(!contains(afterConfirm, "unconfirmed"),
          "confirming a delivered payload retires exactly what it rendered");
    check(contains(afterConfirm, "arrived_during_retry"),
          "a span that finished after the confirmed snapshot was taken is "
          "not swept away with it");

    tr.confirmDelivered();
    tr.stop();
}

/*
 * The bug: takePayload() used to advance a counter's exported baseline (and
 * reset a gauge's peak) in the same call that rendered them, before the
 * caller had tried to POST the string. A rejected POST then meant that delta
 * was gone: the next export computed against the new baseline as if the
 * failed one had shipped, exactly the "sent=0 evening" class of defect this
 * exporter exists to make visible, except silent to itself.
 */
void testFailedPostDoesNotLoseMetrics() {
    TEST("an uncommitted counter delta survives a takePayload() the way a "
         "failed POST would leave it");

    OtlpMetricsExporter& mx = OtlpMetricsExporter::instance();
    mx.start(g_workDir);

    mx.add("test.retry_counter", 4);
    const std::string firstAttempt = mx.takePayload();
    check(contains(firstAttempt, "\"asInt\":\"4\""),
          "the delta of 4 is in the first rendered payload");

    /* Simulated failed POST: no commitPayload() call. Against the pre-fix
     * code, the first takePayload() already advanced the exported baseline
     * to 4, so this renders a delta of 0 and the check below fails. */
    const std::string retry = mx.takePayload();
    check(contains(retry, "\"asInt\":\"4\""),
          "an uncommitted payload renders the identical delta again, not "
          "zero");

    /* A delta that accrues while the (simulated) POST is in flight must not
     * be swallowed by committing the earlier attempt: commitPayload() may
     * only move the baseline to what `retry` actually rendered (4), not to
     * whatever the live value has become since. */
    mx.add("test.retry_counter", 2);
    mx.commitPayload();

    const std::string afterCommit = mx.takePayload();
    check(contains(afterCommit, "\"asInt\":\"2\""),
          "the delta that accrued during the retry is exported on its own");
    check(!contains(afterCommit, "\"asInt\":\"6\""),
          "the already-committed 4 is not counted again alongside it");

    mx.commitPayload();
    mx.stop();
}

/*
 * The window is what keeps a per-frame span from costing a flush per frame.
 * It is read on the render thread and written on the teardown thread, and it
 * counts rather than flags, so overlapping teardowns cannot close it early.
 */
void testSpanWindow() {
    TEST("the per-frame span window nests and is closed only by its last holder");

    check(!otlp_span_window_open(), "closed by default");
    {
        OtlpSpanWindow a;
        check(otlp_span_window_open(), "open inside one window");
        {
            OtlpSpanWindow b;
            check(otlp_span_window_open(), "still open inside two");
        }
        check(otlp_span_window_open(),
              "the inner window closing does not close the outer one");
    }
    check(!otlp_span_window_open(), "closed once every holder is gone");
}

/*
 * Gauges leave this process only over the network, on a worker that parks when
 * the console loses focus and dies with it. During a sleep and at the moment of
 * a crash they are unreadable, which is exactly when they are wanted. Marks go
 * to the card, so they carry the gauges out.
 *
 * The peak matters more than the value. listop_inflight is held for about a
 * hundred milliseconds and frames_in_flight for microseconds, against a ten
 * second flush, so an instantaneous read is zero almost every time it is taken.
 * "It was never above 1" is the conclusion the gauge exists to support, and a
 * point sample says exactly that whether or not the collision happened.
 */
void testGaugesRideOnMarks() {
    TEST("marks carry the gauges, including a peak a point sample would miss");

    OtlpTraceExporter& tr = OtlpTraceExporter::instance();
    OtlpMetricsExporter& mx = OtlpMetricsExporter::instance();
    tr.start(g_workDir);
    mx.start(g_workDir);
    (void)mx.takePayload();
    mx.commitPayload();  // clear any pending baseline left by an earlier test

    /* A collision, opened and closed before anything could sample it. */
    {
        OtlpGaugeScope a("test.mark_inflight");
        OtlpGaugeScope b("test.mark_inflight");
    }

    otlp_trace_mark("after.the.collision");
    const std::string doc = readFile(g_workDir + "/spans.jsonl");
    const size_t markPos = doc.rfind("after.the.collision");
    check(markPos != std::string::npos, "the mark was written");
    const std::string markLine = doc.substr(markPos);

    check(contains(markLine, "\"m.test.mark_inflight\":0"),
          "the mark carries the gauge, which by now has returned to 0");
    check(contains(markLine, "\"m.test.mark_inflight.peak\":2"),
          "and the peak of 2, which is the collision a point sample cannot see");

    /* The same peak must reach the network payload, as its own series. */
    const std::string payload = mx.takePayload();
    check(contains(payload, "test.mark_inflight.peak"),
          "the peak is exported as its own metric series");
    check(contains(payload, "\"asInt\":\"2\""), "carrying the value 2");
    /* Confirms this exact payload as delivered, which is what actually
     * resets the peak. Without it the peak of 2 would still be pending and
     * would render again below, since nothing failed and nothing should be
     * retried. */
    mx.commitPayload();

    /* And after an export the peak restarts from the current value, not from
     * zero, or anything still held open across a flush would report a peak
     * below its own current value. */
    const std::string third = mx.takePayload();
    mx.commitPayload();
    check(!contains(third, "\"asInt\":\"2\""),
          "the peak does not persist into later intervals");

    tr.stop();
    mx.stop();
}

}  // namespace

int main() {
    setupWorkDir();
    std::cout << "OTLP host tests (workdir " << g_workDir << ")\n\n";

    testNoDanglingParents();
    testRealNestingIsPreserved();
    testSessionSharesOneTraceId();
    testCountersAreDelta();
    testGaugesRepeatAndBalance();
    testJournalIsWrittenPerSpan();
    testMarksAreImmediateAndOrdered();
    testJournalRotates();
    testPayloadEscaping();
    testTimestampsAreStringsAndPlausible();
    testFailedPostDoesNotLoseSpans();
    testFailedPostDoesNotLoseMetrics();
    testSpanWindow();
    testGaugesRideOnMarks();
    testConcurrentUse();
    testDisabledExporterIsInert();

    std::cout << "\n" << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    if (g_failures) {
        std::cout << g_failures << " FAILED\n";
        return 1;
    }
    std::cout << "ok\n";
    return 0;
}
