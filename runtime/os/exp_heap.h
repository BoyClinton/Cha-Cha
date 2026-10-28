// Host-side bookkeeping for a Cafe OS expanded heap (MEMCreateExpHeapEx & co.) over a guest address range. Blocks
// are tracked on the host, so guest memory holds only the user data. Negative alignment allocates from the top of
// the heap, as on Cafe OS.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>

namespace mh3u::os {

class ExpHeap {
public:
    ExpHeap(uint32_t start, uint32_t end);

    uint32_t alloc(uint32_t size, int32_t align);  // 0 on failure
    bool free(uint32_t addr);
    uint32_t size_of(uint32_t addr) const;         // 0 if not a live block
    uint32_t total_free() const;
    uint32_t largest_free(int32_t align) const;    // MEMGetAllocatableSizeForExpHeapEx
    bool contains(uint32_t addr) const { return addr >= start_ && addr < end_; }
    uint32_t start() const { return start_; }
    uint32_t end() const { return end_; }

private:
    void insert_free(uint32_t addr, uint32_t size);
    uint32_t start_, end_;
    std::map<uint32_t, uint32_t> free_;  // addr -> size, coalesced
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> used_;  // user addr -> {block start, block size}
    mutable std::recursive_mutex m_;
};

}  // namespace mh3u::os
