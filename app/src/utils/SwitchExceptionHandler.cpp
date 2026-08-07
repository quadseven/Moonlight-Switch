/*
 * Records a CPU exception to the span journal before the process dies.
 *
 * Why this is the missing piece
 * ----------------------------
 * The crash under investigation produces no Atmosphere crash report and no
 * fatal report. That single observation is consistent with three completely
 * different failures, and nothing available today separates them:
 *
 *   - a CPU fault, which normally does produce a crash report, so its absence
 *     would mean the report is being lost rather than never written;
 *   - the OS terminating the process from outside, after an exit request whose
 *     grace period was overrun by teardown, which leaves no artifact anywhere
 *     by design;
 *   - a hang, ended by the operator hard powering the console off, which also
 *     leaves nothing.
 *
 * These want opposite investigations. A mark written from the exception
 * handler answers it directly: present means the process faulted and the fault
 * address is on the card; absent means it did not, and the remaining two are
 * the only candidates left.
 *
 * Constraints this runs under
 * ---------------------------
 * This executes in an exception context on a dedicated stack, with the faulting
 * thread stopped. Almost nothing is safe here. No allocation, no std::string,
 * no stdio: the heap or a stdio lock may be exactly what is broken, and taking
 * a lock another thread died holding turns a recorded crash into a hang.
 *
 * So: a preopened file descriptor captured while the app was healthy, a fixed
 * static buffer, hand rolled hex, one write, one fsync. Nothing that can
 * allocate and nothing that can block on another thread.
 *
 * The default exception stack is 0x400 bytes, which is enough for this but not
 * for much, so it is enlarged rather than relied upon.
 */

#ifdef __SWITCH__

#include <switch.h>
#include <unistd.h>

#include "SwitchExceptionHandler.hpp"

namespace {

/* The journal's descriptor, captured while the process was healthy. Opening a
 * file from the handler would need the heap and the filesystem layer, both of
 * which may be why we are here. */
int g_journalFd = -1;

/* One line, built in place. Static so the exception stack does not have to
 * hold it. */
char g_line[320];
size_t g_len = 0;

void put(const char* s) {
    while (*s && g_len < sizeof(g_line) - 1) {
        g_line[g_len++] = *s++;
    }
}

void putHex(uint64_t v) {
    static const char digits[] = "0123456789abcdef";
    char tmp[17];
    int i = 16;
    tmp[16] = '\0';
    if (v == 0) {
        put("0");
        return;
    }
    while (v && i > 0) {
        tmp[--i] = digits[v & 0xF];
        v >>= 4;
    }
    put(&tmp[i]);
}

void putDec(uint64_t v) {
    char tmp[21];
    int i = 20;
    tmp[20] = '\0';
    if (v == 0) {
        put("0");
        return;
    }
    while (v && i > 0) {
        tmp[--i] = static_cast<char>('0' + (v % 10));
        v /= 10;
    }
    put(&tmp[i]);
}

}  // namespace

void switch_exception_handler_arm(int journalFd) { g_journalFd = journalFd; }

/*
 * A larger exception stack than the 0x400 default. Overriding libnx's weak
 * symbols; the size variable has to move with the buffer or the kernel is told
 * the wrong bounds.
 */
extern "C" {
alignas(16) u8 __nx_exception_stack[0x1000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

/*
 * Overrides libnx's weak definition. Called with the faulting thread stopped.
 *
 * Returning from here lets libnx continue its default handling, which is what
 * produces the Atmosphere crash report, so this adds a record rather than
 * replacing one.
 */
void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    if (g_journalFd < 0 || ctx == nullptr) {
        return;
    }

    g_len = 0;
    put("{\"mark\":\"cpu.exception\",\"error_desc\":\"0x");
    putHex(ctx->error_desc);
    put("\",\"pc\":\"0x");
    putHex(ctx->pc.x);
    put("\",\"lr\":\"0x");
    putHex(ctx->lr.x);
    put("\",\"sp\":\"0x");
    putHex(ctx->sp.x);
    put("\",\"far\":\"0x");
    putHex(ctx->far.x);
    put("\",\"esr\":\"0x");
    /* Exception syndrome. This is what says whether the fault was a data abort,
     * an instruction abort, an alignment fault or an svcBreak from abort(),
     * which is the difference between a bad pointer and a std::terminate. */
    putHex(ctx->esr);
    put("\",\"fp\":\"0x");
    putHex(ctx->fp.x);
    put("\",\"thread\":");
    /* A bare syscall, no allocation and no lock. The handler runs on the
     * faulting thread, so this is the id that appears on the spans beside it. */
    {
        u64 tid = 0;
        if (R_FAILED(svcGetThreadId(&tid, CUR_THREAD_HANDLE))) {
            tid = 0;
        }
        putDec(tid);
    }
    put("}\n");

    /* Unchecked on purpose. There is nothing useful to do about a failure from
     * here, and a retry loop in an exception handler is a hang. */
    (void)!write(g_journalFd, g_line, g_len);
    /* fflush would be meaningless: this never went through stdio. fsync is
     * what commits to the card, and the console is about to stop. */
    (void)fsync(g_journalFd);
}
}  // extern "C"

#else   // !__SWITCH__

#include "SwitchExceptionHandler.hpp"

/* Nothing to hook on a host build; the tests link this so the call site does
 * not need an ifdef around it. */
void switch_exception_handler_arm(int) {}

#endif  // __SWITCH__
