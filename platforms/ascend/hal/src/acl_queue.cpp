#include "xsched/protocol/device.h"
#include "xsched/ascend/hal.h"
#include "xsched/ascend/hal/driver.h"
#include "xsched/ascend/hal/options.h"
#include "xsched/ascend/hal/acl_queue.h"
#include "xsched/ascend/hal/acl_mem.h"
#include "xsched/ascend/hal/acl_assert.h"
#include "xsched/ascend/hal/acl_command.h"

#include "xsched/ascend/hal/xvmm.h"
#include <typeinfo>
#include <vector>

using namespace xsched::ascend;
using namespace xsched::preempt;
using namespace xsched::protocol;

AclQueue::AclQueue(aclrtStream stream): kStream(stream)
{
    ACL_ASSERT(Driver::rtGetCurrentContext(&context_));
    ACL_ASSERT(Driver::rtGetDevice(&device_id_));
    device_ = MakeDevice(kDeviceTypeNPU, XDeviceId(device_id_));
    if (GetAscendXvmmEnabled()) xmm_ = AclMemoryManager::GetXMemoryManager(context_);
}

void AclQueue::EnsureCurrentDeviceAndContext(const char *reason)
{
    int32_t cur_device = -1;
    aclError dev_ret = Driver::rtGetDevice(&cur_device);
    if (dev_ret != ACL_SUCCESS || cur_device != device_id_) {
        if (dev_ret != ACL_SUCCESS) {
            XWARN("%s: failed to query current device, ret=%d, set stream device %d",
                  reason, dev_ret, device_id_);
        } else {
            XWARN("%s: stream device (%d) != current device (%d), override current",
                  reason, device_id_, cur_device);
        }
        ACL_ASSERT(Driver::rtSetDevice(device_id_));
    }

    aclrtContext cur_ctx = nullptr;
    aclError ret = Driver::rtGetCurrentContext(&cur_ctx);
    if (ret == ACL_SUCCESS && cur_ctx == context_) return;

    if (ret == ACL_ERROR_RT_CONTEXT_NULL) {
        XDEBG("%s: current context is null, set stream context %p", reason, context_);
    } else if (ret != ACL_SUCCESS) {
        XWARN("%s: failed to query current context, ret=%d, set stream context %p",
              reason, ret, context_);
    } else {
        XWARN("%s: stream context (%p) != current context (%p), override current",
              reason, context_, cur_ctx);
    }
    ACL_ASSERT(Driver::rtSetCurrentContext(context_));
}

void AclQueue::Launch(std::shared_ptr<preempt::HwCommand> hw_cmd)
{
    auto acl_cmd = std::dynamic_pointer_cast<AclCommand>(hw_cmd);
    XASSERT(acl_cmd != nullptr, "hw_cmd is not an AclCommand");
    EnsureCurrentDeviceAndContext("AclQueue::Launch");
    aclError ret = acl_cmd->LaunchWrapper(kStream);
    if (UNLIKELY(ret != ACL_SUCCESS)) {
        XWARN("[ACL_QUEUE] launch_failed cmd_type=%s cmd=%p idx=%ld props=%ld "
              "stream=%p stored_ctx=%p device=%d ret=%d",
              typeid(*acl_cmd).name(), acl_cmd.get(),
              static_cast<long>(acl_cmd->GetIdx()),
              static_cast<long>(acl_cmd->GetProps()), kStream, context_, device_id_, ret);
    }
    if (GetAscendXvmmEnabled()) XvmmCheck(ret, "device command");
    ACL_ASSERT(ret);
}

aclError AclQueue::DirectLaunch(std::shared_ptr<AclCommand> command, aclrtStream stream)
{
    command->BeforeLaunch();
    if (GetAscendXvmmEnabled()) {
        auto manager = AclMemoryManager::GetCurrentXMemoryManager();
        command->Predict(kMemoryContextBuffer, manager);
        command->HoldMemory(XvmmAcquire(manager, command->GetMemoryContext()));
    }

    const aclError result = command->LaunchWrapper(stream);
    // Keep command-owned host inputs alive through device completion, even
    // when the access set has no managed allocation. A failed native call may
    // also have submitted work, so finish it before releasing references.
    XvmmCheck(Driver::rtSynchronizeStream(stream), "direct command completion");
    command->OnCompleted();
    return result;
}

void AclQueue::Synchronize()
{
    std::vector<std::shared_ptr<AclCommand>> tails;
    {
        std::lock_guard<std::mutex> lock(batch_mutex_);
        for (const auto &entry : batch_tails_) {
            if (auto tail = entry.lock()) tails.push_back(std::move(tail));
        }
    }
    if (!tails.empty()) tails.back()->WaitUntil(kCommandStateInFlight);
    aclrtContext before_ctx = nullptr;
    aclError before_ret = Driver::rtGetCurrentContext(&before_ctx);
    EnsureCurrentDeviceAndContext("AclQueue::Synchronize");
    aclrtContext ensured_ctx = nullptr;
    aclError ensured_ret = Driver::rtGetCurrentContext(&ensured_ctx);
    aclError ret = Driver::rtSynchronizeStream(kStream);
    if (ret == ACL_SUCCESS) {
        // Concurrent synchronizers retain the same captured prefix until all
        // its Unrefs finish; a later batch is not retired by this snapshot.
        for (const auto &tail : tails) tail->OnCompleted();
    }
    aclrtContext after_ctx = nullptr;
    aclError after_ret = Driver::rtGetCurrentContext(&after_ctx);
    if (ret != ACL_SUCCESS) {
        XWARN("[ACL_QUEUE] sync_failed stream=%p stored_ctx=%p before_ret=%d before_ctx=%p "
              "ensured_ret=%d ensured_ctx=%p final_ret=%d "
              "after_ret=%d after_ctx=%p device=%d",
              kStream, context_, before_ret, before_ctx, ensured_ret, ensured_ctx,
              ret, after_ret, after_ctx, device_id_);
    }
    if (GetAscendXvmmEnabled()) XvmmCheck(ret, "device command");
    ACL_ASSERT(ret);
}

void AclQueue::OnXQueueCreate()
{
    EnsureCurrentDeviceAndContext("AclQueue::OnXQueueCreate");
}

EXPORT_C_FUNC XResult AclQueueCreate(HwQueueHandle *hwq, aclrtStream stream)
{
    if (hwq == nullptr) {
        XWARN("AclQueueCreate failed: hwq is nullptr");
        return kXSchedErrorInvalidValue;
    }
    if (stream == nullptr) {
        XWARN("AclQueueCreate failed: stream is nullptr");
        return kXSchedErrorInvalidValue;
    }

    HwQueueHandle hwq_h = GetHwQueueHandle(stream);
    auto res = HwQueueManager::Add(hwq_h, [&]() { return std::make_shared<AclQueue>(stream); });
    if (res == kXSchedSuccess) *hwq = hwq_h;
    return res;
}

bool AclQueue::UsesBatchMemory() const
{
    return GetAscendXvmmEnabled();
}

std::shared_ptr<void> AclQueue::PrepareBatch(const CommandLog &commands)
{
    EnsureCurrentDeviceAndContext("AclQueue::PrepareBatch");
    utils::Region memory;
    for (const auto &command : commands) {
        if (auto access = command->GetMemoryContext()) {
            memory.Merge(access->reads);
            memory.Merge(access->writes);
        }
    }
    return XvmmAcquire(xmm_, memory);
}
void AclQueue::LaunchPrepared(std::shared_ptr<HwCommand> command, std::shared_ptr<void> references)
{
    auto acl_command = std::dynamic_pointer_cast<AclCommand>(command);
    XASSERT(acl_command, "batch command is not an AclCommand");
    if (references) {
        acl_command->HoldMemory(std::move(references));
        std::lock_guard<std::mutex> lock(batch_mutex_);
        while (!batch_tails_.empty()) {
            auto tail = batch_tails_.front().lock();
            if (tail && tail->GetState() < kCommandStateCompleted) break;
            batch_tails_.pop_front();
        }
        batch_tails_.push_back(acl_command);
    }
    Launch(command);
}
