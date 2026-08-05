#include "StdoutCapture.hpp"

#include <fstream>
#include <mutex>
#include <string>

#include "OtlpLogExporter.hpp"

#ifdef __SWITCH__
#include <sys/iosupport.h>
#include <unistd.h>
#endif

namespace {

bool g_active = false;

#ifdef __SWITCH__

/*
 * Threading and re-entrancy
 * -------------------------
 * printf can be called from any thread, so the partial line buffers are
 * shared state and need a lock.
 *
 * There is deliberately no thread_local here, and no thread_local guard
 * against re-entering this function. OtlpLogExporter.cpp documents why: a
 * thread_local store faults on a thread spawned on this platform, because
 * TPIDR_EL0 is zero and the generated code offsets straight off it. That rule
 * applies to this file for the same reason.
 *
 * Re-entrancy is therefore prevented by construction rather than by a guard.
 * Nothing below writes to stdout or stderr: it appends to a std::string, and
 * calls OtlpLogExporter::logRaw, which takes a mutex, pushes onto a deque and
 * bumps counters. If anything on that path ever gains a printf, this becomes
 * an infinite recursion, and the lock below turns it into a deadlock instead.
 * Keep it silent.
 *
 * Note also that brls::Logger must never be called from here. borealis writes
 * the formatted line to logOut before firing its event, and fires that event
 * while holding logMtx; logging from this path would both feed the capture
 * its own output and deadlock on a non recursive mutex.
 */
std::mutex g_mutex;
std::string g_outBuffer;
std::string g_errBuffer;

// A single write can carry many lines, or a fragment of one. Ship on newline
// and keep the remainder for the next call, so a record is a line rather than
// whatever size the caller happened to write in.
//
// The cap exists because a caller that never emits a newline would otherwise
// grow this without bound. printf("%s", giant_blob) is a real thing, and an
// unbounded buffer on a console with no swap is worse than a split line.
constexpr size_t kMaxPending = 8192;

void drain(std::string& pending, const char* data, size_t len, bool isStderr) {
    pending.append(data, len);

    size_t start = 0;
    while (true) {
        const size_t newline = pending.find('\n', start);
        if (newline == std::string::npos) {
            break;
        }
        size_t end = newline;
        // Trailing CR, so a library using \r\n does not leave one on the body.
        if (end > start && pending[end - 1] == '\r') {
            end--;
        }
        if (end > start) {
            OtlpLogExporter::instance().logRaw(
                pending.substr(start, end - start), isStderr);
        }
        start = newline + 1;
    }

    pending.erase(0, start);

    if (pending.size() > kMaxPending) {
        OtlpLogExporter::instance().logRaw(pending, isStderr);
        pending.clear();
    }
}

ssize_t captureWrite(struct _reent* r, void* fd, const char* ptr, size_t len) {
    (void)r;
    (void)fd;
    if (ptr == nullptr || len == 0) {
        return static_cast<ssize_t>(len);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        drain(g_outBuffer, ptr, len, false);
    }
    // Report everything consumed. Returning less makes stdio retry the tail,
    // which would duplicate the part already buffered.
    return static_cast<ssize_t>(len);
}

ssize_t captureWriteErr(struct _reent* r, void* fd, const char* ptr,
                        size_t len) {
    (void)r;
    (void)fd;
    if (ptr == nullptr || len == 0) {
        return static_cast<ssize_t>(len);
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        drain(g_errBuffer, ptr, len, true);
    }
    return static_cast<ssize_t>(len);
}

// Same shape as libnx's own dotab_stdout in console.c: a device that
// implements nothing but write_r, installed into the STD_OUT / STD_ERR slots
// of devoptab_list. Both must be static storage, since devoptab_list keeps
// the pointer for the life of the process.
const devoptab_t g_outDevice = {
    .name = "otel-out",
    .write_r = captureWrite,
};

const devoptab_t g_errDevice = {
    .name = "otel-err",
    .write_r = captureWriteErr,
};

#endif  // __SWITCH__

}  // namespace

namespace StdoutCapture {

bool install(const std::string& workingDir) {
#ifdef __SWITCH__
    if (g_active) {
        return true;
    }

    // Presence of the file is the switch; contents are ignored. Same
    // convention as the otel-* files the exporter already reads.
    {
        std::ifstream flag(workingDir + "/otel-capture-stdout");
        if (!flag.good()) {
            return false;
        }
    }

    // Nothing to capture into. Installing anyway would swallow every printf
    // and send it nowhere, which is strictly worse than leaving it alone:
    // output that used to reach a console or an nxlink session would vanish.
    if (!OtlpLogExporter::instance().enabled()) {
        return false;
    }

    // Unbuffered, so a line reaches captureWrite when it is printed rather
    // than when stdio decides the block is full. A crash with the interesting
    // line still sitting in a stdio buffer is the failure mode this whole
    // exporter exists to avoid.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    devoptab_list[STD_OUT] = &g_outDevice;
    devoptab_list[STD_ERR] = &g_errDevice;

    g_active = true;
    return true;
#else
    (void)workingDir;
    return false;
#endif
}

bool active() { return g_active; }

}  // namespace StdoutCapture
