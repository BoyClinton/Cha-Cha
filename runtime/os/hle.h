// Shared pieces for HLE implementations of Cafe OS imports (named after the export they implement, CLAUDE.md).
// Calling convention (PowerPC EABI): integer/pointer args in r3..r10, FP args in f1..f8, result in r3 (f1 for FP).
#pragma once

#include "guest_memory.h"
#include "ppc_context.h"

#include <atomic>
#include <filesystem>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace mh3u::os {

class ExpHeap;
}  // namespace mh3u::os
namespace mh3u::fs {
class OverlayFs;
}
namespace mh3u::gfx {
class Presenter;
}
namespace mh3u::os {

// Thrown by the exit() family to unwind out of guest code back to the runtime's boot function.
struct GuestExit {
    int code;
};

struct Runtime {
    core::GuestMemory* mem = nullptr;
    uint32_t main_thread = 0;  // guest OSThread of the main thread
    std::map<uint32_t, std::unique_ptr<ExpHeap>> heaps;  // MEM heap handle -> heap
    uint32_t mem1_heap = 0, mem2_heap = 0, fg_heap = 0;  // MEMGetBaseHeapHandle arenas
    bool trace_imports = false;
    std::atomic<uint32_t> frames{0};          // GX2SwapScanBuffers calls
    uint32_t exit_after_frames = 0;           // MH3U_EXIT_AFTER_FRAMES (0 = run until closed)
    std::atomic<bool> exit_requested{false};  // set by the swap that reaches exit_after_frames
    fs::OverlayFs* vfs = nullptr;  // /vol/content, /vol/code, /vol/save
    gfx::Presenter* presenter = nullptr;  // Vulkan device (+ window unless headless); null if init failed
    std::filesystem::path data_dir;       // settings, saves, caches (runtime/boot/app_paths.h)
    std::filesystem::path shader_dir;     // translated shaders: <key>.<stage>.spv + .meta
};

Runtime& rt();
inline uint8_t* gbase() { return rt().mem->base(); }

inline uint32_t arg(const ppc::Context& c, int i) { return c.r[3 + i]; }
inline void ret(ppc::Context& c, uint32_t v) { c.r[3] = v; }
inline std::string guest_cstr(uint32_t addr) {
    std::string s;
    if (!addr) return s;
    for (const uint8_t* p = gbase() + addr; *p; ++p) s += static_cast<char>(*p);
    return s;
}

// Register a host function under "rpl.Name".
#define HLE_REGISTER(rpl, name) ::mh3u::ppc::register_import(#rpl "." #name, &hle_##rpl##_##name)
#define HLE_FUNC(rpl, name) static void hle_##rpl##_##name([[maybe_unused]] ::mh3u::ppc::Context& ctx, [[maybe_unused]] uint8_t* base)

// Gives a host function a fake guest code address (call_indirect resolves it), e.g. for function-pointer data
// imports like MEMAllocFromDefaultHeap.
uint32_t hle_function_address(ppc::GuestFunction fn);

// Stall diagnostics: blocking HLE waits register what the calling guest thread waits on (and the guest call site);
// dump_threads() prints every guest thread with its entry point and current wait (also via OSSetThreadName names).
struct WaitScope {
    WaitScope(const char* what, uint32_t object, uint32_t lr);
    ~WaitScope();
};
void dump_threads();
uint32_t current_thread();
void note_import(const char* name);  // last import per guest thread, for dump_threads (tracing only)

void register_coreinit_base(Runtime& r);
void register_coreinit_memory(Runtime& r);
void register_coreinit_sync(Runtime& r);
void register_coreinit_time(Runtime& r);
void register_nn_act(Runtime& r);
void register_coreinit_fs(Runtime& r);
void register_nn_save(Runtime& r);
void register_coreinit_config(Runtime& r);
void register_coreinit_thread(Runtime& r);  // after coreinit_base
void register_input(Runtime& r);
void register_coreinit_dynload(Runtime& r);
void register_applets(Runtime& r);
void register_offline(Runtime& r);
void register_coreinit_misc(Runtime& r);
void register_h264(Runtime& r);  // Media Foundation H.264 decoder

}  // namespace mh3u::os
