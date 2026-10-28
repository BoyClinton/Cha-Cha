// HLE import registry: name -> host implementation, and guest code address -> host implementation.
#include "ppc_context.h"

#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mh3u::ppc {
namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}
std::unordered_map<std::string, GuestFunction>& by_name() {
    static std::unordered_map<std::string, GuestFunction> m;
    return m;
}
std::map<uint32_t, GuestFunction>& by_addr() {
    static std::map<uint32_t, GuestFunction> m;
    return m;
}

}  // namespace

void (*import_trace_hook)(const char*) = nullptr;

void register_import(const char* qualified_name, GuestFunction fn) {
    std::lock_guard g(lock());
    by_name()[qualified_name] = fn;
}

GuestFunction resolve_import(const char* qualified_name) {
    std::lock_guard g(lock());
    auto it = by_name().find(qualified_name);
    return it == by_name().end() ? nullptr : it->second;
}

void register_indirect(uint32_t guest_addr, GuestFunction fn) {
    std::lock_guard g(lock());
    by_addr()[guest_addr] = fn;
}

GuestFunction find_indirect(uint32_t guest_addr) {
    std::lock_guard g(lock());
    auto it = by_addr().find(guest_addr);
    return it == by_addr().end() ? nullptr : it->second;
}

}  // namespace mh3u::ppc

namespace mh3u::ppc {

GuestFunction* override_slot(uint32_t guest_addr) {
    static std::mutex m;
    static std::map<uint32_t, GuestFunction> slots;  // map nodes never move: the returned pointers stay valid
    std::lock_guard g(m);
    return &slots[guest_addr];
}

void set_override(uint32_t guest_addr, GuestFunction fn) { *override_slot(guest_addr) = fn; }

}  // namespace mh3u::ppc
