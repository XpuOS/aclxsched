#include "xsched/preempt/xmem/xmem.h"

using namespace xsched::utils;
using namespace xsched::preempt;

void *XMemoryManager::Allocate(std::shared_ptr<HwMemoryRequest> req)
{
    std::lock_guard<std::mutex> lock(mtx_);
    void *ptr = hw_mm_->AllocatePagable(req);
    if (ptr == nullptr) return ptr;
    size_t size = req->Size();
    allocated_[ptr] = size;
    return ptr;
}

void XMemoryManager::Free(void *ptr)
{
    std::lock_guard<std::mutex> lock(mtx_);
    hw_mm_->Free(ptr);
    allocated_.erase(ptr);
}

void XMemoryManager::Advise(std::shared_ptr<utils::Region> mem, XMemoryAdviseType advise)
{
    XDEBG("XMemoryManager::Advise %zu MB type %d", mem->Size() / 1024 / 1024, advise);
    hw_mm_->Advise(mem, advise);
}

void XMemoryManager::UpdateMemoryInfo(uint64_t operation_id)
{
    std::lock_guard<std::mutex> lock(update_mtx_);
    if (operation_id <= last_update_operation_id_) return;
    last_update_operation_id_ = operation_id;
    hw_mm_->UpdateMemoryInfo();
}

void XMemoryManager::OnContextTypeChange(XMemoryContextType type)
{
    hw_mm_->OnContextTypeChange(type);
}

size_t XMemoryManager::GetTotalAllocatedSize()
{
    size_t total_size = 0;
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto &[ptr, size] : allocated_) {
        total_size += size;
    }
    return total_size;
}

size_t XMemoryManager::GetAllocatedSize(void *ptr)
{
    std::lock_guard<std::mutex> lock(mtx_);

    // Use upper_bound to find the first element strictly greater than ptr.
    auto it = allocated_.upper_bound(ptr);
    if (it == allocated_.begin()) return 0;

    // Decrement to get the allocation with the largest address <= ptr.
    --it;

    uintptr_t target = (uintptr_t)ptr;
    uintptr_t base = (uintptr_t)it->first;
    size_t size = it->second;

    if (target < base || target >= base + size) return 0;
    return (base + size) - target;
}

std::shared_ptr<Region> XMemoryManager::GetAllocatedMemory()
{
    std::lock_guard<std::mutex> lock(mtx_);
    auto region = std::make_shared<Region>();
    for (auto &[ptr, size] : allocated_) {
        region->Merge((uint64_t)ptr, (uint64_t)ptr + size);
    }
    return region;
}

std::vector<std::pair<void *, size_t>> XMemoryManager::Intersect(const Region &region)
{
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::pair<void *, size_t>> ranges;
    region.ForEach([&](uint64_t begin, uint64_t end) {
        auto it = allocated_.upper_bound(reinterpret_cast<void *>(begin));
        if (it != allocated_.begin()) --it;
        for (; it != allocated_.end(); ++it) {
            const auto &[ptr, size] = *it;
            const auto base = reinterpret_cast<uintptr_t>(ptr);
            if (base >= end) break;
            const auto lo = std::max<uint64_t>(begin, base);
            const auto hi = std::min<uint64_t>(end, base + size);
            if (lo < hi) ranges.emplace_back(reinterpret_cast<void *>(lo), hi - lo);
        }
    });
    return ranges;
}
