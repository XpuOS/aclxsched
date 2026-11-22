#pragma once
#include "xsched/ascend/hal/options.h"
#include "xsched/preempt/xmem/xmem.h"
#include "xsched/ascend/hal/acl.h"
namespace xsched::ascend {
// Owns one successful, whole-batch working-set admission.
std::shared_ptr<void> XvmmAcquire(std::shared_ptr<preempt::XMemoryManager> manager,
                                std::shared_ptr<preempt::MemorySet> memory);
std::shared_ptr<void> XvmmAcquire(std::shared_ptr<preempt::XMemoryManager> manager,
                                const utils::Region &memory);
std::shared_ptr<void> XvmmAcquireRange(void *dst, const void *src, size_t bytes);
void XvmmFinish();
void XvmmCheck(int result, const char *operation);
}
