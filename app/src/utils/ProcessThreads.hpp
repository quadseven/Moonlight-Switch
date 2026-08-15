#pragma once

#include <cstdint>

/**
 * Counts every thread in the process, not just the ones we can see.
 *
 * Why this exists
 * ---------------
 * On 2026-08-15 `pthread_create` returned ENOMEM while the heap was at 2% of
 * its ceiling (67.8 of 3285 MiB) and moonlight-common-c held 11 threads. Both
 * of those were measured, both were flat, and both are therefore ruled out.
 * Whatever ran out is something neither gauge can see.
 *
 * `moonlight.common_threads_active` only counts moonlight-common-c's threads.
 * borealis, FFmpeg, the OTLP worker, the render thread and libnx's internals
 * are all invisible to it, and the wall may be Horizon's per-process thread
 * limit rather than any kind of memory.
 *
 * How
 * ---
 * `-Wl,--wrap=pthread_create`. The linker redirects every reference to
 * `pthread_create` in the whole link, including the ones inside static
 * archives we do not compile, so this sees threads created by code we never
 * touch. That is the entire point: the threads we already know about are the
 * ones already exonerated.
 *
 * The count is maintained by substituting the caller's start routine with a
 * trampoline that decrements on the way out.
 *
 * Known gap, stated rather than hidden: a thread leaving via `pthread_exit()`
 * rather than by returning skips the decrement, so the live count would drift
 * upward. Nothing in this app's own code does that, but a static library might,
 * and if the live count climbs while `threads_started - threads_finished`
 * disagrees, suspect this before believing a leak.
 */

/** Live threads right now: started and not yet returned. */
int64_t process_threads_live();

/** Every thread ever created through pthread_create in this process. */
int64_t process_threads_started();

/** Threads that returned normally. live == started - finished, absent the gap above. */
int64_t process_threads_finished();

/** Times pthread_create itself failed. Non-zero is the failure being chased. */
int64_t process_thread_create_failures();

/**
 * Live count sampled inside the failing pthread_create call, or -1 if it has
 * not failed yet.
 *
 * This is the number the whole exercise is for. The gauges are sampled every
 * ten seconds and the app dies about four seconds after the failure, so a
 * periodic sample is not guaranteed to survive to be shipped. This one is
 * captured at the instant of the failure and then reported by every later
 * flush, so it reaches the backend if the process lives even one more cycle.
 */
int64_t process_threads_live_at_last_failure();
