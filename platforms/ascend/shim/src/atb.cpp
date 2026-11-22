#include "xsched/ascend/shim/atb.h"
#include "xsched/ascend/shim/shim.h"

#ifdef XSCHED_HAS_ATB
#include "intercept_symbol.h"

#include <atb/context.h>
#include <atb/infer_op_params.h>
#include <atb/operation.h>

#include <cstring>
#include <unordered_set>

using namespace xsched::ascend;
using namespace xsched::preempt;

namespace
{

void *AtbSymbol(const char *name)
{
    static void *library = dlopen("libatb.so", RTLD_NOW | RTLD_LOCAL);
    XASSERT(library != nullptr, "cannot load ATB: %s", dlerror());
    void *symbol = RealDlSym(library, name);
    XASSERT(symbol != nullptr, "ATB symbol unavailable: %s", name);
    return symbol;
}

std::mutex operations_mutex;
std::unordered_set<const atb::Operation *> managed_operations;

std::shared_ptr<MemorySet> Access(const atb::VariantPack &pack,
                                  void *workspace, uint64_t bytes)
{
    auto memory = std::make_shared<MemorySet>();
    auto add = [](xsched::utils::Region &region, void *data, uint64_t size) {
        if (data == nullptr || size == 0) return;
        const auto begin = reinterpret_cast<uintptr_t>(data);
        XASSERT(size <= UINTPTR_MAX - begin, "ATB tensor range overflow");
        region.Merge(begin, begin + size);
    };
    for (const auto &tensor : pack.inTensors)
        add(memory->reads, tensor.deviceData, tensor.dataSize);
    for (const auto &tensor : pack.outTensors)
        add(memory->writes, tensor.deviceData, tensor.dataSize);
    // Scratch can be read after writes within the same operation.
    add(memory->reads, workspace, bytes);
    add(memory->writes, workspace, bytes);
    return memory;
}

class AtbCommand final : public AclCommand
{
public:
    AtbCommand(atb::Operation *operation, const atb::VariantPack &pack,
               uint8_t *workspace, uint64_t bytes, atb::Context *context,
               bool setup)
        : operation_(operation), pack_(pack), workspace_(workspace), bytes_(bytes)
        , context_(context), setup_(setup)
        , mem_ctx_(Access(pack, workspace, bytes))
    {
        // ATB reuses mutable runners, hostData and tiling metadata after its
        // API returns. Preserve that contract by waiting for host dispatch,
        // while device completion still follows the queue's normal events.
        SetProps(kCommandPropertyBlockingSubmit | kCommandPropertyDispatchContinuation);
    }

    atb::Status Status() const { return status_; }
    uint64_t WorkspaceSize() const { return bytes_; }
    std::shared_ptr<MemorySet> GetMemoryContext() override { return mem_ctx_; }

private:
    aclError Launch(aclrtStream stream) override
    {
        UNUSED(stream); // The native operation chooses its context stream(s).
        NativeCallScope native;
        if (setup_) status_ = operation_->Setup(pack_, bytes_, context_);
        else status_ = operation_->Execute(pack_, workspace_, bytes_, context_);
        // Setup/Execute report synchronous validation errors to their caller.
        // Do not turn an ATB parameter error into a fatal ACL worker error.
        return ACL_SUCCESS;
    }

    atb::Operation *operation_;
    atb::VariantPack pack_;
    uint8_t *workspace_;
    uint64_t bytes_;
    atb::Context *context_;
    bool setup_;
    atb::Status status_ = atb::NO_ERROR;
    std::shared_ptr<MemorySet> mem_ctx_;
};

class ManagedOperation final : public atb::Operation
{
public:
    explicit ManagedOperation(atb::Operation *operation): native_(operation) {}
    atb::Operation *Native() const { return native_; }
    std::string GetName() const override { return native_->GetName(); }
    uint32_t GetInputNum() const override { return native_->GetInputNum(); }
    uint32_t GetOutputNum() const override { return native_->GetOutputNum(); }
    atb::Status InferShape(const atb::SVector<atb::TensorDesc> &in,
                           atb::SVector<atb::TensorDesc> &out) const override
    { return native_->InferShape(in, out); }

    atb::Status Setup(const atb::VariantPack &pack, uint64_t &bytes,
                      atb::Context *context) override
    {
        if (InNativeCall() || context == nullptr) {
            NativeCallScope native;
            return native_->Setup(pack, bytes, context);
        }
        PythonGilRelease gil;
        const auto stream = Stream(context);
        auto queue = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
        if (!queue && !GetAscendXvmmEnabled()) {
            NativeCallScope native;
            return native_->Setup(pack, bytes, context);
        }
        auto command = std::make_shared<AtbCommand>(native_, pack, nullptr, 0, context, true);
        if (queue) queue->Submit(command);
        else ACL_ASSERT(AclQueue::DirectLaunch(command, stream));
        bytes = command->WorkspaceSize();
        return command->Status();
    }

    atb::Status Execute(const atb::VariantPack &pack, uint8_t *workspace,
                        uint64_t bytes, atb::Context *context) override
    {
        if (InNativeCall() || context == nullptr) {
            NativeCallScope native;
            return native_->Execute(pack, workspace, bytes, context);
        }
        PythonGilRelease gil;
        const auto stream = Stream(context);
        auto queue = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
        if (!queue && !GetAscendXvmmEnabled()) {
            NativeCallScope native;
            return native_->Execute(pack, workspace, bytes, context);
        }
        auto command = std::make_shared<AtbCommand>(native_, pack, workspace, bytes, context, false);
        if (queue) queue->Submit(command);
        else ACL_ASSERT(AclQueue::DirectLaunch(command, stream));
        return command->Status();
    }

private:
    aclrtStream Stream(atb::Context *context) const
    {
        using GetId = uint32_t (*)(atb::Operation *);
        static auto get_id = reinterpret_cast<GetId>(
            AtbSymbol("_ZN3atb18GetExecuteStreamIdEPNS_9OperationE"));
        auto streams = context->GetExecuteStreams();
        if (streams.empty()) return context->GetExecuteStream();
        const auto id = get_id(native_);
        return id < streams.size() ? streams[id] : nullptr;
    }

    atb::Operation *native_;
};

atb::Operation *Unwrap(const atb::Operation *operation)
{
    std::lock_guard<std::mutex> lock(operations_mutex);
    if (managed_operations.contains(operation))
        return static_cast<const ManagedOperation *>(operation)->Native();
    return const_cast<atb::Operation *>(operation);
}

atb::Status Wrap(atb::Status result, atb::Operation **out)
{
    if (result == atb::NO_ERROR && out != nullptr && *out != nullptr && !InNativeCall()) {
        auto wrapper = std::make_unique<ManagedOperation>(*out);
        std::lock_guard<std::mutex> lock(operations_mutex);
        managed_operations.insert(wrapper.get());
        *out = wrapper.release();
    }
    return result;
}

} // namespace

// Public operation families imported by the torch-npu 310P ATB plugin.
// Resolve the C++ template ABI dynamically, without linking ATB into the shim.
#define ATB_OPERATIONS(M) \
    M(LinearParam, 11) \
    M(ElewiseParam, 12) \
    M(RmsNormParam, 12) \
    M(GroupTopkParam, 14) \
    M(SelfAttentionParam, 18) \
    M(PagedAttentionParam, 19) \
    M(ReshapeAndCacheParam, 20) \
    M(RopeParam, 9)

#define ATB_CREATE_SYMBOL(TYPE, LENGTH) \
    "_ZN3atb15CreateOperationINS_5infer" #LENGTH #TYPE "EEEiRKT_PPNS_9OperationE"
namespace atb
{

#define ATB_API(TYPE, LENGTH) \
template <> __attribute__((visibility("default"))) \
Status CreateOperation<infer::TYPE>(const infer::TYPE &param, Operation **out) \
{ \
    using Func = Status (*)(const infer::TYPE &, Operation **); \
    static auto function = reinterpret_cast<Func>(AtbSymbol(ATB_CREATE_SYMBOL(TYPE, LENGTH))); \
    Status result; \
    { NativeCallScope native; result = function(param, out); } \
    return Wrap(result, out); \
}
ATB_OPERATIONS(ATB_API)
#undef ATB_API

__attribute__((visibility("default"))) Status DestroyOperation(Operation *operation)
{
    PythonGilRelease gil;
    using Func = Status (*)(Operation *);
    static auto function = reinterpret_cast<Func>(AtbSymbol("_ZN3atb16DestroyOperationEPNS_9OperationE"));
    auto *native_operation = Unwrap(operation);
    NativeCallScope native;
    auto result = function(native_operation);
    if (operation != native_operation && result == NO_ERROR) {
        { std::lock_guard<std::mutex> lock(operations_mutex); managed_operations.erase(operation); }
        delete operation;
    }
    return result;
}

__attribute__((visibility("default"))) Status SetExecuteStreamId(Operation *operation, uint32_t id)
{
    using Func = Status (*)(Operation *, uint32_t);
    static auto function = reinterpret_cast<Func>(AtbSymbol("_ZN3atb18SetExecuteStreamIdEPNS_9OperationEj"));
    NativeCallScope native;
    return function(Unwrap(operation), id);
}

__attribute__((visibility("default"))) uint32_t GetExecuteStreamId(Operation *operation)
{
    using Func = uint32_t (*)(Operation *);
    static auto function = reinterpret_cast<Func>(AtbSymbol("_ZN3atb18GetExecuteStreamIdEPNS_9OperationE"));
    NativeCallScope native;
    return function(Unwrap(operation));
}

} // namespace atb

void *xsched::ascend::FindAtbInterceptedSymbol(const char *name)
{
    static const InterceptedSymbol symbols[] = {
#define ATB_ENTRY(TYPE, LENGTH) \
        {ATB_CREATE_SYMBOL(TYPE, LENGTH), reinterpret_cast<void *>(&atb::CreateOperation<atb::infer::TYPE>)},
        ATB_OPERATIONS(ATB_ENTRY)
#undef ATB_ENTRY
        {"_ZN3atb16DestroyOperationEPNS_9OperationE", reinterpret_cast<void *>(&atb::DestroyOperation)},
        {"_ZN3atb18SetExecuteStreamIdEPNS_9OperationEj", reinterpret_cast<void *>(&atb::SetExecuteStreamId)},
        {"_ZN3atb18GetExecuteStreamIdEPNS_9OperationE", reinterpret_cast<void *>(&atb::GetExecuteStreamId)},
    };
    const auto *symbol = FindInterceptedSymbol(symbols, name);
    return symbol == nullptr ? nullptr : symbol->address;
}

#else

void *xsched::ascend::FindAtbInterceptedSymbol(const char *) { return nullptr; }

#endif
