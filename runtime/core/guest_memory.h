// Guest address space: the whole 32-bit range is reserved as one host block (guest address A = base() + A, as the
// recompiled code assumes) and regions are committed as the runtime maps them. Accessing an uncommitted address
// faults; install_fault_reporter() turns such faults into a message with the guest address.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mh3u::core {

// Fixed guest memory map used by the runtime (see docs/runtime.md).
namespace layout {
constexpr uint32_t kCodeBase = 0x02000000;      // .syscall + .text
constexpr uint32_t kDataBase = 0x10000000;      // .rodata .data .module_id .bss .ipc_smem
constexpr uint32_t kMem2HeapBase = 0x10600000;  // MEM2 default heap arena (after .ipc_smem)
constexpr uint32_t kMem2HeapEnd = 0x50000000;
constexpr uint32_t kStackBase = 0x60000000;     // guest thread stacks
constexpr uint32_t kStackEnd = 0x64000000;
constexpr uint32_t kHleBase = 0x70000000;       // runtime-owned guest objects (OSThread, argv, ...)
constexpr uint32_t kHleEnd = 0x71000000;
constexpr uint32_t kImportBase = 0xC0000000;    // import stubs/data symbols (data imports are read here)
constexpr uint32_t kImportEnd = 0xC0010000;
constexpr uint32_t kHleFunctionBase = 0xC1000000;  // fake code addresses for host functions (never dereferenced)
}  // namespace layout

class GuestMemory {
public:
    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    uint8_t* base() const { return base_; }
    // Commits [addr, addr + size) (rounded to pages), zero-filled. Returns false on failure.
    bool commit(uint32_t addr, uint32_t size);
    bool committed(uint32_t addr) const;
    // Guest addresses in [from, to) where `n` bytes equal `pattern`, committed memory only (tools: finding where the
    // game keeps data it loaded). Slow-ish (a linear scan), at most `max` results.
    std::vector<uint32_t> find(const uint8_t* pattern, size_t n, uint32_t from, uint32_t to, size_t max = 16) const;

    // Bump allocator in [kHleBase, kHleEnd) for runtime-created guest objects; zeroed, `align` bytes aligned.
    uint32_t alloc_hle(uint32_t size, uint32_t align = 16);

    void install_fault_reporter();

    // Diagnostics: makes the whole pages inside [addr, addr + size) inaccessible; the first access from any thread is logged
    // (read/write, guest address, `tag`) and the pages become accessible again, so execution continues. Used to find
    // CPU accesses to memory the GPU owns (MH3U_WATCH_TARGETS).
    void watch(uint32_t addr, uint32_t size, uint32_t tag);

    // Write tracking (the space is reserved with MEM_WRITE_WATCH), so caches of guest data (GPU textures) can skip
    // re-reading memory nobody wrote. Single caller thread (the renderer's).
    // A background thread scans the `set_watched` ranges (the memory the caller caches; {addr, size}, sorted, not
    // overlapping) over and over, stamping pages written since the previous pass with a new epoch. Scanning only what
    // is cached keeps it cheap; every page whose OS write state is reset gets stamped, so nothing is lost.
    struct Range {
        uint32_t addr, size;
    };
    void set_watched(std::vector<Range> ranges);  // starts the scanner on first use
    // Usage: e = current_epoch(); if (written_since(addr, size, seen)) ...; seen = e. Load the epoch first.
    uint32_t current_epoch() const { return epoch_.load(std::memory_order_acquire); }
    // True if a page of [addr, addr + size) was stamped after epoch `since`. Any write counts: game code, HLE file
    // reads, DMA copies. Writes newer than the scanner's last pass (under a millisecond) show up on a later check.
    bool written_since(uint32_t addr, uint32_t size, uint32_t since) const;
    uint64_t scans() const { return scans_.load(); }
    uint64_t scanned_pages() const { return scan_pages_.load(); }  // pages stamped in all passes
    static GuestMemory* current();  // the instance (one per process)

private:
    uint8_t* base_ = nullptr;
    uint32_t hle_next_ = layout::kHleBase;
    void scan_loop();
    std::unique_ptr<std::atomic<uint32_t>[]> page_epoch_;  // per 4 KB page: epoch of the last write seen (0 = never)
    std::atomic<uint32_t> epoch_{0};
    std::mutex watch_m_;
    std::vector<Range> watched_;  // guarded by watch_m_
    std::thread scanner_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> scans_{0}, scan_pages_{0};
};

}  // namespace mh3u::core
