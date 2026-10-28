#include "profiler.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

namespace mh3u::profiler {
namespace {

constexpr int kMaxFrames = 48;
constexpr int kExeCallers = 3;  // Cha-Cha.exe frames kept above a sample outside the exe
using Site = std::array<uint64_t, 1 + kExeCallers>;  // {function outside the exe, its first exe callers}

struct Thread {
    DWORD tid = 0;
    HANDLE h = nullptr;
    uint64_t samples = 0, in_exe = 0;
    std::unordered_map<uint64_t, uint32_t> self;  // innermost frame address -> samples
    std::unordered_map<uint64_t, uint32_t> incl;  // any frame address -> samples it was on the stack
    std::map<Site, uint32_t> blocked;             // samples outside the exe, by blocking site
};

std::atomic<bool> g_running{false};
std::thread g_sampler;
std::vector<std::unique_ptr<Thread>> g_threads;  // sampler thread only, until report() joins it
uint64_t g_exe_lo = 0, g_exe_hi = 0;
std::chrono::steady_clock::time_point g_start;

bool in_exe(uint64_t a) { return a >= g_exe_lo && a < g_exe_hi; }

constexpr size_t kStackCopy = 64 * 1024;

// Copies the top of a suspended thread's stack. Nothing here may take a user-mode lock: the suspended thread could
// hold it (the loader lock while a DLL loads, the heap lock) and the sampler would wait forever.
size_t copy_stack(DWORD64 rsp, uint8_t* buf) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(rsp), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    const DWORD64 end = reinterpret_cast<DWORD64>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t n = static_cast<size_t>(std::min<DWORD64>(end - rsp, kStackCopy));
    __try {
        std::memcpy(buf, reinterpret_cast<const void*>(rsp), n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}

// Stack walk over the copy (after the thread was resumed), with the x64 unwind tables: stack and frame pointers into
// the original stack are rebased into the copy. Code without unwind data (leaf functions, some drivers) is treated as
// a leaf; a bad frame ends the walk instead of crashing the process.
int walk(CONTEXT c, const uint8_t* buf, size_t len, uint64_t* out) {
    const DWORD64 lo = c.Rsp, hi = c.Rsp + len, delta = reinterpret_cast<DWORD64>(buf) - lo;
    const auto rebase = [&](DWORD64& r) {
        if (r >= lo && r < hi) r += delta;
    };
    rebase(c.Rsp);
    rebase(c.Rbp);
    const DWORD64 blo = lo + delta, bhi = hi + delta;
    int n = 0;
    __try {
        while (n < kMaxFrames && c.Rip && c.Rsp >= blo && c.Rsp + 8 <= bhi) {
            out[n++] = c.Rip;
            const DWORD64 sp = c.Rsp;
            DWORD64 image = 0;
            if (PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(c.Rip, &image, nullptr)) {
                void* handler = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &handler, &frame, nullptr);
            } else {
                c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
                c.Rsp += 8;
            }
            rebase(c.Rsp);
            rebase(c.Rbp);
            if (c.Rsp <= sp) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void enumerate() {
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{sizeof te};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        const bool known = std::any_of(g_threads.begin(), g_threads.end(), [&](const auto& t) { return t->tid == te.th32ThreadID; });
        if (known) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        auto t = std::make_unique<Thread>();
        t->tid = te.th32ThreadID;
        t->h = h;
        g_threads.push_back(std::move(t));
    }
    CloseHandle(snap);
}

void sample_loop() {
    timeBeginPeriod(1);
    auto last_enum = std::chrono::steady_clock::time_point{};
    uint64_t frames[kMaxFrames];
    static uint8_t stack[kStackCopy];
    while (g_running.load()) {
        if (const auto now = std::chrono::steady_clock::now(); now - last_enum > std::chrono::milliseconds(200)) {
            enumerate();
            last_enum = now;
        }
        for (auto& t : g_threads) {
            if (SuspendThread(t->h) == static_cast<DWORD>(-1)) continue;  // exited
            CONTEXT c{};
            c.ContextFlags = CONTEXT_FULL;
            const bool ok = GetThreadContext(t->h, &c) != 0;
            const size_t len = ok ? copy_stack(c.Rsp, stack) : 0;
            ResumeThread(t->h);
            const int n = len ? walk(c, stack, len, frames) : 0;
            if (!n) continue;
            ++t->samples;
            ++t->self[frames[0]];
            for (int i = 0; i < n; ++i)  // inclusive: each distinct address once per sample
                if (std::find(frames, frames + i, frames[i]) == frames + i) ++t->incl[frames[i]];
            if (in_exe(frames[0])) {
                ++t->in_exe;
                continue;
            }
            Site s{};
            s[0] = frames[0];
            for (int i = 1, k = 1; i < n && k <= kExeCallers; ++i)
                if (in_exe(frames[i])) s[k++] = frames[i];
            ++t->blocked[s];
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    timeEndPeriod(1);
}

std::string symbol(uint64_t a) {
    if (!a) return "?";
    HANDLE proc = GetCurrentProcess();
    std::string mod;
    IMAGEHLP_MODULE64 m{};
    m.SizeOfStruct = sizeof m;
    if (SymGetModuleInfo64(proc, a, &m)) mod = m.ModuleName;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    auto* s = reinterpret_cast<SYMBOL_INFO*>(buf);
    s->SizeOfStruct = sizeof(SYMBOL_INFO);
    s->MaxNameLen = 512;
    DWORD64 disp = 0;
    if (SymFromAddr(proc, a, &disp, s)) return mod + "!" + s->Name;
    char hex[64];
    std::snprintf(hex, sizeof hex, "%s+0x%llX", mod.empty() ? "?" : mod.c_str(), static_cast<unsigned long long>(a - m.BaseOfImage));
    return hex;
}

std::string thread_name(HANDLE h) {
    PWSTR w = nullptr;
    std::string out;
    if (SUCCEEDED(GetThreadDescription(h, &w)) && w) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            out.resize(static_cast<size_t>(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
        }
        LocalFree(w);
    }
    return out.empty() ? "(unnamed)" : out;
}

template <typename M>
std::vector<std::pair<uint32_t, std::string>> top(const M& m, size_t n) {
    std::vector<std::pair<uint32_t, std::string>> v;
    for (const auto& [name, c] : m) v.emplace_back(c, name);
    std::sort(v.rbegin(), v.rend());
    if (v.size() > n) v.resize(n);
    return v;
}

}  // namespace

void start_from_env() {
    char* env = nullptr;
    size_t len = 0;
    const bool on = _dupenv_s(&env, &len, "MH3U_PROFILE") == 0 && env && *env && *env != '0';
    std::free(env);
    if (!on || g_running.load()) return;
    const auto* exe = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + dos->e_lfanew);
    g_exe_lo = reinterpret_cast<uint64_t>(exe);
    g_exe_hi = g_exe_lo + nt->OptionalHeader.SizeOfImage;
    g_start = std::chrono::steady_clock::now();
    g_running = true;
    g_sampler = std::thread(sample_loop);
    std::printf("profiler: sampling all threads (~1 kHz)\n");
}

bool running() { return g_running.load(); }

void name_this_thread(const wchar_t* name) { SetThreadDescription(GetCurrentThread(), name); }

void report() {
    if (!g_running.exchange(false)) return;
    g_sampler.join();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    std::sort(g_threads.begin(), g_threads.end(), [](const auto& a, const auto& b) { return a->in_exe > b->in_exe; });
    std::printf("profile: %.1f s, %zu threads (busiest in Cha-Cha.exe first; %% of that thread's samples)\n", secs, g_threads.size());
    for (const auto& t : g_threads) {
        if (!t->samples) continue;
        std::map<std::string, uint32_t> self;
        for (const auto& [a, c] : t->self) self[symbol(a)] += c;
        std::map<std::string, uint32_t> blocked;
        for (const auto& [s, c] : t->blocked) {
            std::string k = symbol(s[0]);
            for (int i = 1; i <= kExeCallers && s[i]; ++i) k += "  <- " + symbol(s[i]);
            blocked[k] += c;
        }
        const double total = static_cast<double>(t->samples);
        std::printf("\n== %s (tid %lu): %llu samples, %.1f%% in Cha-Cha.exe\n", thread_name(t->h).c_str(), t->tid,
                    static_cast<unsigned long long>(t->samples), 100.0 * static_cast<double>(t->in_exe) / total);
        if (static_cast<double>(t->in_exe) / total < 0.02 && blocked.size() <= 2) {  // idle helper thread: one line
            for (const auto& [c, k] : top(blocked, 1)) std::printf("   idle %5.1f%%  %s\n", 100.0 * c / total, k.c_str());
            continue;
        }
        for (const auto& [c, k] : top(self, 15)) std::printf("   self %5.1f%%  %s\n", 100.0 * c / total, k.c_str());
        std::map<std::string, uint32_t> incl;  // by function; max over its addresses (no double count per sample)
        for (const auto& [a, c] : t->incl) {
            uint32_t& v = incl[symbol(a)];
            v = std::max(v, c);
        }
        for (const auto& [c, k] : top(incl, 30)) std::printf("   incl %5.1f%%  %s\n", 100.0 * c / total, k.c_str());
        for (const auto& [c, k] : top(blocked, 10)) std::printf("   wait %5.1f%%  %s\n", 100.0 * c / total, k.c_str());
    }
    std::fflush(stdout);
}

}  // namespace mh3u::profiler

#else  // Windows only (thread sampling / guard pages): no-ops elsewhere

#include <pthread.h>

#include <string>

namespace mh3u::profiler {
void start_from_env() {}
bool running() { return false; }
void report() {}
void name_this_thread(const wchar_t* name) {  // Linux: 15 characters, ASCII
    std::string n;
    for (; *name && n.size() < 15; ++name) n += static_cast<char>(*name < 128 ? *name : '?');
    pthread_setname_np(pthread_self(), n.c_str());
}
}  // namespace mh3u::profiler
#endif
