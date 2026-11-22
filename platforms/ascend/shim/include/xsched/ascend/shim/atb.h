#pragma once

namespace xsched::ascend
{

// ATB remains optional: no library is loaded until an ATB API is called.
void *FindAtbInterceptedSymbol(const char *name);

} // namespace xsched::ascend
