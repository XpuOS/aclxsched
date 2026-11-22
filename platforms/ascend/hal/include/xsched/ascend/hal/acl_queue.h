#pragma once

#include <list>

#include "xsched/types.h"
#include "xsched/ascend/hal/acl.h"
#include "xsched/ascend/hal/handle.h"
#include "xsched/preempt/hal/hw_queue.h"

namespace xsched::ascend
{

class AclCommand;

class AclQueue : public preempt::HwQueue
{
public:
    AclQueue(aclrtStream stream);
    virtual ~AclQueue() = default;

    // Execute a single-command batch without an XQueue, including the default
    // stream. Finish device use before releasing references and command inputs.
    static aclError DirectLaunch(std::shared_ptr<AclCommand> command, aclrtStream stream);

    virtual void Launch(std::shared_ptr<preempt::HwCommand> hw_cmd) override;
    virtual bool UsesBatchMemory() const override;
    virtual std::shared_ptr<void> PrepareBatch(const preempt::CommandLog &) override;
    virtual void LaunchPrepared(std::shared_ptr<preempt::HwCommand>, std::shared_ptr<void>) override;
    virtual void Synchronize() override;
    virtual void OnXQueueCreate() override;

    virtual XDevice       GetDevice()            override { return device_; }
    virtual HwQueueHandle GetHandle()            override { return GetHwQueueHandle(kStream); }
    virtual bool          SupportDynamicLevel()  override { return false; }
    virtual XPreemptLevel GetMaxSupportedLevel() override { return kPreemptLevelBlock; }
    virtual std::shared_ptr<preempt::XMemoryManager> GetXMemoryManager() override { return xmm_; }

private:
    void EnsureCurrentDeviceAndContext(const char *reason);

    const aclrtStream kStream;
    XDevice           device_;
    int32_t           device_id_ = 0;
    aclrtContext      context_ = nullptr;
    std::shared_ptr<preempt::XMemoryManager> xmm_ = nullptr;
    std::mutex batch_mutex_;
    std::list<std::weak_ptr<AclCommand>> batch_tails_;
};

} // namespace xsched::ascend
