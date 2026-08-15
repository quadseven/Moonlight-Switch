#include "ProcessHealth.hpp"

#include "OtlpMetricsExporter.hpp"

#ifdef __SWITCH__
#include <malloc.h>
#include <switch.h>

#include <Limelight.h>
#endif

ProcessHealthSample process_health_read() {
    ProcessHealthSample sample;

#ifdef __SWITCH__
    /* newlib walks its free list under the allocator lock to answer this. Once
     * per flush interval that is nothing; on the update path it would not be,
     * which is why this is sampled by the exporter's worker rather than
     * anywhere in the video or input path. */
    const struct mallinfo info = mallinfo();
    sample.heapValid = true;
    sample.heapArenaBytes = static_cast<int64_t>(info.arena);
    sample.heapUsedBytes = static_cast<int64_t>(info.uordblks);

    /* The allocator only knows about the heap. Thread stacks, the graphics
     * driver's mappings and the socket transfer memory are all outside it and
     * all count against the same process limit, so ask the kernel too. When
     * these two disagree about where the memory went, the difference is the
     * answer. */
    u64 used = 0;
    u64 total = 0;
    if (R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) &&
        R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0))) {
        sample.memValid = true;
        sample.memUsedBytes = static_cast<int64_t>(used);
        sample.memTotalBytes = static_cast<int64_t>(total);
    }

    sample.threadsValid = true;
    sample.libraryThreads = LiGetActiveThreadCount();
#endif

    return sample;
}

void process_health_publish(const ProcessHealthSample& sample) {
    OtlpMetricsExporter& metrics = OtlpMetricsExporter::instance();

    /* Each block is guarded separately. svcGetInfo failing is not a reason to
     * stop reporting the heap.
     *
     * Skipping rather than publishing zero, because a gauge repeats its current
     * value every interval: writing nothing holds the last real reading, while
     * writing zero puts a step down to the floor in the middle of the chart.
     * heap_used falling to nothing looks exactly like memory being released,
     * which is the opposite of what a console about to run out is doing. */
    if (sample.heapValid) {
        metrics.gauge("moonlight.heap_arena_bytes", sample.heapArenaBytes);
        metrics.gauge("moonlight.heap_used_bytes", sample.heapUsedBytes);
    }

    if (sample.memValid) {
        metrics.gauge("moonlight.mem_used_bytes", sample.memUsedBytes);
        metrics.gauge("moonlight.mem_total_bytes", sample.memTotalBytes);
    }

    if (sample.threadsValid) {
        metrics.gauge("moonlight.common_threads_active", sample.libraryThreads);
    }
}

void process_health_sample() { process_health_publish(process_health_read()); }
