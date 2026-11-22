#pragma once

#include <mutex>
#include <map>
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <cstdint>

#include "xsched/ascend/hal/driver.h"
#include "xsched/ascend/hal/acl_assert.h"
#include "xsched/preempt/hal/hw_command.h"

namespace xsched::ascend
{

#define ACL_COMMAND(name, base, ret_t, func, ...) \
    class name : public base \
    { \
    public: \
        name(FOR_EACH_PAIR_COMMA(DECLARE_PARAM, __VA_ARGS__)) \
            : base(), FOR_EACH_PAIR_COMMA(DECLARE_COPY_PRIVATE_ARG, __VA_ARGS__) {} \
        virtual ~name() = default; \
    private: \
        FOR_EACH_PAIR_SEMICOLON(DECLARE_PRIVATE_PARAM, __VA_ARGS__) \
        virtual ret_t Launch(aclrtStream stream) override \
        { return func(FOR_EACH_PAIR_COMMA(DECLARE_PRIVATE_ARG, __VA_ARGS__) __VA_OPT__(,) stream); } \
    };

class AclCommand : public preempt::HwCommand
{
public:
    AclCommand(): HwCommand(preempt::kCommandPropertyNone) {}
    virtual ~AclCommand();
    virtual void Synchronize() override;
    virtual bool Synchronizable() override;
    virtual bool EnableSynchronization() override;
    virtual void OnCompleted() override;
    void HoldMemory(std::shared_ptr<void> references);
    aclError LaunchWrapper(aclrtStream stream);

protected:
    virtual aclError Launch(aclrtStream stream) = 0;
    virtual void OnLaunchComplete(aclError ret) { UNUSED(ret); }

private:
    aclrtEvent following_event_ = nullptr;
    std::mutex completion_mutex_;
    std::shared_ptr<void> memory_references_;
};

class AclEventRecordCommand : public AclCommand
{
public:
    AclEventRecordCommand(aclrtEvent event);
    virtual ~AclEventRecordCommand() = default;
    virtual void Synchronize() override { ACL_ASSERT(Driver::rtSynchronizeEvent(event_)); }
    virtual bool Synchronizable() override { return true; }
    virtual bool EnableSynchronization() override { return true; }

private:
    aclrtEvent event_;
    virtual aclError Launch(aclrtStream stream) override
    { return Driver::rtRecordEvent(event_, stream); }
};

class TensorDesc
{
public:
    TensorDesc() = default;
    virtual ~TensorDesc() { if (desc_) Driver::DestroyTensorDesc(desc_); }
    const aclTensorDesc *desc() const { return desc_; }

    static std::shared_ptr<TensorDesc> Create(const aclTensorDesc *desc);
    static bool Destroy(const aclTensorDesc *desc);

private:
    const aclTensorDesc *desc_ = nullptr;
    static std::mutex tensor_desc_mutex_;
    static std::unordered_map<const aclTensorDesc *, std::shared_ptr<TensorDesc>> tensor_descs_;
};

class DataBuffer
{
public:
    DataBuffer() = default;
    virtual ~DataBuffer() { if (buffer_) Driver::DestroyDataBuffer(buffer_); }
    const aclDataBuffer *buffer() const { return buffer_; }

    static std::shared_ptr<DataBuffer> Create(const aclDataBuffer *buffer);
    static bool Destroy(const aclDataBuffer *buffer);

private:
    const aclDataBuffer *buffer_ = nullptr;
    static std::mutex data_buffer_mutex_;
    static std::unordered_map<const aclDataBuffer *, std::shared_ptr<DataBuffer>> data_buffers_;
};

class OpAttr
{
public:
    OpAttr() = default;
    virtual ~OpAttr() { if (attr_) Driver::opDestroyAttr(attr_); }
    const aclopAttr *attr() const { return attr_; }

    static std::shared_ptr<OpAttr> Create(const aclopAttr *attr);
    static bool Destroy(const aclopAttr *attr);

private:
    const aclopAttr *attr_ = nullptr;
    static std::mutex op_attr_mutex_;
    static std::unordered_map<const aclopAttr *, std::shared_ptr<OpAttr>> op_attrs_;
};

class AclNNResource;
using AclNNResources = std::vector<std::shared_ptr<AclNNResource>>;
using AclNNExecuteFunc = aclnnStatus (*)(void *, uint64_t, aclOpExecutor *, aclrtStream);

class AclNNExecutor
{
public:
    AclNNExecutor() = default;
    virtual ~AclNNExecutor();
    static std::shared_ptr<AclNNExecutor> Create(aclOpExecutor *executor);
    static bool Destroy(aclOpExecutor *executor);
    static void RecordWorkspace(aclOpExecutor *executor, AclNNResources resources,
                                std::shared_ptr<preempt::MemorySet> mem_ctx);
    static aclnnStatus SetRepeatable(aclOpExecutor *executor);
    AclNNResources RetainResources();
    void DiscardUnlaunched();
    aclnnStatus Execute(AclNNExecuteFunc execute_fn, void *workspace,
                        uint64_t workspace_size, aclrtStream stream);
    static void RecordResources(aclOpExecutor *, const void *const *, size_t);
    static void RecordCachedMemory(aclOpExecutor *, std::shared_ptr<preempt::MemorySet>);
    static void Rebind(aclOpExecutor *, const void *tensor, void *address);
    std::shared_ptr<preempt::MemorySet> GetMemoryContext() const;

private:
    aclOpExecutor *executor_ = nullptr;
    bool repeatable_ = false;
    std::map<const void *, std::shared_ptr<preempt::MemorySet>> bindings_;
    AclNNResources resources_;
    std::shared_ptr<preempt::MemorySet> mem_ctx_ = nullptr;
    mutable std::shared_ptr<preempt::MemorySet> bound_mem_ctx_ = nullptr;
    static std::mutex executor_mutex_;
    static std::unordered_map<aclOpExecutor *, std::shared_ptr<AclNNExecutor>> executors_;
};

class AclNNResource
{
public:
    using DestroyFunc = aclnnStatus (*)(const void *);

    AclNNResource() = default;
    virtual ~AclNNResource();

    static void Create(const void *resource, DestroyFunc destroy_fn,
                       std::shared_ptr<preempt::MemorySet> mem_ctx = nullptr,
                       AclNNResources dependencies = {});
    static bool Destroy(const void *resource);
    static void Update(const void *resource, std::shared_ptr<preempt::MemorySet> memory);
    static AclNNResources Retain(const void *const *resources, size_t count);
    static std::shared_ptr<preempt::MemorySet> GetMemoryContext(
        const void *const *resources, size_t count);
    static std::shared_ptr<preempt::MemorySet> GetMemoryContext(const AclNNResources &resources);

private:
    const void *resource_ = nullptr;
    DestroyFunc destroy_fn_ = nullptr;
    std::shared_ptr<preempt::MemorySet> mem_ctx_ = nullptr;
    AclNNResources dependencies_;
    static std::mutex resource_mutex_;
    static std::unordered_map<const void *, std::shared_ptr<AclNNResource>> resources_;
};

class AclOpCompileAndExecuteCommand : public AclCommand
{
public:
    AclOpCompileAndExecuteCommand(const char * opType,
                                  std::shared_ptr<std::vector<std::shared_ptr<TensorDesc>>> inputDesc,
                                  std::shared_ptr<std::vector<std::shared_ptr<DataBuffer>>> inputs,
                                  std::shared_ptr<std::vector<std::shared_ptr<TensorDesc>>> outputDesc,
                                  std::shared_ptr<std::vector<std::shared_ptr<DataBuffer>>> outputs,
                                  std::shared_ptr<OpAttr> attr,
                                  aclopEngineType engineType, aclopCompileType compileFlag,
                                  const char * opPath, bool v2 = false);
    virtual ~AclOpCompileAndExecuteCommand() override;
    virtual std::shared_ptr<preempt::MemorySet> GetMemoryContext() override { return mem_ctx_; }
    virtual void Predict(XMemoryContextType ctx,
                                      std::shared_ptr<preempt::XMemoryManager> xmm) override;
private:
    std::string opType_;
    bool opType_is_null_;
    std::shared_ptr<std::vector<std::shared_ptr<TensorDesc>>> inputDesc_;
    std::shared_ptr<std::vector<std::shared_ptr<DataBuffer>>> inputs_;
    std::shared_ptr<std::vector<std::shared_ptr<TensorDesc>>> outputDesc_;
    std::shared_ptr<std::vector<std::shared_ptr<DataBuffer>>> outputs_;
    std::shared_ptr<OpAttr> attr_;
    aclopEngineType engineType_;
    aclopCompileType compileFlag_;
    std::string opPath_;
    bool opPath_is_null_;
    bool v2_;
    std::vector<aclDataBuffer *> input_buffer_snapshots_;
    std::vector<aclDataBuffer *> output_buffer_snapshots_;
    std::vector<void *> input_buffer_addrs_;
    std::vector<void *> output_buffer_addrs_;
    std::vector<size_t> input_buffer_sizes_;
    std::vector<size_t> output_buffer_sizes_;
    std::vector<std::vector<uint8_t>> input_host_buffer_copies_;
    std::vector<bool> input_owns_host_buffer_;
    std::shared_ptr<preempt::MemorySet> mem_ctx_ = nullptr;
    virtual aclError Launch(aclrtStream stream) override;
};

class AclNNCommand : public AclCommand
{
public:
    AclNNCommand(const char *api_name, void *workspace, uint64_t workspace_size,
                 std::shared_ptr<AclNNExecutor> executor, AclNNExecuteFunc execute_fn);
    virtual ~AclNNCommand() override;
    virtual std::shared_ptr<preempt::MemorySet> GetMemoryContext() override { return mem_ctx_; }

private:
    std::string api_name_;
    void *workspace_ = nullptr;
    uint64_t workspace_size_ = 0;
    // Keep parameters alive while an unlaunched executor is being destroyed.
    AclNNResources resources_;
    std::shared_ptr<AclNNExecutor> executor_ = nullptr;
    AclNNExecuteFunc execute_fn_ = nullptr;
    std::shared_ptr<preempt::MemorySet> mem_ctx_ = nullptr;

    virtual aclError Launch(aclrtStream stream) override;
};

	ACL_COMMAND(AclModelExecuteCommand, AclCommand, aclError, Driver::mdlExecuteAsync, uint32_t, modelId, const aclmdlDataset *, input, aclmdlDataset *, output);

    class AclMemsetCommand : public AclCommand
    {
    public:
        std::shared_ptr<preempt::MemorySet> GetMemoryContext() override {
            auto memory = std::make_shared<preempt::MemorySet>();
            const auto b = reinterpret_cast<uintptr_t>(devPtr_);
            if (devPtr_ && count_) memory->writes.Merge(b, b + count_);
            return memory;
        }
        AclMemsetCommand(void *devPtr, size_t maxCount, int32_t value, size_t count)
            : devPtr_(devPtr), maxCount_(maxCount), value_(value), count_(count) {}
        virtual ~AclMemsetCommand() = default;

    private:
        void *devPtr_;
        size_t maxCount_;
        int32_t value_;
        size_t count_;
        virtual aclError Launch(aclrtStream stream) override
        {
            return Driver::rtMemsetAsync(devPtr_, maxCount_, value_, count_, stream);
        }
    };

class AclMemcpyCommand : public AclCommand
{
public:
    std::shared_ptr<preempt::MemorySet> GetMemoryContext() override {
        auto memory = std::make_shared<preempt::MemorySet>();
        auto b = reinterpret_cast<uintptr_t>(src_);
        if (src_ && count_) memory->reads.Merge(b, b + count_);
        b = reinterpret_cast<uintptr_t>(dst_);
        if (dst_ && count_) memory->writes.Merge(b, b + count_);
        return memory;
    }
    AclMemcpyCommand(void *dst, size_t destMax, const void *src, size_t count, aclrtMemcpyKind kind);
    virtual ~AclMemcpyCommand() = default;

private:
    void *dst_;
    size_t destMax_;
    const void *src_;
    size_t count_;
    aclrtMemcpyKind kind_;
    std::vector<char> staged_src_;
    virtual aclError Launch(aclrtStream stream) override
    {
        return Driver::rtMemcpyAsync(dst_, destMax_, src_, count_, kind_, stream);
    }
};

} // namespace xsched::ascend
