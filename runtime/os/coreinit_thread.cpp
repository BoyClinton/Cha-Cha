// coreinit threads: every guest OSThread runs on its own host thread with its own ppc::Context, on the stack the
// game supplies. Created suspended (OSCreateThread), started by OSResumeThread, finished by returning from the
// entry function or OSExitThread; OSJoinThread waits and fetches the exit value. Priorities and core affinity are
// recorded but scheduling is left to the host.
#include "hle.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace mh3u::os {
namespace {

struct ThreadExit {
    int32_t value;
};

struct GuestThread {
    uint32_t addr = 0, entry = 0, argc = 0, argv = 0, stack_top = 0;
    int32_t priority = 0;
    uint32_t created_frame = 0;
    std::atomic<const char*> last_import{nullptr};
    std::atomic<uint64_t> imports{0};
    std::thread host;
    std::mutex m;
    std::condition_variable cv;
    int32_t suspend = 1;
    bool started = false, finished = false;
    int32_t exit_value = 0;
};

std::mutex g_m;
std::map<uint32_t, std::unique_ptr<GuestThread>> g_threads;
thread_local uint32_t t_current = 0;
thread_local struct GuestThread* t_thread = nullptr;

GuestThread* find(uint32_t addr) {
    std::lock_guard g(g_m);
    auto it = g_threads.find(addr);
    return it == g_threads.end() ? nullptr : it->second.get();
}

void run(GuestThread* t) {
    t_current = t->addr;
    t_thread = t;
#ifdef _WIN32
    wchar_t desc[64];  // for the profiler (MH3U_PROFILE) and debuggers
    std::swprintf(desc, 64, L"guest OSThread %08X entry %08X prio %d", t->addr, t->entry, t->priority);
    SetThreadDescription(GetCurrentThread(), desc);
#else
    char desc[16];  // Linux allows 15 characters
    std::snprintf(desc, sizeof desc, "guest %08X", t->addr);
    pthread_setname_np(pthread_self(), desc);
#endif
    ppc::Context ctx{};
    ctx.r[1] = (t->stack_top - 0x20) & ~0xFu;
    ctx.r[3] = t->argc;
    ctx.r[4] = t->argv;
    int32_t value = 0;
    try {
        ppc::call_indirect(ctx, gbase(), t->entry);
        value = static_cast<int32_t>(ctx.r[3]);
    } catch (const ThreadExit& e) {
        value = e.value;
    } catch (const GuestExit& e) {
        std::fprintf(stderr, "guest thread %08X called exit(%d)\n", t->addr, e.code);
        std::fflush(stdout);
        std::_Exit(e.code);
    }
    {
        std::lock_guard g(t->m);
        t->finished = true;
        t->exit_value = value;
    }
    t->cv.notify_all();
}

// BOOL OSCreateThread(OSThread*, OSThreadEntry, s32 argc, void* argv, void* stack_top, u32 stack_size,
//                     s32 priority, OSThreadAttributes)
HLE_FUNC(coreinit, OSCreateThread) {
    auto t = std::make_unique<GuestThread>();
    t->addr = arg(ctx, 0);
    t->entry = arg(ctx, 1);
    t->argc = arg(ctx, 2);
    t->argv = arg(ctx, 3);
    t->stack_top = arg(ctx, 4);
    t->priority = static_cast<int32_t>(arg(ctx, 6));
    t->created_frame = rt().frames.load();
    std::memset(base + t->addr, 0, 0x6A0);
    ppc::st32(base, t->addr + 0x320, 0x74487244);  // 'tHrD'
    if (rt().trace_imports)
        std::fprintf(stderr, "  OSCreateThread(%08X, entry %08X, stack %08X+%X, prio %d)\n", t->addr, t->entry,
                     t->stack_top, arg(ctx, 5), static_cast<int32_t>(arg(ctx, 6)));
    std::lock_guard g(g_m);
    g_threads[t->addr] = std::move(t);
    ret(ctx, 1);
}
// s32 OSResumeThread(OSThread*) -> previous suspend count
HLE_FUNC(coreinit, OSResumeThread) {
    GuestThread* t = find(arg(ctx, 0));
    if (!t) { ret(ctx, 0); return; }
    std::lock_guard g(t->m);
    int32_t prev = t->suspend;
    if (t->suspend > 0 && --t->suspend == 0 && !t->started) {
        t->started = true;
        t->host = std::thread(run, t);
    }
    ret(ctx, static_cast<uint32_t>(prev));
}
// BOOL OSJoinThread(OSThread*, s32* result)
HLE_FUNC(coreinit, OSJoinThread) {
    GuestThread* t = find(arg(ctx, 0));
    if (!t) { ret(ctx, 0); return; }
    WaitScope w("OSJoinThread", arg(ctx, 0), ctx.lr);
    {
        std::unique_lock l(t->m);
        t->cv.wait(l, [&] { return t->finished; });
    }
    if (t->host.joinable()) t->host.join();
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), static_cast<uint32_t>(t->exit_value));
    ret(ctx, 1);
}
// void OSExitThread(s32 value)
HLE_FUNC(coreinit, OSExitThread) { throw ThreadExit{static_cast<int32_t>(arg(ctx, 0))}; }
// void OSCancelThread(OSThread*): cooperative cancellation is not modelled
HLE_FUNC(coreinit, OSCancelThread) {
    std::fprintf(stderr, "[thread] OSCancelThread(%08X) ignored\n", arg(ctx, 0));
}
HLE_FUNC(coreinit, OSGetCurrentThread) { ret(ctx, t_current ? t_current : rt().main_thread); }

struct WaitInfo {
    const char* what;
    uint32_t object, lr;
    uint64_t since_frame;
};
std::mutex g_wait_m;
std::map<uint32_t, WaitInfo> g_waits;  // guest thread -> current wait

}  // namespace

uint32_t current_thread() { return t_current ? t_current : rt().main_thread; }

void note_import(const char* name) {
    if (!t_thread) return;
    t_thread->last_import.store(name, std::memory_order_relaxed);
    t_thread->imports.fetch_add(1, std::memory_order_relaxed);
}

WaitScope::WaitScope(const char* what, uint32_t object, uint32_t lr) {
    std::lock_guard g(g_wait_m);
    g_waits[current_thread()] = {what, object, lr, rt().frames.load()};
}
WaitScope::~WaitScope() {
    std::lock_guard g(g_wait_m);
    g_waits.erase(current_thread());
}

void dump_threads() {
    std::lock_guard g(g_wait_m);
    std::lock_guard t(g_m);
    std::printf("guest threads (%zu + main):\n", g_threads.size());
    const auto line = [&](uint32_t addr, const char* state, uint32_t entry, int32_t prio, const GuestThread* th) {
        std::printf("  %08X entry %08X prio %2d %-9s", addr, entry, prio, state);
        if (th) {
            const char* last = th->last_import.load();
            std::printf(" created f%u, %llu imports, last %s", th->created_frame,
                        static_cast<unsigned long long>(th->imports.load()), last ? last : "-");
        }
        auto w = g_waits.find(addr);
        if (w != g_waits.end())
            std::printf(" waiting in %s(%08X) from %08X since frame %llu", w->second.what, w->second.object, w->second.lr,
                        static_cast<unsigned long long>(w->second.since_frame));
        std::printf("\n");
    };
    line(rt().main_thread, "main", 0, 16, nullptr);
    for (const auto& [addr, th] : g_threads)
        line(addr, th->finished ? "finished" : th->started ? "running" : "suspended", th->entry, th->priority, th.get());
}

void register_coreinit_thread(Runtime&) {
    HLE_REGISTER(coreinit, OSCreateThread);
    HLE_REGISTER(coreinit, OSResumeThread);
    HLE_REGISTER(coreinit, OSJoinThread);
    HLE_REGISTER(coreinit, OSExitThread);
    HLE_REGISTER(coreinit, OSCancelThread);
    HLE_REGISTER(coreinit, OSGetCurrentThread);  // replaces the main-thread-only version
}

}  // namespace mh3u::os
