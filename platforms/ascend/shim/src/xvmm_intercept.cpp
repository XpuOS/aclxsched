#include "xsched/ascend/shim/shim.h"
#include "xsched/ascend/shim/python.h"
#include "xsched/ascend/hal/acl_mem.h"
#include "xsched/utils/symbol.h"
using namespace xsched::ascend;
namespace {
thread_local std::vector<void *> cached_addresses;
template <typename SetAddress>
aclnnStatus RebindExecutor(SetAddress set_address, aclOpExecutor *executor,
                          size_t index, aclTensor *tensor, void *address)
{
    PythonGilRelease gil;
    // Cached executors are mutable: finish queued uses before changing either
    // the native object or its memory access metadata.
    if (GetAscendXvmmEnabled()) xsched::preempt::XQueueManager::ForEachWaitAll();
    auto ret = set_address(executor, index, tensor, address);
    if (ret == ACL_SUCCESS) AclNNExecutor::Rebind(executor, tensor, address);
    return ret;
}
void RecordCachedAccess(aclOpExecutor *executor) {
    if (!executor || !GetAscendXvmmEnabled()) return;
    auto memory = std::make_shared<xsched::preempt::MemorySet>();
    auto manager = AclMemoryManager::GetCurrentXMemoryManager();
    for (void *address : cached_addresses) {
        // PTA supplies storage addresses, including interior allocations, but
        // no byte counts. Bound conservative storage coverage by the owning
        // managed allocation; never extend it into a neighboring allocation.
        auto bytes = manager->GetAllocatedSize(address);
        auto begin = reinterpret_cast<uintptr_t>(address);
        if (bytes) memory->reads.Merge(begin, begin + bytes);
    }
    AclNNExecutor::RecordCachedMemory(executor, std::move(memory));
}
}
EXPORT_C_FUNC void InitPTACacheThreadLocal()
{
    cached_addresses.clear();
    auto native = reinterpret_cast<void (*)()>(ResolveOriginalAclnn("InitPTACacheThreadLocal"));
    native();
}
EXPORT_C_FUNC void AddTensorAddrToCachedList(void *address)
{
    if (GetAscendXvmmEnabled()) cached_addresses.push_back(address);
    auto native = reinterpret_cast<void (*)(void *)>(ResolveOriginalAclnn("AddTensorAddrToCachedList"));
    native(address);
}
EXPORT_C_FUNC aclOpExecutor *PTAGetExecCache(uint64_t hash, uint64_t *workspace)
{
    auto native = reinterpret_cast<aclOpExecutor *(*)(uint64_t, uint64_t *)>(ResolveOriginalAclnn("PTAGetExecCache"));
    auto executor = native(hash, workspace);
    RecordCachedAccess(executor);
    return executor;
}
EXPORT_C_FUNC aclOpExecutor *PTAFindExecCache(uint8_t *key, size_t length, uint64_t *workspace)
{
    auto native = reinterpret_cast<aclOpExecutor *(*)(uint8_t *, size_t, uint64_t *)>(ResolveOriginalAclnn("PTAFindExecCache"));
    auto executor = native(key, length, workspace);
    RecordCachedAccess(executor);
    return executor;
}
EXPORT_C_FUNC aclnnStatus aclSetInputTensorAddr(aclOpExecutor *executor, size_t index, aclTensor *tensor, void *address)
{
    return RebindExecutor(NnBase::SetInputTensorAddr, executor, index, tensor, address);
}
EXPORT_C_FUNC aclnnStatus AclSetInputTensorAddr(aclOpExecutor *executor, size_t index, aclTensor *tensor, void *address)
{
    return RebindExecutor(NnBase::SetInputTensorAddr, executor, index, tensor, address);
}
EXPORT_C_FUNC aclnnStatus aclSetOutputTensorAddr(aclOpExecutor *executor, size_t index, aclTensor *tensor, void *address)
{
    return RebindExecutor(NnBase::SetOutputTensorAddr, executor, index, tensor, address);
}
EXPORT_C_FUNC aclnnStatus AclSetOutputTensorAddr(aclOpExecutor *executor, size_t index, aclTensor *tensor, void *address)
{
    return RebindExecutor(NnBase::SetOutputTensorAddr, executor, index, tensor, address);
}
EXPORT_C_FUNC aclnnStatus aclSetTensorAddr(aclOpExecutor *executor, size_t index, aclTensor *tensor, void *address)
{
    return RebindExecutor(NnBase::SetTensorAddr, executor, index, tensor, address);
}
EXPORT_C_FUNC aclnnStatus AclSetTensorAddr(aclOpExecutor *executor, size_t index, aclTensor *tensor, void *address)
{
    return RebindExecutor(NnBase::SetTensorAddr, executor, index, tensor, address);
}
EXPORT_C_FUNC aclnnStatus aclInitTensor(aclTensor *tensor, const int64_t *dims, uint64_t ndims,
    aclDataType type, const int64_t *stride, int64_t offset, aclFormat format,
    const int64_t *storage, uint64_t nstorage, void *address)
{
    auto ret = NnBase::InitTensor(tensor, dims, ndims, type, stride, offset, format, storage, nstorage, address);
    if (ret == ACL_SUCCESS && GetAscendXvmmEnabled()) {
        auto memory = std::make_shared<xsched::preempt::MemorySet>();
        // Protect the complete managed storage from this interior address.
        auto size = AclMemoryManager::GetCurrentXMemoryManager()->GetAllocatedSize(address);
        auto begin = reinterpret_cast<uintptr_t>(address);
        if (size) memory->reads.Merge(begin, begin + size);
        AclNNResource::Update(tensor, std::move(memory));
    }
    return ret;
}
