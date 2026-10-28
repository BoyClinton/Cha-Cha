// Unit test for the MEM expanded-heap bookkeeping (alignment, top allocation, coalescing, size queries).
#include "exp_heap.h"

#include <cstdio>

using mh3u::os::ExpHeap;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

int main() {
    ExpHeap h(0x10000, 0x20000);  // 64 KB
    CHECK(h.total_free() == 0x10000);

    uint32_t a = h.alloc(100, 0x40);
    CHECK(a == 0x10000 && h.size_of(a) == 100);
    uint32_t b = h.alloc(0x100, 0x100);
    CHECK(b == 0x10100);  // aligned past `a`
    uint32_t top = h.alloc(0x200, -0x40);
    CHECK(top == 0x20000 - 0x200);  // negative alignment allocates from the top

    CHECK(h.alloc(0x20000, 4) == 0);  // too large
    uint32_t free_before = h.total_free();
    CHECK(h.free(b));
    CHECK(!h.free(b));  // double free rejected
    CHECK(h.total_free() == free_before + 0x100);

    // freeing everything coalesces back to one block
    CHECK(h.free(a) && h.free(top));
    CHECK(h.total_free() == 0x10000);
    CHECK(h.largest_free(4) == 0x10000);
    CHECK(h.alloc(0x10000, 4) == 0x10000);

    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
