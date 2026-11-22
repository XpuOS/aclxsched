#include "xsched/utils/env.h"
#include "xsched/utils/xassert.h"
#include "xsched/ascend/hal/options.h"

bool xsched::ascend::GetAscendXvmmEnabled()
{
    static const bool enabled = [] {
        const bool requested = GetEnvOption("XSCHED_ASCEND_XVMM", false);
#ifndef XSCHED_HAS_XVMM
        XASSERT(!requested, "XSCHED_ASCEND_XVMM requires a build with the xvmm SDK");
#endif
        return requested;
    }();
    return enabled;
}
