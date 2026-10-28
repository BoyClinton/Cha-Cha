#include "read_watch.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

#pragma comment(lib, "dbghelp.lib")

namespace mh3u::read_watch {
namespace {

uint8_t* g_base = nullptr;
uint32_t g_ptr = 0, g_offset = 0;  // "@ptr+offset" form; g_ptr 0 = fixed address in g_offset
std::atomic<uintptr_t> g_host{0};  // host address of the watched word once resolved
std::atomic<bool> g_armed{false};
std::mutex g_m;
std::map<std::tuple<uint64_t, uint64_t, uint64_t>, uint64_t> g_hits;  // (rip, caller, caller's caller) -> count
thread_local bool t_stepping = false;

uintptr_t page_of(uintptr_t a) { return a & ~uintptr_t{0xFFF}; }

void arm() {
    DWORD old = 0;
    if (VirtualProtect(reinterpret_cast<void*>(page_of(g_host)), 0x1000, PAGE_READWRITE | PAGE_GUARD, &old)) g_armed = true;
}

// Two callers of the faulting function, by unwinding the faulting context.
void callers(CONTEXT c, uint64_t out[2]) {
    for (int i = 0; i < 2; ++i) {
        out[i] = 0;
        DWORD64 image = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
        if (!fn) {
            c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
            c.Rsp += 8;
        } else {
            void* handler = nullptr;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, fn, &c, &handler, &frame, nullptr);
        }
        if (!c.Rip) return;
        out[i] = c.Rip;
    }
}

LONG CALLBACK handler(EXCEPTION_POINTERS* e) {
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == STATUS_GUARD_PAGE_VIOLATION && g_host) {
        const uintptr_t at = static_cast<uintptr_t>(e->ExceptionRecord->ExceptionInformation[1]);
        if (page_of(at) != page_of(g_host)) return EXCEPTION_CONTINUE_SEARCH;
        if (at + 8 > g_host && at < g_host + 4) {  // a read or write overlapping the watched word
            uint64_t c[2];
            callers(*e->ContextRecord, c);
            std::lock_guard g(g_m);
            ++g_hits[{e->ContextRecord->Rip, c[0], c[1]}];
        }
        e->ContextRecord->EFlags |= 0x100;  // single-step the access, then re-arm
        t_stepping = true;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == STATUS_SINGLE_STEP && t_stepping) {
        t_stepping = false;
        arm();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

std::string symbol(uint64_t a) {
    if (!a) return "-";
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 255;
    DWORD64 disp = 0;
    char out[320];
    if (SymFromAddr(GetCurrentProcess(), a, &disp, s)) std::snprintf(out, sizeof out, "%s+0x%llx", s->Name, static_cast<unsigned long long>(disp));
    else std::snprintf(out, sizeof out, "0x%llx", static_cast<unsigned long long>(a));
    return out;
}

}  // namespace

void start_from_env(uint8_t* base) {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_WATCH_READ") != 0 || !env) return;
    const std::string spec = env;
    std::free(env);
    g_base = base;
    if (spec[0] == '@') {
        const size_t plus = spec.find('+');
        g_ptr = static_cast<uint32_t>(std::strtoul(spec.c_str() + 1, nullptr, 16));
        g_offset = plus == std::string::npos ? 0 : static_cast<uint32_t>(std::strtoul(spec.c_str() + plus + 1, nullptr, 16));
    } else {
        g_offset = static_cast<uint32_t>(std::strtoul(spec.c_str(), nullptr, 16));
    }
    AddVectoredExceptionHandler(1, &handler);
    std::printf("read watch: %s\n", spec.c_str());
}

void poll() {
    if (!g_base) return;
    if (!g_host) {
        uint32_t guest = g_offset;
        if (g_ptr) {
            const uint8_t* p = g_base + g_ptr;
            const uint32_t v = uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
            if (!v) return;
            guest = v + g_offset;
        }
        g_host = reinterpret_cast<uintptr_t>(g_base) + guest;
        std::printf("read watch: guest %08X armed\n", guest);
    }
    arm();  // also re-arms after a guard hit on another thread that has not stepped yet (harmless)
}

void report() {
    if (!g_base) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::lock_guard g(g_m);
    std::printf("read watch: %zu distinct readers\n", g_hits.size());
    for (const auto& [k, n] : g_hits)
        std::printf("  %8llu  %s  <- %s  <- %s\n", static_cast<unsigned long long>(n), symbol(std::get<0>(k)).c_str(),
                    symbol(std::get<1>(k)).c_str(), symbol(std::get<2>(k)).c_str());
}

}  // namespace mh3u::read_watch

#else  // Windows only (thread sampling / guard pages): no-ops elsewhere

namespace mh3u::read_watch {
void start_from_env(uint8_t*) {}
void poll() {}
void report() {}
}  // namespace mh3u::read_watch
#endif
