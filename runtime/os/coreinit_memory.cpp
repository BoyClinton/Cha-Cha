// coreinit MEM*: expanded heaps, base heap handles and the default-heap function pointers.
#include "exp_heap.h"
#include "hle.h"

#include <cstdio>
#include <cstring>
#include <map>

namespace mh3u::os {
namespace {

ExpHeap* heap(uint32_t handle) {
    auto it = rt().heaps.find(handle);
    return it == rt().heaps.end() ? nullptr : it->second.get();
}

ExpHeap* heap_containing(uint32_t addr) {
    for (auto& [h, hp] : rt().heaps)
        if (hp->contains(addr)) return hp.get();
    return nullptr;
}

constexpr uint32_t kHeapHeader = 0x40;  // guest-visible handle storage at the start of each heap

uint32_t create_exp_heap(uint32_t start, uint32_t size) {
    if (size <= kHeapHeader) return 0;
    rt().heaps[start] = std::make_unique<ExpHeap>(start + kHeapHeader, start + size);
    std::memset(gbase() + start, 0, kHeapHeader);
    ppc::st32(gbase(), start, 0x45585048);  // 'EXPH', as the real heap header's magic
    return start;
}

// MEMHeapHandle MEMCreateExpHeapEx(void* start, u32 size, u16 flags)
HLE_FUNC(coreinit, MEMCreateExpHeapEx) {
    ret(ctx, create_exp_heap(arg(ctx, 0), arg(ctx, 1)));
    if (rt().trace_imports)
        std::fprintf(stderr, "  MEMCreateExpHeapEx(%08X, %08X) -> %08X\n", arg(ctx, 0), arg(ctx, 1), ctx.r[3]);
}
// void* MEMDestroyExpHeap(MEMHeapHandle)
HLE_FUNC(coreinit, MEMDestroyExpHeap) {
    uint32_t h = arg(ctx, 0);
    rt().heaps.erase(h);
    ret(ctx, h);
}
// void* MEMAllocFromExpHeapEx(MEMHeapHandle, u32 size, int align)
HLE_FUNC(coreinit, MEMAllocFromExpHeapEx) {
    ExpHeap* h = heap(arg(ctx, 0));
    uint32_t size = arg(ctx, 1);
    ret(ctx, h ? h->alloc(size, static_cast<int32_t>(arg(ctx, 2))) : 0);
    if (rt().trace_imports && size >= 0x100000)
        std::fprintf(stderr, "  MEMAllocFromExpHeapEx(%08X, %08X, %d) -> %08X\n", arg(ctx, 0), size,
                     static_cast<int32_t>(arg(ctx, 2)), ctx.r[3]);
}
// void MEMFreeToExpHeap(MEMHeapHandle, void*)
HLE_FUNC(coreinit, MEMFreeToExpHeap) {
    if (ExpHeap* h = heap(arg(ctx, 0))) h->free(arg(ctx, 1));
}
// u32 MEMGetSizeForMBlockExpHeap(const void*)
HLE_FUNC(coreinit, MEMGetSizeForMBlockExpHeap) {
    ExpHeap* h = heap_containing(arg(ctx, 0));
    ret(ctx, h ? h->size_of(arg(ctx, 0)) : 0);
}
// u32 MEMGetAllocatableSizeForExpHeapEx(MEMHeapHandle, int align)
HLE_FUNC(coreinit, MEMGetAllocatableSizeForExpHeapEx) {
    ExpHeap* h = heap(arg(ctx, 0));
    ret(ctx, h ? h->largest_free(static_cast<int32_t>(arg(ctx, 1))) : 0);
}
// u32 MEMGetTotalFreeSizeForExpHeap(MEMHeapHandle)
HLE_FUNC(coreinit, MEMGetTotalFreeSizeForExpHeap) {
    ExpHeap* h = heap(arg(ctx, 0));
    ret(ctx, h ? h->total_free() : 0);
}
// MEMHeapHandle MEMGetBaseHeapHandle(MEMBaseHeapType)  (0 = MEM1, 1 = MEM2, 8 = foreground bucket)
HLE_FUNC(coreinit, MEMGetBaseHeapHandle) {
    switch (arg(ctx, 0)) {
        case 0: ret(ctx, rt().mem1_heap); break;
        case 1: ret(ctx, rt().mem2_heap); break;
        case 8: ret(ctx, rt().fg_heap); break;
        default: ret(ctx, 0); break;
    }
}
// MEMHeapHandle MEMSetBaseHeapHandle(MEMArena, MEMHeapHandle) -> previous
HLE_FUNC(coreinit, MEMSetBaseHeapHandle) {
    uint32_t* slot = arg(ctx, 0) == 0 ? &rt().mem1_heap : arg(ctx, 0) == 1 ? &rt().mem2_heap : &rt().fg_heap;
    uint32_t prev = *slot;
    *slot = arg(ctx, 1);
    ret(ctx, prev);
}

// BOOL-ish int OSGetMemBound(OSMemoryType type, u32* addr, u32* size): 1 = MEM1, 2 = MEM2. Returns 0 on success.
// MEM1 is not used by a base heap here, so it is reported whole; MEM2 is the arena of the default/base heap.
HLE_FUNC(coreinit, OSGetMemBound) {
    uint32_t type = arg(ctx, 0), a = 0, s = 0;
    if (type == 1) { a = 0xF4000000; s = 0x02000000; }
    else if (type == 2) { a = core::layout::kMem2HeapBase; s = core::layout::kMem2HeapEnd - core::layout::kMem2HeapBase; }
    else { ret(ctx, static_cast<uint32_t>(-1)); return; }
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), a);
    if (arg(ctx, 2)) ppc::st32(base, arg(ctx, 2), s);
    if (rt().trace_imports) std::fprintf(stderr, "  OSGetMemBound(%u) -> %08X + %08X\n", type, a, s);
    ret(ctx, 0);
}

// BOOL OSGetForegroundBucketFreeArea(u32* addr, u32* size): the foreground bucket lives at 0xE0000000; the 40 MB
// handed to the app here is an assumption (committed by the boot code).
HLE_FUNC(coreinit, OSGetForegroundBucketFreeArea) {
    if (arg(ctx, 0)) ppc::st32(base, arg(ctx, 0), 0xE0000000);
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), 0x02800000);
    ret(ctx, 1);
}
// BOOL OSGetForegroundBucket(u32* addr, u32* size)
HLE_FUNC(coreinit, OSGetForegroundBucket) {
    if (arg(ctx, 0)) ppc::st32(base, arg(ctx, 0), 0xE0000000);
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), 0x02800000);
    ret(ctx, 1);
}

// ---- frame heaps: bump allocation from both ends, freed wholesale (handles from MEMCreateFrmHeapEx, not imported
// by this game; kept for completeness of the API surface it does import)
struct FrmHeap {
    uint32_t start, end, head, tail;
};
std::map<uint32_t, FrmHeap> g_frm;

// void* MEMAllocFromFrmHeapEx(MEMHeapHandle, u32 size, int align)   align < 0: from the tail
HLE_FUNC(coreinit, MEMAllocFromFrmHeapEx) {
    auto it = g_frm.find(arg(ctx, 0));
    if (rt().trace_imports)
        std::fprintf(stderr, "  MEMAllocFromFrmHeapEx(%08X, %08X, %d)%s\n", arg(ctx, 0), arg(ctx, 1),
                     static_cast<int32_t>(arg(ctx, 2)), it == g_frm.end() ? " unknown heap" : "");
    if (it == g_frm.end()) { ret(ctx, 0); return; }
    FrmHeap& h = it->second;
    uint32_t size = (arg(ctx, 1) + 3) & ~3u;
    int32_t align = static_cast<int32_t>(arg(ctx, 2));
    uint32_t a = static_cast<uint32_t>(align < 0 ? -align : align);
    if (a < 4) a = 4;
    if (align >= 0) {
        uint32_t p = (h.head + a - 1) & ~(a - 1);
        if (p + size > h.tail) { ret(ctx, 0); return; }
        h.head = p + size;
        ret(ctx, p);
    } else {
        uint32_t p = (h.tail - size) & ~(a - 1);
        if (p < h.head || size > h.tail) { ret(ctx, 0); return; }
        h.tail = p;
        ret(ctx, p);
    }
}
// void MEMFreeToFrmHeap(MEMHeapHandle, int mode): 1 = head, 2 = tail, 3 = both
HLE_FUNC(coreinit, MEMFreeToFrmHeap) {
    auto it = g_frm.find(arg(ctx, 0));
    if (it == g_frm.end()) return;
    if (arg(ctx, 1) & 1) it->second.head = it->second.start;
    if (arg(ctx, 1) & 2) it->second.tail = it->second.end;
}
// u32 MEMGetAllocatableSizeForFrmHeapEx(MEMHeapHandle, int align)
HLE_FUNC(coreinit, MEMGetAllocatableSizeForFrmHeapEx) {
    auto it = g_frm.find(arg(ctx, 0));
    if (it == g_frm.end()) { ret(ctx, 0); return; }
    int32_t align = static_cast<int32_t>(arg(ctx, 1));
    uint32_t a = static_cast<uint32_t>(align < 0 ? -align : align);
    if (a < 4) a = 4;
    uint32_t p = (it->second.head + a - 1) & ~(a - 1);
    ret(ctx, p < it->second.tail ? (it->second.tail - p) & ~3u : 0);
}

// ---- MEMAllocator over an expanded heap: {const MEMAllocatorFunc* funcs, heap, align, 0}; funcs = {alloc, free}
uint32_t g_allocator_funcs = 0;
void allocator_alloc(ppc::Context& ctx, uint8_t* b) {  // (MEMAllocator*, u32 size)
    ExpHeap* h = heap(ppc::ld32(b, arg(ctx, 0) + 4));
    ret(ctx, h ? h->alloc(arg(ctx, 1), static_cast<int32_t>(ppc::ld32(b, arg(ctx, 0) + 8))) : 0);
}
void allocator_free(ppc::Context& ctx, uint8_t* b) {  // (MEMAllocator*, void*)
    if (ExpHeap* h = heap(ppc::ld32(b, arg(ctx, 0) + 4))) h->free(arg(ctx, 1));
}
// void MEMInitAllocatorForExpHeap(MEMAllocator*, MEMHeapHandle, int align)
HLE_FUNC(coreinit, MEMInitAllocatorForExpHeap) {
    ppc::st32(base, arg(ctx, 0), g_allocator_funcs);
    ppc::st32(base, arg(ctx, 0) + 4, arg(ctx, 1));
    ppc::st32(base, arg(ctx, 0) + 8, arg(ctx, 2));
    ppc::st32(base, arg(ctx, 0) + 12, 0);
}
// void* MEMAllocFromAllocator(MEMAllocator*, u32 size) / void MEMFreeToAllocator(MEMAllocator*, void*): via funcs
HLE_FUNC(coreinit, MEMAllocFromAllocator) {
    uint32_t funcs = ppc::ld32(base, arg(ctx, 0));
    ppc::call_indirect(ctx, base, ppc::ld32(base, funcs));
}
HLE_FUNC(coreinit, MEMFreeToAllocator) {
    uint32_t funcs = ppc::ld32(base, arg(ctx, 0));
    ppc::call_indirect(ctx, base, ppc::ld32(base, funcs + 4));
}

// Default heap = the MEM2 base heap. These three are *data* imports holding function pointers.
void default_alloc(ppc::Context& ctx, uint8_t*) {
    ExpHeap* h = heap(rt().mem2_heap);
    ret(ctx, h ? h->alloc(arg(ctx, 0), 0x40) : 0);
}
void default_alloc_ex(ppc::Context& ctx, uint8_t*) {
    ExpHeap* h = heap(rt().mem2_heap);
    ret(ctx, h ? h->alloc(arg(ctx, 0), static_cast<int32_t>(arg(ctx, 1))) : 0);
}
void default_free(ppc::Context& ctx, uint8_t*) {
    if (ExpHeap* h = heap_containing(arg(ctx, 0))) h->free(arg(ctx, 0));
}

}  // namespace

void register_coreinit_memory(Runtime& r) {
    using namespace core::layout;
    r.mem2_heap = create_exp_heap(kMem2HeapBase, kMem2HeapEnd - kMem2HeapBase);

    HLE_REGISTER(coreinit, MEMCreateExpHeapEx);
    HLE_REGISTER(coreinit, MEMDestroyExpHeap);
    HLE_REGISTER(coreinit, MEMAllocFromExpHeapEx);
    HLE_REGISTER(coreinit, MEMFreeToExpHeap);
    HLE_REGISTER(coreinit, MEMGetSizeForMBlockExpHeap);
    HLE_REGISTER(coreinit, MEMGetAllocatableSizeForExpHeapEx);
    HLE_REGISTER(coreinit, MEMGetTotalFreeSizeForExpHeap);
    HLE_REGISTER(coreinit, MEMGetBaseHeapHandle);
    HLE_REGISTER(coreinit, MEMSetBaseHeapHandle);
    HLE_REGISTER(coreinit, OSGetMemBound);
    HLE_REGISTER(coreinit, MEMAllocFromFrmHeapEx);
    HLE_REGISTER(coreinit, MEMFreeToFrmHeap);
    HLE_REGISTER(coreinit, MEMGetAllocatableSizeForFrmHeapEx);
    HLE_REGISTER(coreinit, MEMInitAllocatorForExpHeap);
    HLE_REGISTER(coreinit, MEMAllocFromAllocator);
    HLE_REGISTER(coreinit, MEMFreeToAllocator);
    // MEM1 (32 MB at 0xF4000000) and the foreground bucket (0xE0000000) base heaps are *frame* heaps on Cafe OS
    // (the game sizes them with MEMGetAllocatableSizeForFrmHeapEx); both regions are committed by the boot code.
    r.mem1_heap = 0xF4000000;
    g_frm[r.mem1_heap] = {0xF4000040, 0xF6000000, 0xF4000040, 0xF6000000};
    r.fg_heap = 0xE0000000;
    g_frm[r.fg_heap] = {0xE0000040, 0xE2800000, 0xE0000040, 0xE2800000};
    g_allocator_funcs = r.mem->alloc_hle(8, 4);
    ppc::st32(gbase(), g_allocator_funcs, hle_function_address(&allocator_alloc));
    ppc::st32(gbase(), g_allocator_funcs + 4, hle_function_address(&allocator_free));
    HLE_REGISTER(coreinit, OSGetForegroundBucketFreeArea);
    HLE_REGISTER(coreinit, OSGetForegroundBucket);

    // data imports: each slot holds a function pointer
    ppc::st32(gbase(), 0xC0007F88, hle_function_address(&default_alloc));     // MEMAllocFromDefaultHeap
    ppc::st32(gbase(), 0xC0007F90, hle_function_address(&default_alloc_ex));  // MEMAllocFromDefaultHeapEx
    ppc::st32(gbase(), 0xC0007F98, hle_function_address(&default_free));      // MEMFreeToDefaultHeap
}

}  // namespace mh3u::os
