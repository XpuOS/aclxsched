#pragma once

#include <cstring>
#include <span>

namespace xsched::ascend
{

struct InterceptedSymbol
{
    const char *name;
    void *address;
};

inline const InterceptedSymbol *FindInterceptedSymbol(std::span<const InterceptedSymbol> symbols,
                                                      const char *name)
{
    if (name == nullptr) return nullptr;
    for (const auto &symbol : symbols) {
        if (std::strcmp(symbol.name, name) == 0) return &symbol;
    }
    return nullptr;
}

} // namespace xsched::ascend
