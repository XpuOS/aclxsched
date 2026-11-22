#include "xsched/xqueue.h"
#include "xsched/utils/map.h"
#include "xsched/protocol/def.h"
#include "xsched/preempt/hal/hw_queue.h"
#include "xsched/ascend/hal.h"
#include "xsched/ascend/hal/options.h"
#include "xsched/ascend/hal/acl_mem.h"
#include "xsched/ascend/hal/xvmm.h"
#include "xsched/ascend/shim/shim.h"
#include "xsched/ascend/hal/acl_queue.h"
#include "xsched/ascend/hal/acl_command.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <vector>
#include <unordered_map>
#include <algorithm>

using namespace xsched::preempt;

namespace xsched::ascend
{

static utils::ObjectMap<aclrtEvent, std::shared_ptr<AclEventRecordCommand>> g_events;

namespace
{

std::mutex event_wait_mutex;
std::unordered_map<aclrtEvent, std::vector<std::weak_ptr<AclCommand>>> event_waits;

void TrackEventWait(aclrtEvent event, const std::shared_ptr<AclCommand> &command)
{
    std::lock_guard<std::mutex> lock(event_wait_mutex);
    auto &waits = event_waits[event];
    std::erase_if(waits, [](const auto &wait) { return wait.expired(); });
    waits.push_back(command);
}

bool EventRecordPending(aclrtEvent event)
{
    auto record = g_events.Get(event, nullptr);
    // Batch completion includes matching Unref, so a successful query may be
    // followed immediately by Free without racing reference retirement.
    const auto ready = GetAscendXvmmEnabled() ? kCommandStateCompleted : kCommandStateInFlight;
    return record && record->GetState() < ready;
}

// CANN embeds Python when the first ACLop is compiled. Initialize it on the
// caller, whose lifetime includes aclFinalize, rather than a launch worker that
// can disappear when its stream is destroyed (Python then waits for that
// vanished initializing thread during interpreter shutdown).
aclError InitializeCompiler(const char *type, int ninputs,
    aclopCompile_arg2_t inputs, int noutputs, aclopCompile_arg4_t outputs,
    const aclopAttr *attr, aclopEngineType engine, aclopCompileType flag,
    const char *path)
{
    NativeCallScope native;
    static std::once_flag once;
    aclError result = ACL_SUCCESS;
    std::call_once(once, [&] {
        result = OpCompiler::opCompile(type, ninputs, inputs, noutputs,
                                      outputs, attr, engine, flag, path);
        if (result != ACL_SUCCESS) throw result;
    });
    return result;
}

std::shared_ptr<MemorySet> MakeMemoryContext(void *addr, size_t size)
{
    if (addr == nullptr || size == 0) return nullptr;
    const uint64_t begin = reinterpret_cast<uint64_t>(addr);
    uint64_t end = begin + size;
    if (end < begin) end = std::numeric_limits<uint64_t>::max();
    auto mem_ctx = std::make_shared<MemorySet>();
    mem_ctx->reads.Merge(begin, end);
    return mem_ctx;
}

size_t TensorStorageBytes(aclDataType data_type,
                          const int64_t *view_dims, uint64_t view_dims_num,
                          const int64_t *storage_dims, uint64_t storage_dims_num)
{
    const int64_t *dims = storage_dims_num == 0 ? view_dims : storage_dims;
    const uint64_t dims_num = storage_dims_num == 0 ? view_dims_num : storage_dims_num;
    if (dims_num != 0 && dims == nullptr) return 0;

    size_t elements = 1;
    for (uint64_t i = 0; i < dims_num; ++i) {
        if (dims[i] <= 0) return 0;
        const size_t dim = static_cast<size_t>(dims[i]);
        if (elements > std::numeric_limits<size_t>::max() / dim) return 0;
        elements *= dim;
    }

    const size_t element_size = Driver::DataTypeSize(data_type);
    if (element_size == 0 || elements > std::numeric_limits<size_t>::max() / element_size) {
        return 0;
    }
    return elements * element_size;
}

bool HasAutoXQueueForStream(aclrtStream stream)
{
    if (stream == nullptr) return false;
    return HwQueueManager::GetXQueue(GetHwQueueHandle(stream)) != nullptr;
}

XResult AutoCreateXQueueForStream(aclrtStream stream)
{
    if (stream == nullptr) return kXSchedErrorInvalidValue;
    if (HasAutoXQueueForStream(stream)) return kXSchedSuccess;
    return XQueueManager::AutoCreate([&](HwQueueHandle *hwq) {
        return AclQueueCreate(hwq, stream);
    });
}

XResult AutoDestroyXQueueForStream(aclrtStream stream)
{
    if (stream == nullptr) return kXSchedErrorInvalidValue;
    if (!HasAutoXQueueForStream(stream)) return kXSchedSuccess;
    return XQueueManager::AutoDestroy(GetHwQueueHandle(stream));
}

} // namespace

aclError XrtMallocCommon(void **devPtr, size_t size, aclrtMemMallocPolicy policy)
{
    if (InNativeCall()) return Driver::rtMalloc(devPtr, size, policy);
    if (!GetAscendXvmmEnabled()) return Driver::rtMalloc(devPtr, size, policy);
    if (devPtr == nullptr) return ACL_ERROR_INVALID_PARAM;

    auto acl_req = std::make_shared<AclMemoryRequest>(size, policy);
    void *ptr = AclMemoryManager::GetCurrentXMemoryManager()->Allocate(acl_req);
    if (ptr == nullptr) return ACL_ERROR_RT_MEMORY_ALLOCATION;
    *devPtr = ptr;
    return ACL_SUCCESS;
}

aclError XopCompileAndExecute(const char * opType, int numInputs, aclopCompileAndExecute_arg2_t inputDesc, aclopCompileAndExecute_arg3_t inputs, int numOutputs, aclopCompileAndExecute_arg5_t outputDesc, aclopCompileAndExecute_arg6_t outputs, const aclopAttr * attr, aclopEngineType engineType, aclopCompileType compileFlag, const char * opPath , aclrtStream stream)
{
    if (InNativeCall()) return OpCompiler::opCompileAndExecute(opType, numInputs, inputDesc, inputs, numOutputs, outputDesc, outputs, attr, engineType, compileFlag, opPath, stream);
    PythonGilRelease gil;
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr && !GetAscendXvmmEnabled()) return OpCompiler::opCompileAndExecute(opType, numInputs, inputDesc, inputs, numOutputs, outputDesc, outputs, attr, engineType, compileFlag, opPath, stream);

    try {
        InitializeCompiler(opType, numInputs, inputDesc, numOutputs, outputDesc,
                           attr, engineType, compileFlag, opPath);
    } catch (aclError error) { return error; }
    auto input_descs = std::make_shared<std::vector<std::shared_ptr<TensorDesc>>>();
    auto input_buffers = std::make_shared<std::vector<std::shared_ptr<DataBuffer>>>();
    auto output_descs = std::make_shared<std::vector<std::shared_ptr<TensorDesc>>>();
    auto output_buffers = std::make_shared<std::vector<std::shared_ptr<DataBuffer>>>();
    input_descs->reserve(numInputs);
    input_buffers->reserve(numInputs);
    output_descs->reserve(numOutputs);
    output_buffers->reserve(numOutputs);
    for (int i = 0; i < numInputs; ++i) {
        input_descs->push_back(TensorDesc::Create(inputDesc[i]));
        input_buffers->push_back(DataBuffer::Create(inputs[i]));
    }
    for (int i = 0; i < numOutputs; ++i) {
        output_descs->push_back(TensorDesc::Create(outputDesc[i]));
        output_buffers->push_back(DataBuffer::Create(outputs[i]));
    }

    auto hw_cmd = std::make_shared<AclOpCompileAndExecuteCommand>(opType,
                                                                  input_descs, input_buffers,
                                                                  output_descs, output_buffers,
                                                                  OpAttr::Create(attr),
                                                                  engineType, compileFlag, opPath);
    if (xq == nullptr) return AclQueue::DirectLaunch(hw_cmd, stream);
    xq->Submit(hw_cmd);
    return ACL_SUCCESS;
}

aclError XopCompileAndExecuteV2(const char * opType, int numInputs, aclopCompileAndExecuteV2_arg2_t inputDesc, aclopCompileAndExecuteV2_arg3_t inputs, int numOutputs, aclopCompileAndExecuteV2_arg5_t outputDesc, aclopCompileAndExecuteV2_arg6_t outputs, aclopAttr * attr, aclopEngineType engineType, aclopCompileType compileFlag, const char * opPath , aclrtStream stream)
{
    if (InNativeCall()) return OpCompiler::opCompileAndExecuteV2(opType, numInputs, inputDesc, inputs, numOutputs, outputDesc, outputs, attr, engineType, compileFlag, opPath, stream);
    PythonGilRelease gil;
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr && !GetAscendXvmmEnabled()) return OpCompiler::opCompileAndExecuteV2(opType, numInputs, inputDesc, inputs, numOutputs, outputDesc, outputs, attr, engineType, compileFlag, opPath, stream);

    try {
        InitializeCompiler(opType, numInputs, inputDesc, numOutputs, outputDesc,
                           attr, engineType, compileFlag, opPath);
    } catch (aclError error) { return error; }
    auto input_descs = std::make_shared<std::vector<std::shared_ptr<TensorDesc>>>();
    auto input_buffers = std::make_shared<std::vector<std::shared_ptr<DataBuffer>>>();
    auto output_descs = std::make_shared<std::vector<std::shared_ptr<TensorDesc>>>();
    auto output_buffers = std::make_shared<std::vector<std::shared_ptr<DataBuffer>>>();
    input_descs->reserve(numInputs);
    input_buffers->reserve(numInputs);
    output_descs->reserve(numOutputs);
    output_buffers->reserve(numOutputs);
    for (int i = 0; i < numInputs; ++i) {
        input_descs->push_back(TensorDesc::Create(inputDesc[i]));
        input_buffers->push_back(DataBuffer::Create(inputs[i]));
    }
    for (int i = 0; i < numOutputs; ++i) {
        output_descs->push_back(TensorDesc::Create(outputDesc[i]));
        output_buffers->push_back(DataBuffer::Create(outputs[i]));
    }

    auto hw_cmd = std::make_shared<AclOpCompileAndExecuteCommand>(opType,
                                                                  input_descs, input_buffers,
                                                                  output_descs, output_buffers,
                                                                  OpAttr::Create(attr),
                                                                  engineType, compileFlag, opPath, true);
    if (xq == nullptr) return AclQueue::DirectLaunch(hw_cmd, stream);
    xq->Submit(hw_cmd);
    return ACL_SUCCESS;
}

aclError XrtRecordEvent(aclrtEvent event, aclrtStream stream)
{
    if (InNativeCall()) return Driver::rtRecordEvent(event, stream);
    PythonGilRelease gil;
    XDEBG("XrtRecordEvent(event: %p, stream: %p)", event, stream);
    if (event == nullptr) return Driver::rtRecordEvent(event, stream);

    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq == nullptr) return Driver::rtRecordEvent(event, stream);

    auto xevent = std::make_shared<AclEventRecordCommand>(event);
    xq->Submit(xevent);
    g_events.Add(event, xevent);
    return ACL_SUCCESS;
}

aclError XrtSynchronizeEvent(aclrtEvent event)
{
    if (InNativeCall()) return Driver::rtSynchronizeEvent(event);
    PythonGilRelease gil;
    XDEBG("XrtSynchronizeEvent(event: %p)", event);
    auto xevent = g_events.Get(event, nullptr);
    aclError ret = ACL_SUCCESS;
    if (xevent == nullptr) ret = Driver::rtSynchronizeEvent(event);
    else xevent->Wait();
    return ret;
}

aclError XrtDestroyEvent(aclrtEvent event)
{
    if (InNativeCall()) return Driver::rtDestroyEvent(event);
    PythonGilRelease gil;
    auto record = g_events.Del(event, nullptr);
    std::vector<std::weak_ptr<AclCommand>> waits;
    {
        std::lock_guard<std::mutex> lock(event_wait_mutex);
        auto it = event_waits.find(event);
        if (it != event_waits.end()) {
            waits = std::move(it->second);
            event_waits.erase(it);
        }
    }
    // Submission stays asynchronous. Destruction must still follow every
    // native use of this handle, including waits queued on other streams.
    if (record) record->Wait();
    for (const auto &wait : waits) {
        if (auto command = wait.lock()) command->WaitUntil(kCommandStateInFlight);
    }
    return Driver::rtDestroyEvent(event);
}

aclError XrtStreamWaitEvent(aclrtStream stream, aclrtEvent event)
{
    if (InNativeCall()) return Driver::rtStreamWaitEvent(stream, event);
    PythonGilRelease gil;
    class WaitEvent final : public AclCommand {
        aclrtEvent event_;
        std::shared_ptr<AclEventRecordCommand> record_;
    public:
        WaitEvent(aclrtEvent e, std::shared_ptr<AclEventRecordCommand> r)
            : event_(e), record_(std::move(r)) {
            SetProps(kCommandPropertyIdempotent);
        }
        void BeforeLaunch() override {
            if (record_) record_->WaitUntil(kCommandStateInFlight);
        }
        aclError Launch(aclrtStream s) override { return Driver::rtStreamWaitEvent(s, event_); }
    };
    auto queue = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (!queue) {
        if (auto record = g_events.Get(event, nullptr)) record->WaitUntil(kCommandStateInFlight);
        return Driver::rtStreamWaitEvent(stream, event);
    }
    auto command = std::make_shared<WaitEvent>(event, g_events.Get(event, nullptr));
    TrackEventWait(event, command);
    queue->Submit(command);
    return ACL_SUCCESS;
}

aclError XrtQueryEvent(aclrtEvent event, aclrtEventStatus *status)
{
    if (!InNativeCall() && status && EventRecordPending(event)) {
        *status = ACL_EVENT_STATUS_NOT_READY;
        return ACL_SUCCESS;
    }
    auto ret = Driver::rtQueryEvent(event, status);
    return ret;
}

aclError XrtQueryEventStatus(aclrtEvent event, aclrtEventRecordedStatus *status)
{
    if (!InNativeCall() && status && EventRecordPending(event)) {
        *status = ACL_EVENT_RECORDED_STATUS_NOT_READY;
        return ACL_SUCCESS;
    }
    auto ret = Driver::rtQueryEventStatus(event, status);
    return ret;
}

aclError XrtSynchronizeDevice()
{
    if (InNativeCall()) return Driver::rtSynchronizeDevice();
    PythonGilRelease gil;
    XDEBG("XrtSynchronizeDevice()");
    auto res = XQueueManager::ForEachWaitAll();
    XASSERT(res == kXSchedSuccess, "failed to WaitAll() on all XQueues, err: %d", res);
    auto ret = Driver::rtSynchronizeDevice();
    return ret;
}

aclError XrtSynchronizeDeviceWithTimeout(int32_t timeout)
{
    if (InNativeCall()) return Driver::rtSynchronizeDeviceWithTimeout(timeout);
    PythonGilRelease gil;
    XDEBG("XrtSynchronizeDeviceWithTimeout(timeout: %d)", timeout);
    auto res = XQueueManager::ForEachWaitAll();
    XASSERT(res == kXSchedSuccess, "failed to WaitAll() on all XQueues, err: %d", res);
    auto ret = Driver::rtSynchronizeDeviceWithTimeout(timeout);
    return ret;
}

aclError XrtSynchronizeStream(aclrtStream stream)
{
    if (InNativeCall()) return Driver::rtSynchronizeStream(stream);
    PythonGilRelease gil;
    XDEBG("XrtSynchronizeStream(stream: %p)", stream);
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq != nullptr) xq->WaitAll();
    auto ret = Driver::rtSynchronizeStream(stream);
    return ret;
}

aclError XrtSynchronizeStreamWithTimeout(aclrtStream stream, int32_t timeout)
{
    if (InNativeCall()) return Driver::rtSynchronizeStreamWithTimeout(stream, timeout);
    PythonGilRelease gil;
    XDEBG("XrtSynchronizeStreamWithTimeout(stream: %p, timeout: %d)", stream, timeout);
    auto xq = HwQueueManager::GetXQueue(GetHwQueueHandle(stream));
    if (xq != nullptr) xq->WaitAll();
    auto ret = Driver::rtSynchronizeStreamWithTimeout(stream, timeout);
    return ret;
}

aclError XrtCreateStream(aclrtStream *stream)
{
    if (InNativeCall()) return Driver::rtCreateStream(stream);
    aclError res = Driver::rtCreateStream(stream);
    if (res != ACL_SUCCESS) return res;
    AutoCreateXQueueForStream(*stream);
    XDEBG("XrtCreateStream(stream: %p)", *stream);
    return res;
}

aclError XrtCreateStreamWithConfig(aclrtStream *stream, uint32_t priority, uint32_t flag)
{
    if (InNativeCall()) return Driver::rtCreateStreamWithConfig(stream, priority, flag);
    aclError res = Driver::rtCreateStreamWithConfig(stream, priority, flag);
    if (res != ACL_SUCCESS) return res;
    AutoCreateXQueueForStream(*stream);
    XDEBG("XrtCreateStreamWithConfig(stream: %p, priority: %u, flag: 0x%x)", *stream, priority, flag);
    return res;
}

aclError XrtDestroyStream(aclrtStream stream)
{
    if (InNativeCall()) return Driver::rtDestroyStream(stream);
    PythonGilRelease gil;
    XDEBG("XrtDestroyStream(stream: %p)", stream);
    AutoDestroyXQueueForStream(stream);
    return Driver::rtDestroyStream(stream);
}

aclError XrtDestroyStreamForce(aclrtStream stream)
{
    if (InNativeCall()) return Driver::rtDestroyStreamForce(stream);
    PythonGilRelease gil;
    XDEBG("XrtDestroyStreamForce(stream: %p)", stream);
    AutoDestroyXQueueForStream(stream);
    return Driver::rtDestroyStreamForce(stream);
}

aclError XrtMemcpy(void *dst, size_t destMax, const void *src, size_t count, aclrtMemcpyKind kind)
{
    if (InNativeCall()) return Driver::rtMemcpy(dst, destMax, src, count, kind);
    PythonGilRelease gil;
    XResult res = XQueueManager::ForEachWaitAll();
    XASSERT(res == kXSchedSuccess, "failed to WaitAll() on all XQueues before aclrtMemcpy, err: %d", res);
    auto lease = XvmmAcquireRange(dst, src, count);
    auto ret = Driver::rtMemcpy(dst, destMax, src, count, kind);
    return ret;
}

aclError XrtMemset(void *devPtr, size_t maxCount, int32_t value, size_t count)
{
    if (InNativeCall()) return Driver::rtMemset(devPtr, maxCount, value, count);
    PythonGilRelease gil;
    XResult res = XQueueManager::ForEachWaitAll();
    XASSERT(res == kXSchedSuccess, "failed to WaitAll() on all XQueues before aclrtMemset, err: %d", res);
    auto lease = XvmmAcquireRange(devPtr, nullptr, count);
    auto ret = Driver::rtMemset(devPtr, maxCount, value, count);
    return ret;
}

aclError XrtMalloc(void **devPtr, size_t size, aclrtMemMallocPolicy policy)
{
    return XrtMallocCommon(devPtr, size, policy);
}

aclError XrtMallocAlign32(void **devPtr, size_t size, aclrtMemMallocPolicy policy)
{
    return XrtMallocCommon(devPtr, size, policy);
}

aclError XrtMallocCached(void **devPtr, size_t size, aclrtMemMallocPolicy policy)
{
    return XrtMallocCommon(devPtr, size, policy);
}

aclError XrtResetDevice(int32_t device)
{
    if (InNativeCall()) return Driver::rtResetDevice(device);
    PythonGilRelease gil;
    if (GetAscendXvmmEnabled()) { XQueueManager::ForEachWaitAll(); XvmmFinish(); }
    return Driver::rtResetDevice(device);
}
aclError XrtResetDeviceForce(int32_t device)
{
    if (InNativeCall()) return Driver::rtResetDeviceForce(device);
    PythonGilRelease gil;
    if (GetAscendXvmmEnabled()) { XQueueManager::ForEachWaitAll(); XvmmFinish(); }
    return Driver::rtResetDeviceForce(device);
}
aclError XrtDestroyContext(aclrtContext context)
{
    if (InNativeCall()) return Driver::rtDestroyContext(context);
    PythonGilRelease gil;
    if (GetAscendXvmmEnabled()) { XQueueManager::ForEachWaitAll(); XvmmFinish(); }
    return Driver::rtDestroyContext(context);
}
aclError XrtFree(void *devPtr)
{
    if (InNativeCall()) return Driver::rtFree(devPtr);
    PythonGilRelease gil;
    if (GetAscendXvmmEnabled()) {
        // The application has completed all uses. Completion APIs publish the
        // matching Unrefs before returning, so Free can directly send Discard.
        AclMemoryManager::GetCurrentXMemoryManager()->Free(devPtr);
        return ACL_SUCCESS;
    }
    XQueueManager::ForEachWaitAll();
    return Driver::rtFree(devPtr);
}

aclError XrtGetMemInfo(aclrtMemAttr attr, size_t *free, size_t *total)
{
    aclError res = Driver::rtGetMemInfo(attr, free, total);
    if (res != ACL_SUCCESS || !GetAscendXvmmEnabled()) return res;
    size_t allocated = AclMemoryManager::GetCurrentXMemoryManager()->GetTotalAllocatedSize();
    *free = *total > allocated ? (*total - allocated) : 0;
    return ACL_SUCCESS;
}

void XopDestroyAttr(const aclopAttr *attr)
{
    if (OpAttr::Destroy(attr)) return;
    Driver::opDestroyAttr(attr);
}

void XDestroyTensorDesc(const aclTensorDesc *desc)
{
    if (TensorDesc::Destroy(desc)) return;
    Driver::DestroyTensorDesc(desc);
}

aclError XDestroyDataBuffer(const aclDataBuffer *dataBuffer)
{
    if (DataBuffer::Destroy(dataBuffer)) return ACL_SUCCESS;
    return Driver::DestroyDataBuffer(dataBuffer);
}

namespace
{

thread_local unsigned managed_list_destroy_depth = 0;

struct ManagedListDestroyScope
{
    ManagedListDestroyScope() { ++managed_list_destroy_depth; }
    ~ManagedListDestroyScope() { --managed_list_destroy_depth; }
};

aclnnStatus DestroyTensorResource(const void *tensor)
{
    return NnBase::DestroyTensor(static_cast<const aclTensor *>(tensor));
}

aclnnStatus DestroyScalarResource(const void *scalar)
{
    return NnBase::DestroyScalar(static_cast<const aclScalar *>(scalar));
}

aclnnStatus DestroyIntArrayResource(const void *array)
{
    return NnBase::DestroyIntArray(static_cast<const aclIntArray *>(array));
}

aclnnStatus DestroyFloatArrayResource(const void *array)
{
    return NnBase::DestroyFloatArray(static_cast<const aclFloatArray *>(array));
}

aclnnStatus DestroyBoolArrayResource(const void *array)
{
    return NnBase::DestroyBoolArray(static_cast<const aclBoolArray *>(array));
}

aclnnStatus DestroyTensorListResource(const void *tensor_list)
{
    ManagedListDestroyScope managed;
    return NnBase::DestroyTensorList(static_cast<const aclTensorList *>(tensor_list));
}

aclnnStatus DestroyScalarListResource(const void *scalar_list)
{
    ManagedListDestroyScope managed;
    return NnBase::DestroyScalarList(static_cast<const aclScalarList *>(scalar_list));
}

} // namespace

aclTensor *XCreateTensor(const int64_t *viewDims, uint64_t viewDimsNum, aclDataType dataType,
                         const int64_t *stride, int64_t offset, aclFormat format,
                         const int64_t *storageDims, uint64_t storageDimsNum,
                         void *tensorData)
{
    if (InNativeCall()) return NnBase::CreateTensor(viewDims, viewDimsNum, dataType, stride, offset, format, storageDims, storageDimsNum, tensorData);
    aclTensor *tensor = NnBase::CreateTensor(viewDims, viewDimsNum, dataType, stride, offset,
                                             format, storageDims, storageDimsNum, tensorData);
    const size_t bytes = TensorStorageBytes(
        dataType, viewDims, viewDimsNum, storageDims, storageDimsNum);
    AclNNResource::Create(tensor, DestroyTensorResource,
                          MakeMemoryContext(tensorData, bytes));
    return tensor;
}

aclScalar *XCreateScalar(void *value, aclDataType dataType)
{
    if (InNativeCall()) return NnBase::CreateScalar(value, dataType);
    aclScalar *scalar = NnBase::CreateScalar(value, dataType);
    AclNNResource::Create(scalar, DestroyScalarResource);
    return scalar;
}

aclIntArray *XCreateIntArray(const int64_t *value, uint64_t size)
{
    if (InNativeCall()) return NnBase::CreateIntArray(value, size);
    aclIntArray *array = NnBase::CreateIntArray(value, size);
    AclNNResource::Create(array, DestroyIntArrayResource);
    return array;
}

aclFloatArray *XCreateFloatArray(const float *value, uint64_t size)
{
    if (InNativeCall()) return NnBase::CreateFloatArray(value, size);
    aclFloatArray *array = NnBase::CreateFloatArray(value, size);
    AclNNResource::Create(array, DestroyFloatArrayResource);
    return array;
}

aclBoolArray *XCreateBoolArray(const bool *value, uint64_t size)
{
    if (InNativeCall()) return NnBase::CreateBoolArray(value, size);
    aclBoolArray *array = NnBase::CreateBoolArray(value, size);
    AclNNResource::Create(array, DestroyBoolArrayResource);
    return array;
}

aclTensorList *XCreateTensorList(const aclTensor *const *value, uint64_t size)
{
    if (InNativeCall()) return NnBase::CreateTensorList(value, size);
    aclTensorList *tensor_list = NnBase::CreateTensorList(value, size);
    std::vector<const void *> resources;
    resources.reserve(size);
    for (uint64_t i = 0; i < size; ++i) resources.push_back(value[i]);
    auto dependencies = AclNNResource::Retain(resources.data(), resources.size());
    auto mem_ctx = AclNNResource::GetMemoryContext(dependencies);
    AclNNResource::Create(tensor_list, DestroyTensorListResource, std::move(mem_ctx),
                          std::move(dependencies));
    return tensor_list;
}

aclScalarList *XCreateScalarList(const aclScalar *const *value, uint64_t size)
{
    if (InNativeCall()) return NnBase::CreateScalarList(value, size);
    aclScalarList *scalar_list = NnBase::CreateScalarList(value, size);
    std::vector<const void *> resources;
    resources.reserve(size);
    for (uint64_t i = 0; i < size; ++i) resources.push_back(value[i]);
    AclNNResource::Create(scalar_list, DestroyScalarListResource, nullptr,
                          AclNNResource::Retain(resources.data(), resources.size()));
    return scalar_list;
}

aclnnStatus XDestroyTensor(const aclTensor *tensor)
{
    if (InNativeCall() && managed_list_destroy_depth == 0) return NnBase::DestroyTensor(tensor);
    if (AclNNResource::Destroy(tensor)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyTensor(tensor);
}

aclnnStatus XDestroyScalar(const aclScalar *scalar)
{
    if (InNativeCall() && managed_list_destroy_depth == 0) return NnBase::DestroyScalar(scalar);
    if (AclNNResource::Destroy(scalar)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyScalar(scalar);
}

aclnnStatus XDestroyIntArray(const aclIntArray *array)
{
    if (InNativeCall()) return NnBase::DestroyIntArray(array);
    if (AclNNResource::Destroy(array)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyIntArray(array);
}

aclnnStatus XDestroyFloatArray(const aclFloatArray *array)
{
    if (InNativeCall()) return NnBase::DestroyFloatArray(array);
    if (AclNNResource::Destroy(array)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyFloatArray(array);
}

aclnnStatus XDestroyBoolArray(const aclBoolArray *array)
{
    if (InNativeCall()) return NnBase::DestroyBoolArray(array);
    if (AclNNResource::Destroy(array)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyBoolArray(array);
}

aclnnStatus XDestroyTensorList(const aclTensorList *tensorList)
{
    if (InNativeCall()) return NnBase::DestroyTensorList(tensorList);
    if (AclNNResource::Destroy(tensorList)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyTensorList(tensorList);
}

aclnnStatus XDestroyScalarList(const aclScalarList *scalarList)
{
    if (InNativeCall()) return NnBase::DestroyScalarList(scalarList);
    if (AclNNResource::Destroy(scalarList)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyScalarList(scalarList);
}

aclnnStatus XDestroyAclOpExecutor(aclOpExecutor *executor)
{
    if (InNativeCall()) return NnBase::DestroyAclOpExecutor(executor);
    if (AclNNExecutor::Destroy(executor)) return static_cast<aclnnStatus>(ACL_SUCCESS);
    return NnBase::DestroyAclOpExecutor(executor);
}

aclnnStatus XSetAclOpExecutorRepeatable(aclOpExecutor *executor)
{
    if (InNativeCall()) return NnBase::SetAclOpExecutorRepeatable(executor);
    return AclNNExecutor::SetRepeatable(executor);
}

void XRecordAclNNWorkspace(aclOpExecutor **executor,
                           const void *const *resources, size_t resource_count)
{
    if (executor == nullptr || *executor == nullptr) return;
    auto retained = AclNNResource::Retain(resources, resource_count);
    auto mem_ctx = AclNNResource::GetMemoryContext(retained);
    AclNNExecutor::RecordWorkspace(*executor, std::move(retained), std::move(mem_ctx));
    if (GetAscendXvmmEnabled()) AclNNExecutor::RecordResources(*executor, resources, resource_count);
}

} // namespace xsched::ascend
