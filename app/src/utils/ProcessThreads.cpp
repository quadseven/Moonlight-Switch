#include "ProcessThreads.hpp"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <pthread.h>

namespace {

std::atomic<int64_t> g_live{0};
std::atomic<int64_t> g_started{0};
std::atomic<int64_t> g_finished{0};
std::atomic<int64_t> g_failures{0};
std::atomic<int64_t> g_live_at_failure{-1};

struct StartArgs {
    void* (*entry)(void*);
    void* arg;
};

/*
 * Substituted for the caller's start routine so the count comes down again.
 *
 * The StartArgs block is freed before the real entry point runs rather than
 * after it returns: a thread that never returns would otherwise leak it, and
 * the whole reason this file exists is a suspected resource leak, so leaking to
 * measure one would be its own joke.
 */
void* trampoline(void* raw) {
    StartArgs args = *static_cast<StartArgs*>(raw);
    std::free(raw);

    void* result = args.entry(args.arg);

    g_live.fetch_sub(1, std::memory_order_relaxed);
    g_finished.fetch_add(1, std::memory_order_relaxed);
    return result;
}

} // namespace

extern "C" {

/* Provided by the linker under -Wl,--wrap=pthread_create. */
int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*entry)(void*), void* arg);

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*entry)(void*), void* arg) {
    StartArgs* args = static_cast<StartArgs*>(std::malloc(sizeof(StartArgs)));
    if (!args) {
        /* Out of memory before we even asked for a thread. Counted as a
         * failure because from the caller's side that is what it is, and a
         * silent difference between "we could not measure it" and "it did not
         * happen" is exactly what this file is here to remove. */
        g_failures.fetch_add(1, std::memory_order_relaxed);
        g_live_at_failure.store(g_live.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        return ENOMEM;
    }
    args->entry = entry;
    args->arg = arg;

    /* Incremented before the call, not inside the trampoline. The thread may
     * not be scheduled for some time after pthread_create returns, and a count
     * that lags the thing it counts would read low at exactly the moment the
     * limit is hit, which is the only moment that matters. */
    g_live.fetch_add(1, std::memory_order_relaxed);

    const int rc = __real_pthread_create(thread, attr, trampoline, args);
    if (rc != 0) {
        /* Sampled here, inside the failure, because the periodic gauge is
         * every ten seconds and the app has historically died about four
         * seconds later. A number that is only correct in a sample that never
         * ships is not a measurement. */
        g_live.fetch_sub(1, std::memory_order_relaxed);
        g_failures.fetch_add(1, std::memory_order_relaxed);
        g_live_at_failure.store(g_live.load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
        std::free(args);
        return rc;
    }

    g_started.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

} // extern "C"

int64_t process_threads_live() {
    return g_live.load(std::memory_order_relaxed);
}

int64_t process_threads_started() {
    return g_started.load(std::memory_order_relaxed);
}

int64_t process_threads_finished() {
    return g_finished.load(std::memory_order_relaxed);
}

int64_t process_thread_create_failures() {
    return g_failures.load(std::memory_order_relaxed);
}

int64_t process_threads_live_at_last_failure() {
    return g_live_at_failure.load(std::memory_order_relaxed);
}
