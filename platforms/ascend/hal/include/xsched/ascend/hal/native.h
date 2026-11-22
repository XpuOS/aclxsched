#pragma once
namespace xsched::ascend {
// CANN APIs can call other public APIs through the ELF PLT. Nested native
// calls belong to the same admitted operation and must not re-enter its queue.
extern thread_local unsigned native_call_depth;
inline bool InNativeCall() { return native_call_depth != 0; }
class NativeCallScope {
public:
    NativeCallScope() { ++native_call_depth; }
    ~NativeCallScope() { --native_call_depth; }
    NativeCallScope(const NativeCallScope &) = delete;
    NativeCallScope &operator=(const NativeCallScope &) = delete;
};
}
