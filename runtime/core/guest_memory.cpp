#include "guest_memory.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace mh3u::core {
namespace {

constexpr uint64_t kSpace = 1ull << 32;
uint8_t* g_base = nullptr;  // for the fault reporter

struct Watch {
    uint32_t begin, end, tag;  // page-aligned guest range
};
std::mutex g_watch_m;
std::vector<Watch> g_watches;

#ifdef _WIN32

// A watched range was touched: log it, make it accessible again. True if the access belonged to a watch.
bool hit_watch(uint32_t addr, bool write) {
    std::lock_guard g(g_watch_m);
    for (size_t i = 0; i < g_watches.size(); ++i) {
        const Watch w = g_watches[i];
        if (addr < w.begin || addr >= w.end) continue;
        DWORD old;
        VirtualProtect(g_base + w.begin, w.end - w.begin, PAGE_READWRITE, &old);
        g_watches.erase(g_watches.begin() + static_cast<std::ptrdiff_t>(i));
        std::fprintf(stderr, "watch: CPU %s guest %08X in watched range %08X-%08X (tag %08X), host thread %lu\n",
                     write ? "write to" : "read from", addr, w.begin, w.end, w.tag, GetCurrentThreadId());
        return true;
    }
    return false;
}

LONG CALLBACK fault_reporter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const auto host = static_cast<uint64_t>(r->ExceptionInformation[1]);
    const auto base = reinterpret_cast<uint64_t>(g_base);
    if (host >= base && host < base + kSpace &&
        hit_watch(static_cast<uint32_t>(host - base), r->ExceptionInformation[0] == 1))
        return EXCEPTION_CONTINUE_EXECUTION;
    if (host >= base && host < base + kSpace) {
        std::fprintf(stderr, "guest memory fault: %s guest address %08X (uncommitted)\n",
                     r->ExceptionInformation[0] ? "write to" : "read from", static_cast<uint32_t>(host - base));
    } else {
        std::fprintf(stderr, "host access violation at %p\n", reinterpret_cast<void*>(host));
    }
    // where: module + offset of the faulting instruction (look it up in the module's .pdb)
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(r->ExceptionAddress), &mod))
        GetModuleFileNameA(mod, name, MAX_PATH);
    std::fprintf(stderr, "  at %s+0x%llX\n", name,
                 static_cast<unsigned long long>(reinterpret_cast<uint64_t>(r->ExceptionAddress) - reinterpret_cast<uint64_t>(mod)));
    std::fflush(stderr);
    std::_Exit(3);
}

}  // namespace

static GuestMemory* g_instance = nullptr;

GuestMemory::GuestMemory() {
    base_ = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpace, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS));
    if (!base_) throw std::runtime_error("cannot reserve 4 GB of address space for guest memory");
    g_base = base_;
    g_instance = this;
    page_epoch_ = std::make_unique<std::atomic<uint32_t>[]>(kSpace >> 12);  // value-initialised: 0
}

GuestMemory* GuestMemory::current() { return g_instance; }

void GuestMemory::set_watched(std::vector<Range> ranges) {
    {
        std::lock_guard g(watch_m_);
        watched_ = std::move(ranges);
    }
    if (!scanner_.joinable()) scanner_ = std::thread([this] { scan_loop(); });
}

// Background scanner: stamps written pages with epoch e, then publishes e (release). A reader that loads the epoch
// (acquire) before checking pages therefore never misses a stamp: a stamp is either > what it loaded, or visible.
void GuestMemory::scan_loop() {
    SetThreadDescription(GetCurrentThread(), L"guest memory write watch");
    std::vector<void*> written;
    std::vector<Range> ranges;
    while (!stop_.load()) {
        {
            std::lock_guard g(watch_m_);
            ranges = watched_;
        }
        const uint32_t e = epoch_.load(std::memory_order_relaxed) + 1;
        uint64_t stamped = 0;
        for (const Range& r : ranges) {
            const size_t pages = (size_t{r.size} + 0xFFF) >> 12;
            if (written.size() < pages) written.resize(pages);
            ULONG_PTR count = pages;
            DWORD granularity = 0;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, base_ + r.addr, r.size, written.data(), &count, &granularity) != 0) continue;
            for (ULONG_PTR i = 0; i < count; ++i)
                page_epoch_[(static_cast<uint8_t*>(written[i]) - base_) >> 12].store(e, std::memory_order_relaxed);
            stamped += count;
        }
        epoch_.store(e, std::memory_order_release);
        scan_pages_.fetch_add(stamped, std::memory_order_relaxed);
        scans_.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

bool GuestMemory::written_since(uint32_t addr, uint32_t size, uint32_t since) const {
    if (!size) return false;
    const uint64_t end = std::min<uint64_t>(uint64_t{addr} + size, kSpace);
    for (uint64_t p = addr >> 12; p < (end + 0xFFF) >> 12; ++p)
        if (page_epoch_[p].load(std::memory_order_relaxed) > since) return true;
    return false;
}

GuestMemory::~GuestMemory() {
    stop_ = true;
    if (scanner_.joinable()) scanner_.join();
    if (base_) VirtualFree(base_, 0, MEM_RELEASE);
    if (g_base == base_) g_base = nullptr;
}

bool GuestMemory::commit(uint32_t addr, uint32_t size) {
    return VirtualAlloc(base_ + addr, size, MEM_COMMIT, PAGE_READWRITE) != nullptr;
}

bool GuestMemory::committed(uint32_t addr) const {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(base_ + addr, &mbi, sizeof mbi)) return false;
    return mbi.State == MEM_COMMIT;
}

std::vector<uint32_t> GuestMemory::find(const uint8_t* pattern, size_t n, uint32_t from, uint32_t to, size_t max) const {
    std::vector<uint32_t> out;
    if (!n) return out;
    const std::boyer_moore_horspool_searcher search(pattern, pattern + n);
    for (uint64_t at = from; at < to && out.size() < max;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(base_ + at, &mbi, sizeof mbi)) break;
        const uint64_t region_end = std::min<uint64_t>(to, static_cast<uint64_t>(static_cast<uint8_t*>(mbi.BaseAddress) - base_) + mbi.RegionSize);
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            const uint8_t* lo = base_ + at;
            const uint8_t* hi = base_ + region_end;
            for (const uint8_t* p = lo; out.size() < max;) {
                p = std::search(p, hi, search);
                if (p == hi) break;
                out.push_back(static_cast<uint32_t>(p - base_));
                ++p;
            }
        }
        at = region_end;
    }
    return out;
}

uint32_t GuestMemory::alloc_hle(uint32_t size, uint32_t align) {
    uint32_t a = (hle_next_ + align - 1) & ~(align - 1);
    if (a + size > layout::kHleEnd || a + size < a) throw std::runtime_error("HLE guest object area exhausted");
    hle_next_ = a + size;
    return a;
}

void GuestMemory::watch(uint32_t addr, uint32_t size, uint32_t tag) {
    // whole pages inside the range only: partial pages hold unrelated data (e.g. the GX2 surface structs)
    const uint32_t begin = (addr + 0xFFFu) & ~0xFFFu, end = (addr + size) & ~0xFFFu;
    if (end <= begin) return;
    std::lock_guard g(g_watch_m);
    for (const Watch& w : g_watches)
        if (w.begin < end && begin < w.end) return;  // already watched (overlapping)
    DWORD old;
    if (!VirtualProtect(base_ + begin, end - begin, PAGE_NOACCESS, &old)) return;
    g_watches.push_back({begin, end, tag});
}

void GuestMemory::install_fault_reporter() {
    AddVectoredExceptionHandler(1, fault_reporter);
}

#else  // POSIX: see guest_memory_posix.inc

#include "guest_memory_posix.inc"

#endif

}  // namespace mh3u::core
