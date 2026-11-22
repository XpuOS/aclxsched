#include "xsched/ascend/hal/xvmm.h"
#ifdef XSCHED_HAS_XVMM
#include <xvmm.h>
#endif

#include "xsched/utils/log.h"
#include "xsched/ascend/hal/acl_mem.h"
#include "xsched/ascend/hal/acl_assert.h"

using namespace xsched::ascend;
using namespace xsched::preempt;

#ifdef XSCHED_HAS_XVMM
namespace
{

void EnsureCurrentContext(aclrtContext target)
{
    XASSERT(target != nullptr, "AclMemoryManager requires a valid context");
    aclrtContext current = nullptr;
    aclError ret = Driver::rtGetCurrentContext(&current);
    if (ret == ACL_SUCCESS && current == target) return;
    ACL_ASSERT(Driver::rtSetCurrentContext(target));
}

} // namespace
#endif

std::mutex AclMemoryManager::g_acl_xmm_mtx_;
std::map<aclrtContext, std::shared_ptr<XMemoryManager>> AclMemoryManager::g_acl_xmms_;

std::shared_ptr<XMemoryManager> AclMemoryManager::GetCurrentXMemoryManager()
{
    aclrtContext current_ctx = nullptr;
    ACL_ASSERT(Driver::rtGetCurrentContext(&current_ctx));
    XASSERT(current_ctx != nullptr, "AclMemoryManager requires a current Ascend context");
    return GetXMemoryManager(current_ctx);
}

std::shared_ptr<XMemoryManager> AclMemoryManager::GetXMemoryManager(aclrtContext ctx)
{
    XASSERT(ctx != nullptr, "AclMemoryManager requires a valid Ascend context");
    std::lock_guard<std::mutex> lock(g_acl_xmm_mtx_);
    auto it = g_acl_xmms_.find(ctx);
    if (it != g_acl_xmms_.end()) return it->second;
    auto acl_mm = std::make_shared<AclMemoryManager>(ctx);
    auto acl_xmm = std::make_shared<XMemoryManager>(acl_mm);
    g_acl_xmms_[ctx] = acl_xmm;
    return acl_xmm;
}

AclMemoryManager::AclMemoryManager(aclrtContext ctx): context_(ctx)
{
    aclrtContext current_ctx = nullptr;
    ACL_ASSERT(Driver::rtGetCurrentContext(&current_ctx));
    XASSERT(current_ctx == ctx,
            "create AclMemoryManager failed: current context (%p) does not match context (%p)",
            current_ctx, ctx);
}

void *AclMemoryManager::AllocatePagable(std::shared_ptr<HwMemoryRequest> req)
{
    auto acl_req = std::dynamic_pointer_cast<AclMemoryRequest>(req);
    XASSERT(acl_req != nullptr, "memory request is not an AclMemoryRequest");

    void *ptr = nullptr;
#ifdef XSCHED_HAS_XVMM
    if (GetAscendXvmmEnabled()) {
        EnsureCurrentContext(context_);
        XvmmCheck(xvmm_malloc(acl_req->Size(), &ptr), "Malloc");
        return ptr;
    }
#endif
    ACL_ASSERT(Driver::rtMalloc(&ptr, acl_req->Size(), acl_req->Policy()));
    return ptr;
}

void AclMemoryManager::Free(void *ptr)
{
#ifdef XSCHED_HAS_XVMM
    if (GetAscendXvmmEnabled()) { XvmmCheck(xvmm_free(ptr), "Free"); return; }
#endif
    ACL_ASSERT(Driver::rtFree(ptr));
}

void AclMemoryManager::Advise(std::shared_ptr<utils::Region> mem, XMemoryAdviseType advise)
{
#ifdef XSCHED_HAS_XVMM
    if (!GetAscendXvmmEnabled() || !mem) return;
    if (advise != kMemoryAdviseWillNeed && advise != kMemoryAdviseLoad) return;
    EnsureCurrentContext(context_);
    // Region merging can join adjacent allocations. Split them again at
    // their logical bounds: the SDK itself rounds to 2 MiB pages safely.
    const auto allocations = GetXMemoryManager(context_)->Intersect(*mem);
    std::vector<XVMMRange> ranges;
    ranges.reserve(allocations.size());
    for (auto [ptr, size] : allocations) ranges.push_back({ptr, size});
    if (ranges.empty()) return;
    const auto hinted = xvmm_hint_batch(ranges.data(), ranges.size());
    if (hinted != XVMM_SUCCESS) {
        if (hinted != XVMM_ERROR_BUSY && hinted != XVMM_ERROR_GONE)
            XWARN("XVMM memory hint failed: %d", static_cast<int>(hinted));
        return;
    }
    if (advise == kMemoryAdviseLoad) {
        const auto result = xvmm_prefetch_batch_async(ranges.data(), ranges.size());
        if (result != XVMM_SUCCESS && result != XVMM_ERROR_BUSY && result != XVMM_ERROR_GONE)
            XWARN("XVMM asynchronous prefetch submission failed: %d", static_cast<int>(result));
    }
#else
    UNUSED(mem);
    UNUSED(advise);
#endif
}
