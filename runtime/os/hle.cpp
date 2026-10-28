#include "hle.h"

#include "exp_heap.h"

#include <mutex>

namespace mh3u::os {

Runtime& rt() {
    static Runtime r;
    return r;
}

uint32_t hle_function_address(ppc::GuestFunction fn) {
    static std::mutex m;
    static uint32_t next = core::layout::kHleFunctionBase;
    std::lock_guard g(m);
    uint32_t a = next;
    next += 4;
    ppc::register_indirect(a, fn);
    return a;
}

}  // namespace mh3u::os
