// coreinit: process/thread basics, GHS C runtime locks, libc memory functions, exit.
#include "hle.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

namespace mh3u::os {
namespace {

// The game runs single-threaded on the host for now; the GHS runtime locks are still real (recursive) mutexes so
// that later host threads stay correct.
std::recursive_mutex g_ghs_lock;

HLE_FUNC(coreinit, OSIsDebuggerInitialized) { ret(ctx, 0); }  // nonzero would hit `tw 31` in __rpx_start

HLE_FUNC(coreinit, __ghsLock) { g_ghs_lock.lock(); }
HLE_FUNC(coreinit, __ghsUnlock) { g_ghs_lock.unlock(); }
// GHS per-object mutexes (r3 = pointer to the mutex handle storage): one host lock for all of them for now.
HLE_FUNC(coreinit, __ghs_mtx_init) {}
HLE_FUNC(coreinit, __ghs_mtx_dst) {}
HLE_FUNC(coreinit, __ghs_mtx_lock) { g_ghs_lock.lock(); }
HLE_FUNC(coreinit, __ghs_mtx_unlock) { g_ghs_lock.unlock(); }

HLE_FUNC(coreinit, memcpy) { std::memcpy(base + arg(ctx, 0), base + arg(ctx, 1), arg(ctx, 2)); }  // r3 unchanged = dst
HLE_FUNC(coreinit, memmove) { std::memmove(base + arg(ctx, 0), base + arg(ctx, 1), arg(ctx, 2)); }
HLE_FUNC(coreinit, memset) { std::memset(base + arg(ctx, 0), static_cast<int>(arg(ctx, 1) & 0xFF), arg(ctx, 2)); }

// cores: the game's main thread runs on core 1 (the Wii U main core)
HLE_FUNC(coreinit, OSGetMainCoreId) { ret(ctx, 1); }
HLE_FUNC(coreinit, OSGetCoreId) { ret(ctx, 1); }
// BOOL OSDisableInterrupts() -> previous state; OSRestoreInterrupts(BOOL) -> previous state
HLE_FUNC(coreinit, OSDisableInterrupts) { ret(ctx, 1); }
HLE_FUNC(coreinit, OSRestoreInterrupts) { ret(ctx, 1); }
// caches and I/O ordering have no host equivalent
HLE_FUNC(coreinit, DCFlushRange) {}
HLE_FUNC(coreinit, DCTouchRange) {}
HLE_FUNC(coreinit, OSEnforceInorderIO) {}
// thread attributes (main thread only for now): accepted, BOOL TRUE
HLE_FUNC(coreinit, OSSetThreadName) {}
HLE_FUNC(coreinit, OSSetThreadPriority) { ret(ctx, 1); }
HLE_FUNC(coreinit, OSSetThreadAffinity) { ret(ctx, 1); }
HLE_FUNC(coreinit, OSGetThreadAffinity) { ret(ctx, 2); }  // core 1 mask
HLE_FUNC(coreinit, OSYieldThread) { std::this_thread::yield(); }
// void OSConsoleWrite(const char* buf, u32 size)
HLE_FUNC(coreinit, OSConsoleWrite) { std::fwrite(base + arg(ctx, 0), 1, arg(ctx, 1), stdout); }
HLE_FUNC(coreinit, OSEnableHomeButtonMenu) { ret(ctx, 1); }
HLE_FUNC(coreinit, OSSetScreenCapturePermission) { ret(ctx, 1); }
HLE_FUNC(coreinit, OSGetShutdownReason) { ret(ctx, 0); }
HLE_FUNC(coreinit, OSBlockThreadsOnExit) {}
HLE_FUNC(coreinit, OSSavesDone_ReadyToRelease) {}
// GHS stdio file locks and errno: the single GHS lock (see __ghs_mtx_*); errno is not kept yet
HLE_FUNC(coreinit, __ghs_flock_create) {}
HLE_FUNC(coreinit, __ghs_flock_destroy) {}
HLE_FUNC(coreinit, __ghs_flock_file) { g_ghs_lock.lock(); }
HLE_FUNC(coreinit, __ghs_funlock_file) { g_ghs_lock.unlock(); }
HLE_FUNC(coreinit, __ghs_ftrylock_file) {
    g_ghs_lock.lock();
    ret(ctx, 0);
}
HLE_FUNC(coreinit, __gh_set_errno) {}

// hardware registers: u16 OSReadRegister16(interface, reg) etc. No hardware behind them: reads give 0 and every
// access is logged once, so a register the game depends on shows up.
void log_register(const char* what, uint32_t a, uint32_t b) {
    static std::mutex m;
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    std::lock_guard g(m);
    if (seen.insert({a, b}).second) std::fprintf(stderr, "[hw] %s(%u, 0x%X)\n", what, a, b);
}
HLE_FUNC(coreinit, OSReadRegister16) {
    log_register("OSReadRegister16", arg(ctx, 0), arg(ctx, 1));
    ret(ctx, 0);
}
HLE_FUNC(coreinit, OSWriteRegister16) { log_register("OSWriteRegister16", arg(ctx, 0), arg(ctx, 1)); }
HLE_FUNC(coreinit, __OSReadRegister32Ex) {
    log_register("__OSReadRegister32Ex", arg(ctx, 0), arg(ctx, 1));
    ret(ctx, 0);
}
HLE_FUNC(coreinit, __OSWriteRegister32Ex) { log_register("__OSWriteRegister32Ex", arg(ctx, 0), arg(ctx, 1)); }

// BSPError bspGetHardwareVersion(u32* version): retail-like value (assumed)
HLE_FUNC(coreinit, bspGetHardwareVersion) {
    if (arg(ctx, 0)) ppc::st32(base, arg(ctx, 0), 0x26100010);
    ret(ctx, 0);
}

// OSDriver_Register(name, priority, callbacks, ...): accepted; driver callbacks are never invoked
HLE_FUNC(coreinit, OSDriver_Register) { ret(ctx, 0); }

HLE_FUNC(coreinit, exit) { throw GuestExit{static_cast<int>(arg(ctx, 0))}; }
HLE_FUNC(coreinit, _Exit) { throw GuestExit{static_cast<int>(arg(ctx, 0))}; }

}  // namespace

void register_coreinit_base(Runtime&) {
    HLE_REGISTER(coreinit, OSIsDebuggerInitialized);
    HLE_REGISTER(coreinit, __ghsLock);
    HLE_REGISTER(coreinit, __ghsUnlock);
    HLE_REGISTER(coreinit, __ghs_mtx_init);
    HLE_REGISTER(coreinit, __ghs_mtx_dst);
    HLE_REGISTER(coreinit, __ghs_mtx_lock);
    HLE_REGISTER(coreinit, __ghs_mtx_unlock);
    HLE_REGISTER(coreinit, memcpy);
    HLE_REGISTER(coreinit, memmove);
    HLE_REGISTER(coreinit, memset);
    HLE_REGISTER(coreinit, OSGetMainCoreId);
    HLE_REGISTER(coreinit, OSGetCoreId);
    HLE_REGISTER(coreinit, OSDisableInterrupts);
    HLE_REGISTER(coreinit, OSRestoreInterrupts);
    HLE_REGISTER(coreinit, DCFlushRange);
    HLE_REGISTER(coreinit, DCTouchRange);
    HLE_REGISTER(coreinit, OSEnforceInorderIO);
    HLE_REGISTER(coreinit, OSSetThreadName);
    HLE_REGISTER(coreinit, OSSetThreadPriority);
    HLE_REGISTER(coreinit, OSSetThreadAffinity);
    HLE_REGISTER(coreinit, OSGetThreadAffinity);
    HLE_REGISTER(coreinit, OSYieldThread);
    HLE_REGISTER(coreinit, OSConsoleWrite);
    HLE_REGISTER(coreinit, OSEnableHomeButtonMenu);
    HLE_REGISTER(coreinit, OSSetScreenCapturePermission);
    HLE_REGISTER(coreinit, OSGetShutdownReason);
    HLE_REGISTER(coreinit, OSBlockThreadsOnExit);
    HLE_REGISTER(coreinit, OSSavesDone_ReadyToRelease);
    HLE_REGISTER(coreinit, __ghs_flock_create);
    HLE_REGISTER(coreinit, __ghs_flock_destroy);
    HLE_REGISTER(coreinit, __ghs_flock_file);
    HLE_REGISTER(coreinit, __ghs_funlock_file);
    HLE_REGISTER(coreinit, __ghs_ftrylock_file);
    HLE_REGISTER(coreinit, __gh_set_errno);
    HLE_REGISTER(coreinit, OSReadRegister16);
    HLE_REGISTER(coreinit, OSWriteRegister16);
    HLE_REGISTER(coreinit, __OSReadRegister32Ex);
    HLE_REGISTER(coreinit, __OSWriteRegister32Ex);
    HLE_REGISTER(coreinit, bspGetHardwareVersion);
    HLE_REGISTER(coreinit, OSDriver_Register);
    HLE_REGISTER(coreinit, exit);
    HLE_REGISTER(coreinit, _Exit);
}

}  // namespace mh3u::os
