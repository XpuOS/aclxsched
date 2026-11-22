#pragma once

#include <cstdlib>

#include "xsched/utils/log.h"
#include "xsched/utils/common.h"
#include "xsched/ascend/hal/acl.h"
#include "xsched/ascend/hal/options.h"

namespace xsched::ascend { void XvmmCheck(int result, const char *operation); }

#define ACL_ASSERT(cmd) \
    do { \
        aclError result = cmd; \
        if (UNLIKELY(result != ACL_SUCCESS)) { \
            if (xsched::ascend::GetAscendXvmmEnabled()) \
                xsched::ascend::XvmmCheck(result, #cmd); \
            XERRO("acl error %d", result); \
        } \
    } while (0);
