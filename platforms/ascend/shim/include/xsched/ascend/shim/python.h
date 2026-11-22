#pragma once

#include "xsched/utils/symbol.h"

struct _ts;

namespace xsched::ascend
{

// Python bindings can enter a runtime wait with the GIL held, while a queued
// CANN compilation needs Python on another thread. Resolve the optional API
// at each boundary so native applications need neither Python headers nor a
// Python link dependency, including when CANN loads Python later via dlopen.
class PythonGilRelease
{
public:
    PythonGilRelease()
    {
        auto initialized = reinterpret_cast<int (*)()>(RealDlSym(RTLD_DEFAULT, "Py_IsInitialized"));
        auto held = reinterpret_cast<int (*)()>(RealDlSym(RTLD_DEFAULT, "PyGILState_Check"));
        if (initialized == nullptr || held == nullptr || !initialized() || !held()) return;
        auto save = reinterpret_cast<_ts *(*)()>(RealDlSym(RTLD_DEFAULT, "PyEval_SaveThread"));
        restore_ = reinterpret_cast<void (*)(_ts *)>(RealDlSym(RTLD_DEFAULT, "PyEval_RestoreThread"));
        if (save != nullptr && restore_ != nullptr) state_ = save();
    }
    ~PythonGilRelease() { if (state_ != nullptr) restore_(state_); }
    PythonGilRelease(const PythonGilRelease &) = delete;
    PythonGilRelease &operator=(const PythonGilRelease &) = delete;

private:
    _ts *state_ = nullptr;
    void (*restore_)(_ts *) = nullptr;
};

} // namespace xsched::ascend
