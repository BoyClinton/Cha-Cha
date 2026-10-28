#include "exp_heap.h"

#include <algorithm>

namespace mh3u::os {

ExpHeap::ExpHeap(uint32_t start, uint32_t end) : start_(start), end_(end) {
    if (end > start) free_[start] = end - start;
}

void ExpHeap::insert_free(uint32_t addr, uint32_t size) {
    auto it = free_.emplace(addr, size).first;
    if (auto next = std::next(it); next != free_.end() && it->first + it->second == next->first) {
        it->second += next->second;
        free_.erase(next);
    }
    if (it != free_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            free_.erase(it);
        }
    }
}

uint32_t ExpHeap::alloc(uint32_t size, int32_t align) {
    std::lock_guard g(m_);
    if (size == 0) size = 1;
    size = (size + 3) & ~3u;
    const bool from_top = align < 0;
    uint32_t a = static_cast<uint32_t>(from_top ? -align : align);
    if (a < 4) a = 4;
    auto try_block = [&](std::map<uint32_t, uint32_t>::iterator it) -> uint32_t {
        const uint32_t bs = it->first, be = it->first + it->second;
        uint32_t user = from_top ? ((be - size) & ~(a - 1)) : ((bs + a - 1) & ~(a - 1));
        if (user < bs || user + size > be || user + size < user) return 0;
        free_.erase(it);
        if (user > bs) insert_free(bs, user - bs);
        if (user + size < be) insert_free(user + size, be - (user + size));
        used_[user] = {user, size};
        return user;
    };
    if (from_top) {
        for (auto it = free_.end(); it != free_.begin();)
            if (uint32_t u = try_block(--it)) return u;
    } else {
        for (auto it = free_.begin(); it != free_.end(); ++it)
            if (uint32_t u = try_block(it)) return u;
    }
    return 0;
}

bool ExpHeap::free(uint32_t addr) {
    std::lock_guard g(m_);
    auto it = used_.find(addr);
    if (it == used_.end()) return false;
    insert_free(it->second.first, it->second.second);
    used_.erase(it);
    return true;
}

uint32_t ExpHeap::size_of(uint32_t addr) const {
    std::lock_guard g(m_);
    auto it = used_.find(addr);
    return it == used_.end() ? 0 : it->second.second;
}

uint32_t ExpHeap::total_free() const {
    std::lock_guard g(m_);
    uint32_t t = 0;
    for (auto& [a, s] : free_) t += s;
    return t;
}

uint32_t ExpHeap::largest_free(int32_t align) const {
    std::lock_guard g(m_);
    uint32_t a = static_cast<uint32_t>(align < 0 ? -align : align), best = 0;
    if (a < 4) a = 4;
    for (auto& [bs, sz] : free_) {
        uint32_t user = (bs + a - 1) & ~(a - 1);
        if (user < bs + sz) best = std::max(best, (bs + sz - user) & ~3u);
    }
    return best;
}

}  // namespace mh3u::os
