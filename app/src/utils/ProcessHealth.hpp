#pragma once

#include <cstdint>

/**
 * The resources a Switch process runs out of, sampled and published as gauges.
 *
 * Why this exists
 * ---------------
 * On 2026-08-14 this app wedged the console after a fifteen hour sleep. The
 * first thing to fail was pthread_create() returning ENOMEM, followed 4ms later
 * by EFAULT from three unrelated socket paths, which on libnx move their data
 * through a pool drawn from the same process heap. Everything about that says
 * the heap was gone, and nothing in this app measured the heap.
 *
 * Forty hours of streaming and two long sleeps produced three metrics:
 * connection_terminated, reconnect_attempts and active_session_present. The one
 * quantity that would have shown the failure coming was not among them, so the
 * only evidence of a multi-day trend was a single instant at the end of it.
 *
 * Everything here is expressed as an amount USED, never an amount free
 * -------------------------------------------------------------------
 * The exporter records each gauge's last value and its peak, and the peak is
 * the useful half: a ten second sample interval will not catch a transient by
 * its instantaneous value. For exhaustion the dangerous extreme is the most
 * consumed, so "used" makes peak mean worst case. A free-memory series would
 * report its peak as the moment the most memory was available, which is the
 * opposite of the question. Free is arena minus used, and a dashboard can
 * subtract.
 */
struct ProcessHealthSample {
    /* False when the platform has no way to answer, so a reading of zero is
       never mistaken for an empty heap. */
    bool heapValid = false;
    /* Total the allocator has taken from the system. Grows and does not shrink,
       so it is the ceiling fragmentation is measured against. */
    int64_t heapArenaBytes = 0;
    /* In allocated blocks right now. */
    int64_t heapUsedBytes = 0;

    bool memValid = false;
    /* What the kernel says the process occupies, which includes thread stacks,
       code and mapped memory the allocator never sees. */
    int64_t memUsedBytes = 0;
    int64_t memTotalBytes = 0;

    bool threadsValid = false;
    /* Threads moonlight-common-c is holding. A count that does not return to
       its floor across a reconnect is a leak, visible long before the
       allocation that finally fails. */
    int64_t libraryThreads = 0;
};

/** Reads the platform. Returns an all-invalid sample where it cannot. */
ProcessHealthSample process_health_read();

/**
 * Writes a sample to the metrics exporter. Separate from the read so it can be
 * driven with values a real console would take days to reach.
 */
void process_health_publish(const ProcessHealthSample& sample);

/** Read and publish. Called from the exporter's flush loop. */
void process_health_sample();
