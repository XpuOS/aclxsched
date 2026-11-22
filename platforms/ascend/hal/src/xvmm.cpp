#include "xsched/ascend/hal/xvmm.h"
#include "xsched/ascend/hal/acl_mem.h"
#include "xsched/ascend/hal/acl_assert.h"
#include "xsched/preempt/xqueue/xqueue.h"
#include <cstdio>
#include <cstdlib>
#ifdef XSCHED_HAS_XVMM
#include <xvmm.h>
#endif
using namespace xsched::ascend;
using namespace xsched::preempt;
void xsched::ascend::XvmmCheck(int result, const char *operation)
{
    if (result == 0) return;
    std::fprintf(stderr, "XVMM %s failed: status=%d; request is not replayed\n", operation, result);
    std::fflush(stderr);
    // Failure may occur on a worker while other threads own CANN locks or
    // device work. Global C++/CANN teardown from that thread can deadlock.
    // Process teardown releases the context; the service reclaims the peer.
    std::_Exit(EXIT_FAILURE);
}
#ifdef XSCHED_HAS_XVMM
namespace {
struct References {
    std::vector<XVMMRange> ranges;
    ~References() { if (!ranges.empty()) XvmmCheck(xvmm_unref_batch(ranges.data(), ranges.size()), "UnrefBatch"); }
};

}
#endif
std::shared_ptr<void> xsched::ascend::XvmmAcquire(std::shared_ptr<XMemoryManager> manager,
                                               std::shared_ptr<MemorySet> memory)
{
    if (!GetAscendXvmmEnabled() || !manager || !memory) return {};
    utils::Region all = memory->reads;
    all.Merge(memory->writes);
    return XvmmAcquire(std::move(manager), all);
}
std::shared_ptr<void> xsched::ascend::XvmmAcquire(std::shared_ptr<XMemoryManager> manager,
                                               const utils::Region &memory)
{
    if (!GetAscendXvmmEnabled() || !manager) return {};
#ifdef XSCHED_HAS_XVMM
    auto lease = std::make_shared<References>();
    std::vector<XVMMRange> ranges;
    const auto intersections = manager->Intersect(memory);
    ranges.reserve(intersections.size());
    for (auto [ptr, size] : intersections) ranges.push_back({ptr, size});
    if (ranges.empty()) return {};
    XvmmCheck(xvmm_ref_batch(ranges.data(), ranges.size()), "RefBatch");
    lease->ranges = std::move(ranges);
    return lease;
#else
    (void)memory;
    return {};
#endif
}
std::shared_ptr<void> xsched::ascend::XvmmAcquireRange(void *dst, const void *src, size_t bytes)
{
    if (!GetAscendXvmmEnabled() || !bytes) return {};
    utils::Region memory;
    for (auto ptr : {static_cast<const void *>(dst), src}) {
        auto b = reinterpret_cast<uintptr_t>(ptr);
        XASSERT(!b || bytes <= UINTPTR_MAX - b, "XVMM access range overflow");
        if (ptr) memory.Merge(b, b + bytes);
    }
    return XvmmAcquire(AclMemoryManager::GetCurrentXMemoryManager(), memory);
}
void xsched::ascend::XvmmFinish()
{
#ifdef XSCHED_HAS_XVMM
    if (!GetAscendXvmmEnabled()) return;
    std::vector<std::pair<XQueueHandle, HwQueueHandle>> queues;
    XQueueManager::ForEach([&](auto queue) {
        queues.emplace_back(queue->GetHandle(), queue->GetHwQueue()->GetHandle());
        return kXSchedSuccess;
    });
    for (auto [queue, hardware] : queues) {
        XASSERT(XQueueManager::Del(queue) == kXSchedSuccess, "context teardown could not destroy XQueue");
        XASSERT(HwQueueManager::Del(hardware) == kXSchedSuccess, "context teardown could not destroy HwQueue");
    }
    XVMMStats stats{};
    XvmmCheck(xvmm_get_stats(&stats), "GetStats");
    if (const char *path = std::getenv("XSCHED_XVMM_STATS")) {
        FILE *out = std::fopen(path, "w");
        XASSERT(out, "cannot open XVMM statistics file");
        std::fprintf(out, "{\"reserved_bytes\":%lu,\"peak_reserved_bytes\":%lu,\"live_physical_bytes\":%lu,\"peak_physical_bytes\":%lu,\"allocated_bytes\":%lu,\"freed_bytes\":%lu,\"d2h_bytes\":%lu,\"h2d_bytes\":%lu,\"ref_requests\":%lu,\"unref_requests\":%lu,\"ref_wait_ns\":%lu,\"unref_wait_ns\":%lu}\n",
            stats.reserved_bytes, stats.peak_reserved_bytes, stats.live_physical_bytes, stats.peak_physical_bytes,
            stats.allocated_bytes, stats.freed_bytes, stats.device_to_host_bytes, stats.host_to_device_bytes,
            stats.ref_requests, stats.unref_requests, stats.ref_wait_ns, stats.unref_wait_ns);
        std::fclose(out);
    }
    XASSERT(stats.reserved_bytes == 0 && stats.live_physical_bytes == 0,
            "XVMM context teardown with live allocations; free all managed memory before destroying the context");
    XvmmCheck(xvmm_shutdown(), "Shutdown");
#endif
}
