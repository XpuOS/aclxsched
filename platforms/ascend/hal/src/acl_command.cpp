#include "xsched/utils/xassert.h"
#include "xsched/utils/log.h"
#include "xsched/ascend/hal/event_pool.h"
#include "xsched/ascend/hal/acl_command.h"
#include "xsched/ascend/hal/native.h"
#include "xsched/ascend/hal/options.h"

#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

using namespace xsched::ascend;
thread_local unsigned xsched::ascend::native_call_depth = 0;

namespace
{

bool MergeBufferRange(void *addr, size_t size,
                      xsched::preempt::MemorySet &mem_ctx,
                      bool is_write)
{
    if (addr == nullptr || size == 0) return false;

    const uint64_t begin = reinterpret_cast<uint64_t>(addr);
    uint64_t end = begin + static_cast<uint64_t>(size);
    if (end < begin) end = UINT64_MAX;

    if (is_write) {
        mem_ctx.writes.Merge(begin, end);
    } else {
        mem_ctx.reads.Merge(begin, end);
    }
    return true;
}

void ReadDataBufferRange(const std::shared_ptr<DataBuffer> &data_buffer,
                         void **addr, size_t *size)
{
    if (addr != nullptr) *addr = nullptr;
    if (size != nullptr) *size = 0;
    if (data_buffer == nullptr || data_buffer->buffer() == nullptr) return;

    void *snapshot_addr = Driver::GetDataBufferAddr(data_buffer->buffer());
    size_t snapshot_size = Driver::GetDataBufferSizeV2(data_buffer->buffer());
    if (addr != nullptr) *addr = snapshot_addr;
    if (size != nullptr) *size = snapshot_size;
}

aclDataBuffer *SnapshotDataBuffer(const std::shared_ptr<DataBuffer> &data_buffer,
                                  void *snapshot_addr, size_t snapshot_size)
{
    if (data_buffer == nullptr || data_buffer->buffer() == nullptr) return nullptr;

    aclDataBuffer *snapshot = Driver::CreateDataBuffer(snapshot_addr, snapshot_size);
    if (snapshot == nullptr) {
        XWARN("failed to snapshot aclDataBuffer %p addr=%p size=%zu, fallback to live buffer",
              data_buffer->buffer(), snapshot_addr, snapshot_size);
    }
    return snapshot;
}

bool LooksLikeHostPointer(void *addr)
{
    uintptr_t raw = reinterpret_cast<uintptr_t>(addr);
    // AArch64 host memory can occupy the upper half of the user VA range.
    if (raw == 0) return false;

    // Ascend maps device virtual address space into the process through
    // /dev/davinci_manager. The mapping is visible in /proc/self/maps but is
    // not safe to read with CPU memcpy.
    if (raw >= 0x00000e0000000000ULL && raw < 0x0000180000000000ULL) {
        return false;
    }
    return true;
}

bool RangeEnd(void *addr, size_t size, uintptr_t *end)
{
    if (addr == nullptr || size == 0 || end == nullptr) return false;
    uintptr_t begin = reinterpret_cast<uintptr_t>(addr);
    uintptr_t range_end = begin + static_cast<uintptr_t>(size);
    if (range_end < begin) return false;
    *end = range_end;
    return true;
}

bool RangesOverlap(void *lhs_addr, size_t lhs_size, void *rhs_addr, size_t rhs_size)
{
    uintptr_t lhs_end = 0;
    uintptr_t rhs_end = 0;
    if (!RangeEnd(lhs_addr, lhs_size, &lhs_end) || !RangeEnd(rhs_addr, rhs_size, &rhs_end)) {
        return false;
    }

    uintptr_t lhs_begin = reinterpret_cast<uintptr_t>(lhs_addr);
    uintptr_t rhs_begin = reinterpret_cast<uintptr_t>(rhs_addr);
    return lhs_begin < rhs_end && rhs_begin < lhs_end;
}

bool OverlapsAnyOutputRange(void *addr, size_t size,
                            const std::vector<void *> &output_addrs,
                            const std::vector<size_t> &output_sizes)
{
    for (size_t i = 0; i < output_addrs.size(); ++i) {
        if (RangesOverlap(addr, size, output_addrs[i], output_sizes[i])) return true;
    }
    return false;
}

bool IsReadableHostRange(void *addr, size_t size)
{
    uintptr_t end = 0;
    if (!RangeEnd(addr, size, &end)) return false;
    if (!LooksLikeHostPointer(addr)) return false;

    FILE *fp = std::fopen("/proc/self/maps", "r");
    if (fp == nullptr) return false;

    uintptr_t covered = reinterpret_cast<uintptr_t>(addr);
    char line[512];
    while (std::fgets(line, sizeof(line), fp) != nullptr) {
        unsigned long long start = 0;
        unsigned long long stop = 0;
        char perms[5] = {};
        char path[256] = {};
        int fields = std::sscanf(line, "%llx-%llx %4s %*s %*s %*s %255s",
                                 &start, &stop, perms, path);
        if (fields < 3) continue;
        if (perms[0] != 'r') continue;
        if (fields >= 4 && std::strncmp(path, "/dev/", 5) == 0) continue;
        if (stop <= start) continue;

        uintptr_t map_begin = static_cast<uintptr_t>(start);
        uintptr_t map_end = static_cast<uintptr_t>(stop);
        if (map_end <= covered) continue;
        if (map_begin > covered) continue;

        covered = map_end;
        if (covered >= end) {
            std::fclose(fp);
            return true;
        }
    }

    std::fclose(fp);
    return false;
}

} // namespace

AclNNExecutor::~AclNNExecutor()
{
    NativeCallScope native;
    if (executor_ == nullptr) return;
    NnBase::DestroyAclOpExecutor(executor_);
}

AclCommand::~AclCommand()
{
    if (following_event_ == nullptr) return;
    EventPool::Instance().Push(following_event_);
}

void AclCommand::Synchronize()
{
    XASSERT(following_event_ != nullptr,
            "following_event_ is nullptr, EnableSynchronization() should be called first");
    ACL_ASSERT(Driver::rtSynchronizeEvent(following_event_));
}

void AclCommand::HoldMemory(std::shared_ptr<void> references)
{
    std::lock_guard<std::mutex> lock(completion_mutex_);
    memory_references_ = std::move(references);
}

void AclCommand::OnCompleted()
{
    // SyncAll and an event waiter may confirm the same batch concurrently.
    // Both must finish Unref before either exposes Completed to the caller.
    std::lock_guard<std::mutex> lock(completion_mutex_);
    memory_references_.reset();
}

bool AclCommand::Synchronizable()
{
    return following_event_ != nullptr;
}

bool AclCommand::EnableSynchronization()
{
    if (following_event_ != nullptr) return true;
    following_event_ = (aclrtEvent)EventPool::Instance().Pop();
    return following_event_ != nullptr;
}

aclError AclCommand::LaunchWrapper(aclrtStream stream)
{
    NativeCallScope native;
    aclError launch_ret = Launch(stream);
    OnLaunchComplete(launch_ret);
    if (following_event_ == nullptr) return launch_ret;

    aclError event_ret = Driver::rtRecordEvent(following_event_, stream);
    if (UNLIKELY(launch_ret != ACL_SUCCESS)) {
        if (event_ret != ACL_SUCCESS) {
            XWARN("failed to record completion event after launch failure, stream=%p, "
                  "launch_ret=%d, event_ret=%d",
                  stream, launch_ret, event_ret);
        }
        return launch_ret;
    }
    return event_ret;
}

AclEventRecordCommand::AclEventRecordCommand(aclrtEvent event): event_(event)
{
    XASSERT(event_ != nullptr, "aclrtEvent should not be nullptr");
    this->SetProps(preempt::kCommandPropertyIdempotent);
}

AclMemcpyCommand::AclMemcpyCommand(void *dst, size_t destMax, const void *src, size_t count,
                                   aclrtMemcpyKind kind)
    : dst_(dst), destMax_(destMax), src_(src), count_(count), kind_(kind)
{
    if (kind_ == ACL_MEMCPY_HOST_TO_DEVICE && src_ != nullptr && count_ != 0) {
        staged_src_.resize(count_);
        std::memcpy(staged_src_.data(), src_, count_);
        src_ = staged_src_.data();
    }
}

std::mutex TensorDesc::tensor_desc_mutex_;
std::mutex DataBuffer::data_buffer_mutex_;
std::mutex OpAttr::op_attr_mutex_;
std::mutex AclNNExecutor::executor_mutex_;
std::mutex AclNNResource::resource_mutex_;
std::unordered_map<const aclTensorDesc *, std::shared_ptr<TensorDesc>> TensorDesc::tensor_descs_;
std::unordered_map<const aclDataBuffer *, std::shared_ptr<DataBuffer>> DataBuffer::data_buffers_;
std::unordered_map<const aclopAttr *, std::shared_ptr<OpAttr>> OpAttr::op_attrs_;

namespace
{

using AclNNExecutorMap = std::unordered_map<aclOpExecutor *, std::shared_ptr<AclNNExecutor>>;
using AclNNResourceMap = std::unordered_map<const void *, std::shared_ptr<AclNNResource>>;

AclNNExecutorMap &GetAclNNExecutorMap()
{
    static auto *executors = new AclNNExecutorMap();
    return *executors;
}

AclNNResourceMap &GetAclNNResourceMap()
{
    static auto *resources = new AclNNResourceMap();
    return *resources;
}

auto &GetRetiredAclNNResources()
{
    static auto *resources = new std::unordered_map<const void *, std::weak_ptr<AclNNResource>>;
    return *resources;
}

} // namespace

AclNNResource::~AclNNResource()
{
    NativeCallScope native;
    if (resource_ == nullptr || destroy_fn_ == nullptr) return;
    destroy_fn_(resource_);
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto &retired = GetRetiredAclNNResources();
    auto it = retired.find(resource_);
    if (it != retired.end() && it->second.expired()) retired.erase(it);
}

std::shared_ptr<TensorDesc> TensorDesc::Create(const aclTensorDesc *desc)
{
    std::lock_guard<std::mutex> lock(tensor_desc_mutex_);
    auto it = tensor_descs_.find(desc);
    if (it != tensor_descs_.end()) return it->second;
    auto tensor_desc = std::make_shared<TensorDesc>();
    tensor_desc->desc_ = desc;
    tensor_descs_[desc] = tensor_desc;
    return tensor_desc;
}

bool TensorDesc::Destroy(const aclTensorDesc *desc)
{
    std::unique_lock<std::mutex> lock(tensor_desc_mutex_);
    auto it = tensor_descs_.find(desc);
    if (it == tensor_descs_.end()) return false;
    auto tensor_desc = it->second;
    tensor_descs_.erase(it);
    lock.unlock();
    tensor_desc = nullptr;
    return true;
}

std::shared_ptr<DataBuffer> DataBuffer::Create(const aclDataBuffer *buffer)
{
    std::lock_guard<std::mutex> lock(data_buffer_mutex_);
    auto it = data_buffers_.find(buffer);
    if (it != data_buffers_.end()) return it->second;
    auto data_buffer = std::make_shared<DataBuffer>();
    data_buffer->buffer_ = buffer;
    data_buffers_[buffer] = data_buffer;
    return data_buffer;
}

bool DataBuffer::Destroy(const aclDataBuffer *buffer)
{
    std::unique_lock<std::mutex> lock(data_buffer_mutex_);
    auto it = data_buffers_.find(buffer);
    if (it == data_buffers_.end()) return false;
    auto data_buffer = it->second;
    data_buffers_.erase(it);
    lock.unlock();
    data_buffer = nullptr;
    return true;
}

std::shared_ptr<OpAttr> OpAttr::Create(const aclopAttr *attr)
{
    std::lock_guard<std::mutex> lock(op_attr_mutex_);
    auto it = op_attrs_.find(attr);
    if (it != op_attrs_.end()) return it->second;
    auto op_attr = std::make_shared<OpAttr>();
    op_attr->attr_ = attr;
    op_attrs_[attr] = op_attr;
    return op_attr;
}

bool OpAttr::Destroy(const aclopAttr *attr)
{
    std::unique_lock<std::mutex> lock(op_attr_mutex_);
    auto it = op_attrs_.find(attr);
    if (it == op_attrs_.end()) return false;
    auto op_attr = it->second;
    op_attrs_.erase(it);
    lock.unlock();
    op_attr = nullptr;
    return true;
}

std::shared_ptr<AclNNExecutor> AclNNExecutor::Create(aclOpExecutor *executor)
{
    if (executor == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(executor_mutex_);
    auto &executors = GetAclNNExecutorMap();
    auto it = executors.find(executor);
    if (it != executors.end()) return it->second;
    auto aclnn_executor = std::make_shared<AclNNExecutor>();
    aclnn_executor->executor_ = executor;
    executors[executor] = aclnn_executor;
    return aclnn_executor;
}

void AclNNExecutor::RecordWorkspace(
    aclOpExecutor *executor, AclNNResources resources,
    std::shared_ptr<preempt::MemorySet> mem_ctx)
{
    if (executor == nullptr) return;
    auto replacement = std::make_shared<AclNNExecutor>();
    replacement->executor_ = executor;
    replacement->resources_ = std::move(resources);
    replacement->mem_ctx_ = std::move(mem_ctx);
    std::shared_ptr<AclNNExecutor> previous;
    std::unique_lock<std::mutex> lock(executor_mutex_);
    auto &executors = GetAclNNExecutorMap();
    auto it = executors.find(executor);
    if (it != executors.end()) {
        previous = std::move(it->second);
        // A new first phase identifies a new handle generation.
        previous->executor_ = nullptr;
    }
    executors[executor] = std::move(replacement);
    lock.unlock();
}

aclnnStatus AclNNExecutor::SetRepeatable(aclOpExecutor *executor)
{
    auto wrapper = Create(executor);
    NativeCallScope native;
    aclnnStatus ret = NnBase::SetAclOpExecutorRepeatable(executor);
    if (ret == ACL_SUCCESS && wrapper != nullptr) {
        std::lock_guard<std::mutex> lock(executor_mutex_);
        wrapper->repeatable_ = true;
    }
    return ret;
}

AclNNResources AclNNExecutor::RetainResources()
{
    std::lock_guard<std::mutex> lock(executor_mutex_);
    if (repeatable_) return resources_;
    return std::move(resources_);
}

void AclNNExecutor::DiscardUnlaunched()
{
    aclOpExecutor *executor;
    {
        std::lock_guard<std::mutex> lock(executor_mutex_);
        if (repeatable_ || executor_ == nullptr) return;
        executor = executor_;
        executor_ = nullptr;
        auto &executors = GetAclNNExecutorMap();
        auto it = executors.find(executor);
        if (it != executors.end() && it->second.get() == this) executors.erase(it);
    }
    NativeCallScope native;
    NnBase::DestroyAclOpExecutor(executor);
}

aclnnStatus AclNNExecutor::Execute(AclNNExecuteFunc execute_fn, void *workspace,
                                   uint64_t workspace_size, aclrtStream stream)
{
    if (execute_fn == nullptr) return ACL_ERROR_INVALID_PARAM;
    AclNNResources resources;
    aclOpExecutor *executor;
    {
        std::lock_guard<std::mutex> lock(executor_mutex_);
        executor = executor_;
        if (executor == nullptr) return ACL_ERROR_INVALID_PARAM;
        if (!repeatable_) {
            // The native call consumes a one-shot handle, including cache wraps.
            // Retire this generation before CANN can recycle its address.
            auto &executors = GetAclNNExecutorMap();
            auto it = executors.find(executor);
            if (it != executors.end() && it->second.get() == this) executors.erase(it);
            executor_ = nullptr;
            resources = std::move(resources_);
        }
    }
    NativeCallScope native;
    return execute_fn(workspace, workspace_size, executor, stream);
}

bool AclNNExecutor::Destroy(aclOpExecutor *executor)
{
    if (executor == nullptr) return false;
    std::unique_lock<std::mutex> lock(executor_mutex_);
    auto &executors = GetAclNNExecutorMap();
    auto it = executors.find(executor);
    if (it == executors.end()) return false;
    auto aclnn_executor = it->second;
    executors.erase(it);
    lock.unlock();
    aclnn_executor = nullptr;
    return true;
}

void AclNNResource::Create(const void *resource, DestroyFunc destroy_fn,
                           std::shared_ptr<preempt::MemorySet> mem_ctx,
                           AclNNResources dependencies)
{
    if (resource == nullptr || destroy_fn == nullptr) return;
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto &resources = GetAclNNResourceMap();
    auto it = resources.find(resource);
    if (it != resources.end()) return;
    auto aclnn_resource = std::make_shared<AclNNResource>();
    aclnn_resource->resource_ = resource;
    aclnn_resource->destroy_fn_ = destroy_fn;
    aclnn_resource->mem_ctx_ = std::move(mem_ctx);
    aclnn_resource->dependencies_ = std::move(dependencies);
    resources[resource] = aclnn_resource;
}

bool AclNNResource::Destroy(const void *resource)
{
    if (resource == nullptr) return false;
    std::unique_lock<std::mutex> lock(resource_mutex_);
    auto &resources = GetAclNNResourceMap();
    auto it = resources.find(resource);
    if (it == resources.end()) {
        auto &retired = GetRetiredAclNNResources();
        auto pending = retired.find(resource);
        return pending != retired.end() && !pending->second.expired();
    }
    auto aclnn_resource = it->second;
    GetRetiredAclNNResources()[resource] = aclnn_resource;
    resources.erase(it);
    lock.unlock();
    aclnn_resource = nullptr;
    return true;
}

std::shared_ptr<xsched::preempt::MemorySet> AclNNResource::GetMemoryContext(
    const void *const *resources, size_t count)
{
    return GetMemoryContext(Retain(resources, count));
}

AclNNResources AclNNResource::Retain(const void *const *resources, size_t count)
{
    AclNNResources retained;
    if (resources == nullptr || count == 0) return retained;
    retained.reserve(count);
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto &active = GetAclNNResourceMap();
    auto &retired = GetRetiredAclNNResources();
    for (size_t i = 0; i < count; ++i) {
        std::shared_ptr<AclNNResource> resource;
        auto it = active.find(resources[i]);
        if (it != active.end()) resource = it->second;
        else {
            auto pending = retired.find(resources[i]);
            if (pending != retired.end()) resource = pending->second.lock();
        }
        if (resource != nullptr && std::find(retained.begin(), retained.end(), resource) == retained.end()) {
            retained.push_back(std::move(resource));
        }
    }
    return retained;
}

std::shared_ptr<xsched::preempt::MemorySet> AclNNResource::GetMemoryContext(
    const AclNNResources &resources)
{
    auto mem_ctx = std::make_shared<xsched::preempt::MemorySet>();
    bool has_range = false;
    for (const auto &resource : resources) {
        if (resource->mem_ctx_ == nullptr) continue;
        mem_ctx->reads.Merge(resource->mem_ctx_->reads);
        mem_ctx->writes.Merge(resource->mem_ctx_->writes);
        has_range = true;
    }
    return has_range ? mem_ctx : nullptr;
}

AclOpCompileAndExecuteCommand::AclOpCompileAndExecuteCommand(
    const char *opType,
    std::shared_ptr<std::vector<std::shared_ptr<TensorDesc>>> inputDesc,
    std::shared_ptr<std::vector<std::shared_ptr<DataBuffer>>> inputs,
    std::shared_ptr<std::vector<std::shared_ptr<TensorDesc>>> outputDesc,
    std::shared_ptr<std::vector<std::shared_ptr<DataBuffer>>> outputs,
    std::shared_ptr<OpAttr> attr,
    aclopEngineType engineType, aclopCompileType compileFlag,
    const char *opPath, bool v2)
    : opType_(opType == nullptr ? "" : opType)
    , opType_is_null_(opType == nullptr)
    , inputDesc_(inputDesc)
    , inputs_(inputs)
    , outputDesc_(outputDesc)
    , outputs_(outputs)
    , attr_(attr)
    , engineType_(engineType)
    , compileFlag_(compileFlag)
    , opPath_(opPath == nullptr ? "" : opPath)
    , opPath_is_null_(opPath == nullptr)
    , v2_(v2)
{
    XASSERT(inputDesc_ != nullptr && inputs_ != nullptr &&
            outputDesc_ != nullptr && outputs_ != nullptr,
            "AclOpCompileAndExecuteCommand descriptors and buffers should not be nullptr");
    XASSERT(inputDesc_->size() == inputs_->size(),
            "inputDesc and inputs should have the same size");
    XASSERT(outputDesc_->size() == outputs_->size(),
            "outputDesc and outputs should have the same size");

    input_buffer_snapshots_.resize(inputs_->size(), nullptr);
    input_buffer_addrs_.reserve(inputs_->size());
    input_buffer_sizes_.reserve(inputs_->size());
    input_host_buffer_copies_.resize(inputs_->size());
    input_owns_host_buffer_.resize(inputs_->size(), false);
    for (auto &input : *inputs_) {
        void *addr = nullptr;
        size_t size = 0;
        ReadDataBufferRange(input, &addr, &size);
        input_buffer_addrs_.push_back(addr);
        input_buffer_sizes_.push_back(size);
    }

    output_buffer_snapshots_.reserve(outputs_->size());
    output_buffer_addrs_.reserve(outputs_->size());
    output_buffer_sizes_.reserve(outputs_->size());
    for (auto &output : *outputs_) {
        void *addr = nullptr;
        size_t size = 0;
        ReadDataBufferRange(output, &addr, &size);
        output_buffer_snapshots_.push_back(SnapshotDataBuffer(output, addr, size));
        output_buffer_addrs_.push_back(addr);
        output_buffer_sizes_.push_back(size);
    }

    for (size_t i = 0; i < inputs_->size(); ++i) {
        void *addr = input_buffer_addrs_[i];
        size_t size = input_buffer_sizes_[i];
        void *snapshot_addr = addr;

        const bool host_like = LooksLikeHostPointer(addr);
        const bool readable_host = host_like && IsReadableHostRange(addr, size);
        const bool aliases_output = OverlapsAnyOutputRange(addr, size,
                                                           output_buffer_addrs_,
                                                           output_buffer_sizes_);
        if (size != 0 && readable_host && !aliases_output) {
            // The caller can release or reuse host inputs after enqueueing.
            // Own a snapshot for delayed compilation, without a size cutoff.
            input_host_buffer_copies_[i].resize(size);
            std::memcpy(input_host_buffer_copies_[i].data(), addr, size);
            snapshot_addr = input_host_buffer_copies_[i].data();
            input_owns_host_buffer_[i] = true;
        }

        input_buffer_snapshots_[i] = SnapshotDataBuffer(inputs_->at(i), snapshot_addr, size);
    }
}

AclOpCompileAndExecuteCommand::~AclOpCompileAndExecuteCommand()
{
    for (aclDataBuffer *buffer : input_buffer_snapshots_) {
        if (buffer != nullptr) Driver::DestroyDataBuffer(buffer);
    }
    for (aclDataBuffer *buffer : output_buffer_snapshots_) {
        if (buffer != nullptr) Driver::DestroyDataBuffer(buffer);
    }
}

void AclOpCompileAndExecuteCommand::Predict(
    XMemoryContextType ctx, std::shared_ptr<preempt::XMemoryManager> xmm)
{
    UNUSED(xmm);
    if (ctx <= kMemoryContextAllocation) return;
    if (mem_ctx_ != nullptr) return;

    auto mem_ctx = std::make_shared<preempt::MemorySet>();
    bool has_range = false;
    for (size_t i = 0; i < input_buffer_addrs_.size(); ++i) {
        has_range |= MergeBufferRange(input_buffer_addrs_[i], input_buffer_sizes_[i],
                                      *mem_ctx, false);
    }
    for (size_t i = 0; i < output_buffer_addrs_.size(); ++i) {
        has_range |= MergeBufferRange(output_buffer_addrs_[i], output_buffer_sizes_[i],
                                      *mem_ctx, true);
    }

    if (has_range) mem_ctx_ = std::move(mem_ctx);
}

aclError AclOpCompileAndExecuteCommand::Launch(aclrtStream stream)
{
    int num_inputs = inputDesc_->size();
    int num_outputs = outputDesc_->size();
    std::vector<const aclTensorDesc *> input_desc(num_inputs);
    std::vector<const aclDataBuffer *> inputs(num_inputs);
    std::vector<const aclTensorDesc *> output_desc(num_outputs);
    std::vector<aclDataBuffer *> outputs(num_outputs);
    for (int i = 0; i < num_inputs; ++i) {
        input_desc[i] = inputDesc_->at(i)->desc();
        inputs[i] = input_buffer_snapshots_[i] == nullptr
                    ? inputs_->at(i)->buffer()
                    : input_buffer_snapshots_[i];
    }
    for (int i = 0; i < num_outputs; ++i) {
        output_desc[i] = outputDesc_->at(i)->desc();
        outputs[i] = output_buffer_snapshots_[i] == nullptr
                     ? (aclDataBuffer *)outputs_->at(i)->buffer()
                     : output_buffer_snapshots_[i];
    }
    const char *op_type = opType_is_null_ ? nullptr : opType_.c_str();
    const char *op_path = opPath_is_null_ ? nullptr : opPath_.c_str();
    if (v2_) {
        std::vector<aclTensorDesc *> in_desc(num_inputs), out_desc(num_outputs);
        std::vector<aclDataBuffer *> in_buf(num_inputs);
        for (int i = 0; i < num_inputs; ++i) {
            in_desc[i] = const_cast<aclTensorDesc *>(input_desc[i]);
            in_buf[i] = const_cast<aclDataBuffer *>(inputs[i]);
        }
        for (int i = 0; i < num_outputs; ++i)
            out_desc[i] = const_cast<aclTensorDesc *>(output_desc[i]);
        return OpCompiler::opCompileAndExecuteV2(op_type, num_inputs, in_desc.data(),
            in_buf.data(), num_outputs, out_desc.data(), outputs.data(),
            const_cast<aclopAttr *>(attr_->attr()), engineType_, compileFlag_, op_path, stream);
    }
    return OpCompiler::opCompileAndExecute(op_type,
                                           num_inputs, input_desc.data(), inputs.data(),
                                           num_outputs, output_desc.data(), outputs.data(),
                                           attr_->attr(), engineType_, compileFlag_,
                                           op_path, stream);
}

AclNNCommand::AclNNCommand(const char *api_name, void *workspace,
                           uint64_t workspace_size,
                           std::shared_ptr<AclNNExecutor> executor,
                           AclNNExecuteFunc execute_fn)
    : api_name_(api_name == nullptr ? "" : api_name)
    , workspace_(workspace)
    , workspace_size_(workspace_size)
    , executor_(std::move(executor))
    , execute_fn_(execute_fn)
{
    if (executor_ != nullptr) resources_ = executor_->RetainResources();
    auto executor_ctx = executor_ == nullptr ? nullptr : executor_->GetMemoryContext();
    if (executor_ctx != nullptr) {
        mem_ctx_ = std::make_shared<preempt::MemorySet>(*executor_ctx);
    }
    if (workspace_ == nullptr || workspace_size_ == 0) return;

    if (mem_ctx_ == nullptr) mem_ctx_ = std::make_shared<preempt::MemorySet>();
    const uint64_t begin = reinterpret_cast<uint64_t>(workspace_);
    uint64_t end = begin + workspace_size_;
    if (end < begin) end = std::numeric_limits<uint64_t>::max();
    mem_ctx_->writes.Merge(begin, end);
}

AclNNCommand::~AclNNCommand()
{
    // A dropped one-shot command never transfers its executor to CANN.
    // Destroy it before releasing the parameters used by its internal views.
    if (executor_ != nullptr) executor_->DiscardUnlaunched();
}

aclError AclNNCommand::Launch(aclrtStream stream)
{
    if (execute_fn_ == nullptr) return ACL_ERROR_INVALID_PARAM;

    if (executor_ == nullptr) return ACL_ERROR_INVALID_PARAM;
    aclnnStatus ret = executor_->Execute(execute_fn_, workspace_, workspace_size_, stream);
    resources_.clear();
    return static_cast<aclError>(ret);
}

void AclNNResource::Update(const void *resource, std::shared_ptr<preempt::MemorySet> memory)
{
    std::lock_guard<std::mutex> lock(resource_mutex_);
    auto &items = GetAclNNResourceMap();
    auto it = items.find(resource);
    if (it != items.end()) it->second->mem_ctx_ = std::move(memory);
}
void AclNNExecutor::RecordResources(aclOpExecutor *executor, const void *const *resources, size_t count)
{
    auto item = Create(executor);
    if (!item) return;
    std::lock_guard<std::mutex> lock(executor_mutex_);
    item->bindings_.clear();
    item->bound_mem_ctx_.reset();
    for (size_t i = 0; i < count; ++i) {
        auto memory = AclNNResource::GetMemoryContext(resources + i, 1);
        if (memory) item->bindings_[resources[i]] = std::move(memory);
    }
}
void AclNNExecutor::RecordCachedMemory(aclOpExecutor *executor, std::shared_ptr<preempt::MemorySet> memory)
{
    auto item = Create(executor);
    if (!item) return;
    std::lock_guard<std::mutex> lock(executor_mutex_);
    item->bindings_.clear();
    item->bound_mem_ctx_.reset();
    item->mem_ctx_ = std::move(memory);
}
std::shared_ptr<xsched::preempt::MemorySet> AclNNExecutor::GetMemoryContext() const
{
    std::lock_guard<std::mutex> lock(executor_mutex_);
    if (bindings_.empty()) return mem_ctx_;
    if (bound_mem_ctx_) return bound_mem_ctx_;
    auto result = std::make_shared<preempt::MemorySet>();
    for (const auto &[key, memory] : bindings_) {
        UNUSED(key);
        result->reads.Merge(memory->reads); result->writes.Merge(memory->writes);
    }
    bound_mem_ctx_ = result;
    return result;
}
void AclNNExecutor::Rebind(aclOpExecutor *executor, const void *tensor, void *address)
{
    if (!GetAscendXvmmEnabled()) return;
    auto item = Create(executor);
    XASSERT(item, "missing cached executor");
    std::lock_guard<std::mutex> lock(executor_mutex_);
    auto it = item->bindings_.find(tensor);
    XASSERT(it != item->bindings_.end(), "XVMM cached executor binding metadata missing: tensor=%p", tensor);
    auto replacement = std::make_shared<preempt::MemorySet>();
    uint64_t base = UINT64_MAX;
    it->second->reads.ForEach([&](uint64_t b, uint64_t) { base = std::min(base, b); });
    it->second->writes.ForEach([&](uint64_t b, uint64_t) { base = std::min(base, b); });
    auto translate = [&](const xsched::utils::Region &from, xsched::utils::Region &to) {
        from.ForEach([&](uint64_t b, uint64_t e) {
            auto begin = reinterpret_cast<uintptr_t>(address) + b - base;
            XASSERT(begin <= UINTPTR_MAX - (e - b), "XVMM cached range overflow");
            to.Merge(begin, begin + e - b);
        });
    };
    translate(it->second->reads, replacement->reads);
    translate(it->second->writes, replacement->writes);
    it->second = std::move(replacement);
    item->bound_mem_ctx_.reset();
}
