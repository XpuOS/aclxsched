#include "xsched/ascend/shim/shim.h"
#include "xsched/utils/symbol.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace xsched::ascend
{
namespace
{

// These addresses can be consulted during other DSOs' static destruction.
auto &Originals()
{
    static auto *entries = new std::unordered_map<std::string, void *>;
    return *entries;
}

std::mutex &OriginalMutex()
{
    static auto *mutex = new std::mutex;
    return *mutex;
}

} // namespace

void RememberOriginalAclnn(const char *name, void *address)
{
    if (name == nullptr || address == nullptr) return;
    std::lock_guard<std::mutex> lock(OriginalMutex());
    Originals()[name] = address;
}

void *FindOriginalAclnn(const char *name)
{
    std::lock_guard<std::mutex> lock(OriginalMutex());
    auto it = Originals().find(name);
    return it == Originals().end() ? nullptr : it->second;
}

void *ResolveOriginalAclnn(const char *name)
{
    if (auto original = FindOriginalAclnn(name)) return original;
    // Native feature APIs may call another public API through the PLT.
    // RTLD_NEXT cannot see a library loaded with RTLD_LOCAL.
    static void *library = dlopen("libopapi.so", RTLD_NOW | RTLD_LOCAL);
    if (library != nullptr) {
        if (auto original = RealDlSym(library, name)) return original;
    }

    std::vector<void *> addresses;
    {
        std::lock_guard<std::mutex> lock(OriginalMutex());
        for (const auto &entry : Originals()) addresses.push_back(entry.second);
    }
    std::unordered_set<std::string> searched;
    for (void *address : addresses) {
        Dl_info info{};
        if (!dladdr(address, &info) || info.dli_fname == nullptr ||
            !searched.emplace(info.dli_fname).second) continue;
        void *feature = dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        if (feature == nullptr) continue;
        void *original = RealDlSym(feature, name);
        dlclose(feature);
        if (original != nullptr) {
            RememberOriginalAclnn(name, original);
            return original;
        }
    }
    return RealDlSym(RTLD_NEXT, name);
}

} // namespace xsched::ascend
