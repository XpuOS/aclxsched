#pragma once

#include "xsched/preempt/xmem/xmem.h"
#include "xsched/preempt/hal/hw_mem.h"
#include "xsched/ascend/hal/driver.h"

namespace xsched::ascend
{

class AclMemoryRequest : public preempt::HwMemoryRequest
{
public:
    AclMemoryRequest(size_t size, aclrtMemMallocPolicy policy)
        : HwMemoryRequest(size), kPolicy(policy) {}
    virtual ~AclMemoryRequest() = default;

    aclrtMemMallocPolicy Policy() const { return kPolicy; }

private:
    const aclrtMemMallocPolicy kPolicy;
};

class AclMemoryManager : public preempt::HwMemoryManager
{
public:
    AclMemoryManager(aclrtContext ctx);
    virtual ~AclMemoryManager() = default;

    static std::shared_ptr<preempt::XMemoryManager> GetCurrentXMemoryManager();
    static std::shared_ptr<preempt::XMemoryManager> GetXMemoryManager(aclrtContext ctx);

    virtual void *AllocatePagable(std::shared_ptr<preempt::HwMemoryRequest> req) override;
    virtual void Free(void *ptr) override;
    virtual void Advise(std::shared_ptr<utils::Region> mem, XMemoryAdviseType advise) override;

private:
    aclrtContext context_ = nullptr;

    static std::mutex g_acl_xmm_mtx_;
    static std::map<aclrtContext, std::shared_ptr<preempt::XMemoryManager>> g_acl_xmms_;
};

} // namespace xsched::ascend
