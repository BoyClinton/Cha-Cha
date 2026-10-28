// GX2 HLE: initialisation, scan buffers, surface sizing, GX2R surfaces, timestamps/vsync and swap
// (which presents a frame through gfx::Presenter). Render state is collected into a gfx::DrawState and draws,
// clears, copies and scan-out go to the Vulkan renderer (renderer.h); CPU paths remain for what it cannot handle.
//
// Struct layouts follow the public GX2 headers: GX2Surface (0x74): dim +0, width +4, height +8, depth +0xC,
// mipLevels +0x10, format +0x14, aa +0x18, use +0x1C, imageSize +0x20, image +0x24, mipmapSize +0x28, mipmaps +0x2C,
// tileMode +0x30, swizzle +0x34, alignment +0x38, pitch +0x3C, mipLevelOffset[13] +0x40.
#include "gx2.h"

#include "gx2_surface.h"
#include "hle.h"
#include "host_input.h"
#include "presenter.h"
#include "renderer.h"
#include "tiling.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace mh3u::gfx {
namespace {

using os::arg;
using os::ret;
using os::rt;

constexpr uint64_t kTicksPerSec = 62156250;

std::atomic<uint32_t> g_swap_count{0};
std::atomic<uint64_t> g_timestamp{0};
uint32_t g_swap_interval = 1;
// Frame pacing (MH3U_FPS). The game's simulation runs on a variable timestep (sMain+0x7C = real elapsed time in 1/60 s
// units), so its frame rate is only bounded by the swap interval it asks for (2 = 30 FPS). 0 = honour the swap
// interval (60 Hz / N, original behaviour); > 0 = pace at that many FPS instead; < 0 = unlimited.
double g_target_fps = 0;
uint32_t g_tv_mode = 0, g_tv_format = 0, g_tv_buffering = 0;
uint32_t g_drc_mode = 0;
const auto g_boot = std::chrono::steady_clock::now();

uint64_t now_ticks() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_boot).count();
    return static_cast<uint64_t>(static_cast<double>(ns) * kTicksPerSec / 1e9);
}
void ret64(ppc::Context& ctx, uint64_t v) {
    ctx.r[3] = static_cast<uint32_t>(v >> 32);
    ctx.r[4] = static_cast<uint32_t>(v);
}

// ---- surface sizing
struct FormatInfo {
    uint32_t bytes;  // per element (a 4x4 block for BC formats)
    bool block;
};
FormatInfo format_info(uint32_t format) {
    switch (format & 0x3F) {
        case 0x01: case 0x02: return {1, false};
        case 0x05: case 0x07: case 0x08: case 0x0A: case 0x0B: return {2, false};
        case 0x0D: case 0x0F: case 0x11: case 0x16: case 0x19: case 0x1A: case 0x1B: return {4, false};
        case 0x1C: case 0x1D: case 0x1F: return {8, false};
        case 0x22: return {16, false};
        case 0x31: case 0x34: return {8, true};
        case 0x32: case 0x33: case 0x35: return {16, true};
        default: return {4, false};
    }
}
uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

// GX2Surface <-> addrlib::SurfaceDesc (sizing in runtime/gfx/addrlib)
addrlib::SurfaceDesc read_desc(const uint8_t* base, uint32_t s) {
    addrlib::SurfaceDesc d;
    d.dim = ppc::ld32(base, s);
    d.width = ppc::ld32(base, s + 4);
    d.height = ppc::ld32(base, s + 8);
    d.depth = ppc::ld32(base, s + 0xC);
    d.levels = ppc::ld32(base, s + 0x10);
    d.format = ppc::ld32(base, s + 0x14);
    d.aa = ppc::ld32(base, s + 0x18);
    d.use = ppc::ld32(base, s + 0x1C);
    d.image_size = ppc::ld32(base, s + 0x20);
    d.mip_size = ppc::ld32(base, s + 0x28);
    d.tile_mode = ppc::ld32(base, s + 0x30);
    d.swizzle = ppc::ld32(base, s + 0x34);
    d.alignment = ppc::ld32(base, s + 0x38);
    d.pitch = ppc::ld32(base, s + 0x3C);
    for (int i = 0; i < 13; ++i) d.mip_offset[i] = ppc::ld32(base, s + 0x40 + 4 * i);
    return d;
}
// GX2CalcSurfaceSizeAndAlignment: level count, sizes, alignment, pitch, mip offsets, final tile mode and swizzle
void calc_surface(uint8_t* base, uint32_t s) {
    addrlib::SurfaceDesc d = read_desc(base, s);
    addrlib::calc_size_and_alignment(d);
    ppc::st32(base, s + 0x10, d.levels);
    ppc::st32(base, s + 0x20, d.image_size);
    ppc::st32(base, s + 0x28, d.mip_size);
    ppc::st32(base, s + 0x30, d.tile_mode);
    ppc::st32(base, s + 0x34, d.swizzle);
    ppc::st32(base, s + 0x38, d.alignment);
    ppc::st32(base, s + 0x3C, d.pitch);
    for (int i = 0; i < 13; ++i) ppc::st32(base, s + 0x40 + 4 * i, d.mip_offset[i]);
}

// TV render modes: 1 480p, 2 wide 480p, 3 720p, 4 (720p variant), 5 1080p
void tv_dims(uint32_t mode, uint32_t& w, uint32_t& h) {
    switch (mode) {
        case 1: w = 640; h = 480; break;
        case 2: w = 854; h = 480; break;
        case 5: w = 1920; h = 1080; break;
        default: w = 1280; h = 720; break;
    }
}
uint32_t buffer_count(uint32_t mode) { return mode == 4 ? 3 : std::max(1u, mode); }  // SINGLE 1, DOUBLE 2, TRIPLE 4

#define GX2_FUNC(name) static void hle_gx2_##name([[maybe_unused]] ppc::Context& ctx, [[maybe_unused]] uint8_t* base)

GX2_FUNC(GX2Init) {}  // void GX2Init(u32* attributes): command buffer pool etc. are host-side
GX2_FUNC(GX2Shutdown) {}
GX2_FUNC(GX2TempGetGPUVersion) { ret(ctx, 2); }
GX2_FUNC(GX2GetSystemTVAspectRatio) { ret(ctx, 1); }  // 16:9
// void GX2CalcTVSize(mode, format, buffering, u32* size, BOOL* scaleNeeded)
GX2_FUNC(GX2CalcTVSize) {
    uint32_t w, h;
    tv_dims(arg(ctx, 0), w, h);
    ppc::st32(base, arg(ctx, 3), align_up(w, 32) * align_up(h, 16) * format_info(arg(ctx, 1)).bytes * buffer_count(arg(ctx, 2)));
    if (arg(ctx, 4)) ppc::st32(base, arg(ctx, 4), 0);
}
// void GX2CalcDRCSize(mode, format, buffering, u32* size, BOOL* scaleNeeded): GamePad 854x480
GX2_FUNC(GX2CalcDRCSize) {
    ppc::st32(base, arg(ctx, 3), align_up(854, 32) * align_up(480, 16) * format_info(arg(ctx, 1)).bytes * buffer_count(arg(ctx, 2)));
    if (arg(ctx, 4)) ppc::st32(base, arg(ctx, 4), 0);
}
// void GX2SetTVBuffer(void* buffer, u32 size, mode, format, buffering)
GX2_FUNC(GX2SetTVBuffer) {
    g_tv_mode = arg(ctx, 2);
    g_tv_format = arg(ctx, 3);
    g_tv_buffering = arg(ctx, 4);
    if (rt().trace_imports) std::fprintf(stderr, "  GX2SetTVBuffer(%08X, %X, mode %u, format %X, buffering %u)\n", arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4));
}
GX2_FUNC(GX2SetDRCBuffer) { g_drc_mode = arg(ctx, 2); }
GX2_FUNC(GX2SetTVScale) {}
GX2_FUNC(GX2SetDRCScale) {}
GX2_FUNC(GX2SetTVEnable) {}
GX2_FUNC(GX2SetDRCEnable) {}
GX2_FUNC(GX2SetTVGamma) {}
GX2_FUNC(GX2SetDRCGamma) {}
GX2_FUNC(GX2SetSwapInterval) { g_swap_interval = arg(ctx, 0); }
GX2_FUNC(GX2GetSwapInterval) { ret(ctx, g_swap_interval); }
GX2_FUNC(GX2GetGPUTimeout) { ret(ctx, 10000); }
GX2_FUNC(GX2PrintGPUStatus) {}
// context state: GX2SetupContextStateEx(GX2ContextState*, BOOL profiling); GX2SetContextState(state)
GX2_FUNC(GX2SetupContextStateEx) {}
GX2_FUNC(GX2SetContextState) {}

// surfaces
GX2_FUNC(GX2CalcSurfaceSizeAndAlignment) { calc_surface(base, arg(ctx, 0)); }
GX2_FUNC(GX2GetSurfaceFormatBits) { ret(ctx, format_info(arg(ctx, 0)).bytes * 8); }
// u32 GX2GetSurfaceMipPitch(GX2Surface*, u32 level)
GX2_FUNC(GX2GetSurfaceMipPitch) {
    addrlib::Level l;
    const addrlib::SurfaceDesc d = read_desc(base, arg(ctx, 0));
    ret(ctx, addrlib::level_layout(d, 0, 0, arg(ctx, 1), 0, l) ? l.pitch : d.pitch);
}
// void GX2CalcDepthBufferHiZInfo(GX2DepthBuffer*, u32* size, u32* align)
GX2_FUNC(GX2CalcDepthBufferHiZInfo) {
    const uint32_t s = arg(ctx, 0);
    const uint32_t w = ppc::ld32(base, s + 4), h = ppc::ld32(base, s + 8);
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), align_up((align_up(w, 32) / 8) * (align_up(h, 16) / 8) * 4, 0x800));
    if (arg(ctx, 2)) ppc::st32(base, arg(ctx, 2), 0x800);
}
GX2_FUNC(GX2InitColorBufferRegs) {}
GX2_FUNC(GX2InitDepthBufferRegs) {}
GX2_FUNC(GX2InitDepthBufferHiZEnable) {}
GX2_FUNC(GX2InitTextureRegs) {}
GX2_FUNC(GX2ConvertDepthBufferToTextureSurface) {}

// GX2R: surfaces allocated from the MEM2 default heap (the default GX2R allocator)
uint32_t heap_alloc(ppc::Context& ctx, uint8_t* base, uint32_t size, uint32_t align) {
    ppc::Context c = ctx;
    c.r[3] = size;
    c.r[4] = align;
    ppc::call_indirect(c, base, ppc::ld32(base, 0xC0007F90));  // MEMAllocFromDefaultHeapEx
    return c.r[3];
}
// BOOL GX2RCreateSurface(GX2Surface*, GX2RResourceFlags)
GX2_FUNC(GX2RCreateSurface) {
    const uint32_t s = arg(ctx, 0);
    calc_surface(base, s);
    const uint32_t image = ppc::ld32(base, s + 0x20), mips = ppc::ld32(base, s + 0x28), align = ppc::ld32(base, s + 0x38);
    const uint32_t mem = heap_alloc(ctx, base, image + align_up(mips, 0x800) + 0x800, align);
    ppc::st32(base, s + 0x24, mem);
    ppc::st32(base, s + 0x2C, mips ? mem + align_up(image, 0x800) : 0);
    ret(ctx, mem ? 1 : 0);
}
// BOOL GX2RCreateSurfaceUserMemory(GX2Surface*, u8* image, u8* mipmap, GX2RResourceFlags)
GX2_FUNC(GX2RCreateSurfaceUserMemory) {
    calc_surface(base, arg(ctx, 0));
    ppc::st32(base, arg(ctx, 0) + 0x24, arg(ctx, 1));
    ppc::st32(base, arg(ctx, 0) + 0x2C, arg(ctx, 2));
    ret(ctx, 1);
}
GX2_FUNC(GX2RDestroySurfaceEx) {}  // the heap block is left to the game's heap teardown
// void* GX2RLockSurfaceEx(GX2Surface*, s32 level, GX2RResourceFlags)
GX2_FUNC(GX2RLockSurfaceEx) {
    const uint32_t s = arg(ctx, 0);
    const int32_t level = static_cast<int32_t>(arg(ctx, 1));
    ret(ctx, level <= 0 ? ppc::ld32(base, s + 0x24) : ppc::ld32(base, s + 0x2C) + (level >= 2 ? ppc::ld32(base, s + 0x40 + 4 * (level - 1)) : 0));
}
GX2_FUNC(GX2RUnlockSurfaceEx) {}
GX2_FUNC(GX2RInvalidateSurface) {}
// shaders: buffer sizes for the game to allocate
GX2_FUNC(GX2CalcFetchShaderSizeEx) { ret(ctx, 32 * (arg(ctx, 0) + 4)); }
// void GX2InitFetchShaderEx(GX2FetchShader*, u8* program, u32 count, const GX2AttribStream*, type, tess)
// GX2FetchShader: type +0, regs +4, size +8, program +0xC, attribCount +0x10. No microcode is generated: the
// GX2AttribStream array (0x20 bytes each) is copied into the program buffer, which the renderer reads at draw time.
GX2_FUNC(GX2InitFetchShaderEx) {
    const uint32_t fs = arg(ctx, 0), program = arg(ctx, 1), count = arg(ctx, 2), streams = arg(ctx, 3);
    ppc::st32(base, fs + 0x8, 32 * (count + 4));
    ppc::st32(base, fs + 0xC, program);
    ppc::st32(base, fs + 0x10, count);
    if (program && streams) std::memmove(base + program, base + streams, size_t{count} * 0x20);
}
GX2_FUNC(GX2CalcGeometryShaderInputRingBufferSize) { ret(ctx, arg(ctx, 0) * 0x4000); }
GX2_FUNC(GX2CalcGeometryShaderOutputRingBufferSize) { ret(ctx, arg(ctx, 0) * 0x4000); }

// Samplers: GX2Sampler (3 words) holds our own encoding, read back by GX2SetPixelSampler for the renderer:
// w0 clamp x/y/z (3 bits each) | border << 9 | compare func << 11 | mag << 14 | min << 15 | mip << 16 | aniso << 18;
// w1 min LOD << 16 | max LOD (1/256 units); w2 LOD bias (float bits).
void sampler_bits(uint8_t* base, uint32_t smp, uint32_t lo, uint32_t n, uint32_t v) {
    const uint32_t mask = ((1u << n) - 1) << lo;
    ppc::st32(base, smp, (ppc::ld32(base, smp) & ~mask) | ((v << lo) & mask));
}
uint32_t lod256(double v) { return static_cast<uint32_t>(std::clamp(v, 0.0, 255.0) * 256.0); }
// void GX2InitSampler(GX2Sampler*, GX2TexClampMode, GX2TexXYFilterMode)
GX2_FUNC(GX2InitSampler) {
    const uint32_t smp = arg(ctx, 0), clamp = arg(ctx, 1) & 7, filter = arg(ctx, 2) & 1;
    ppc::st32(base, smp, clamp | clamp << 3 | clamp << 6 | filter << 14 | filter << 15 | 1u << 16);
    ppc::st32(base, smp + 4, lod256(13.0) & 0xFFFF);
    ppc::st32(base, smp + 8, 0);
}
GX2_FUNC(GX2InitSamplerBorderType) { sampler_bits(base, arg(ctx, 0), 9, 2, arg(ctx, 1)); }
// (sampler, x, y, z)
GX2_FUNC(GX2InitSamplerClamping) {
    sampler_bits(base, arg(ctx, 0), 0, 3, arg(ctx, 1));
    sampler_bits(base, arg(ctx, 0), 3, 3, arg(ctx, 2));
    sampler_bits(base, arg(ctx, 0), 6, 3, arg(ctx, 3));
}
GX2_FUNC(GX2InitSamplerDepthCompare) { sampler_bits(base, arg(ctx, 0), 11, 3, arg(ctx, 1)); }
GX2_FUNC(GX2InitSamplerFilterAdjust) {}
// (sampler, float minLod, float maxLod, float bias)
GX2_FUNC(GX2InitSamplerLOD) {
    ppc::st32(base, arg(ctx, 0) + 4, lod256(ctx.f[1].ps0) << 16 | lod256(ctx.f[2].ps0));
    const float bias = static_cast<float>(ctx.f[3].ps0);
    uint32_t b;
    std::memcpy(&b, &bias, 4);
    ppc::st32(base, arg(ctx, 0) + 8, b);
}
// (sampler, mag, min, maxAniso)
GX2_FUNC(GX2InitSamplerXYFilter) {
    sampler_bits(base, arg(ctx, 0), 14, 1, arg(ctx, 1));
    sampler_bits(base, arg(ctx, 0), 15, 1, arg(ctx, 2));
    sampler_bits(base, arg(ctx, 0), 18, 3, arg(ctx, 3));
}
// (sampler, zFilter, mipFilter)
GX2_FUNC(GX2InitSamplerZMFilter) { sampler_bits(base, arg(ctx, 0), 16, 2, arg(ctx, 2)); }

// timing
GX2_FUNC(GX2GetLastSubmittedTimeStamp) { ret64(ctx, g_timestamp.load()); }
GX2_FUNC(GX2WaitTimeStamp) { ret(ctx, 1); }  // nothing runs on a GPU yet: every timestamp is reached
// void GX2SampleBottomGPUCycle(u64* out)
GX2_FUNC(GX2SampleBottomGPUCycle) { ppc::st64(base, arg(ctx, 0), now_ticks()); }
GX2_FUNC(GX2GPUTimeToCPUTime) {}  // same timebase: r3:r4 already hold the value
GX2_FUNC(GX2Flush) { ++g_timestamp; }
GX2_FUNC(GX2DrawDone) {
    ++g_timestamp;
    ret(ctx, 1);
}
// The game waits for a vsync in sLtRender::present (0x02BBF9D0, while renderer flags 2 and 4 are set and a frame is
// still queued: cutscenes) and in sApp::presentStandalone (0x02BC1E34). With our own frame pacing (MH3U_FPS other than
// "game") those waits would hold frames back on top of it, so they return at once; game pacing keeps the 60 Hz tick.
GX2_FUNC(GX2WaitForVsync) {
    if (g_target_fps != 0) return;
    using namespace std::chrono;
    static auto next = steady_clock::now();
    next += microseconds(16667);
    auto now = steady_clock::now();
    if (next < now) next = now;
    std::this_thread::sleep_until(next);
}
// void GX2GetSwapStatus(u32* swapCount, u32* flipCount, OSTime* lastFlip, OSTime* lastVsync)
GX2_FUNC(GX2GetSwapStatus) {
    const uint32_t n = g_swap_count.load();
    if (arg(ctx, 0)) ppc::st32(base, arg(ctx, 0), n);
    if (arg(ctx, 1)) ppc::st32(base, arg(ctx, 1), n);
    if (arg(ctx, 2)) ppc::st64(base, arg(ctx, 2), now_ticks());
    if (arg(ctx, 3)) ppc::st64(base, arg(ctx, 3), now_ticks());
}
// ---------------------------------------------------------------------------------------------------------------
// Rendering. GX2 calls that emit GPU commands are "recordable": inside a display list (Begin/End on the calling
// thread) they are appended to that list, otherwise they run immediately; GX2DirectCallDisplayList replays.
// State setters fill g_ds (DrawState); draws, clears, GX2CopySurface and TV scan-out go to the Vulkan renderer
// (renderer.h). The CPU versions below (clears as uniform fills, detile/retile copies, detile + RGBA8 scan-out into
// the TV image) remain for surfaces the renderer cannot take. Draws are also captured for frame dumps.
// ---------------------------------------------------------------------------------------------------------------

struct Surf {
    uint32_t addr = 0, dim = 0, w = 0, h = 0, depth = 0, mips = 0, format = 0, aa = 0, image_size = 0, image = 0;
    uint32_t tile = 0, swizzle = 0, pitch = 0;
};
Surf read_surface(const uint8_t* base, uint32_t a) {
    Surf s;
    s.addr = a;
    s.dim = ppc::ld32(base, a);
    s.w = ppc::ld32(base, a + 4);
    s.h = ppc::ld32(base, a + 8);
    s.depth = ppc::ld32(base, a + 0xC);
    s.mips = ppc::ld32(base, a + 0x10);
    s.format = ppc::ld32(base, a + 0x14);
    s.aa = ppc::ld32(base, a + 0x18);
    s.image_size = ppc::ld32(base, a + 0x20);
    s.image = ppc::ld32(base, a + 0x24);
    s.tile = ppc::ld32(base, a + 0x30);
    s.swizzle = ppc::ld32(base, a + 0x34);
    s.pitch = ppc::ld32(base, a + 0x3C);
    return s;
}
SurfaceLevel level0(const Surf& s, uint32_t slice = 0) {
    const FormatInfo fi = format_info(s.format);
    SurfaceLevel l;
    l.width = fi.block ? (s.w + 3) / 4 : s.w;
    l.height = fi.block ? (s.h + 3) / 4 : s.h;
    l.bpp = fi.bytes * 8;
    l.tile_mode = s.tile;
    l.pitch = s.pitch;
    l.swizzle = s.swizzle;
    l.slice = slice;
    return l;
}

// Tiling apertures: a linear CPU view of a tiled surface (rows of `pitch` elements, as the game sizes it with
// GX2GetSurfaceMipPitch). The hardware detiles on the fly; here the level is detiled into a buffer in guest memory
// at Allocate and written back (retiled) at Free, so writes through the view reach the surface. MH3U reads its
// character-creation colour gradients this way (uIDMakeAppearance 0x0274A9F0 via 0x02957070): with the raw tiled
// image instead, the chosen skin / hair colour came out wrong. Only level 0 is detiled (the game asks for no other).
struct Aperture {
    uint32_t buffer = 0, capacity = 0, image = 0, row_bytes = 0;
    SurfaceLevel level;
};
std::map<uint32_t, Aperture> g_apertures;           // by handle
std::vector<std::pair<uint32_t, uint32_t>> g_aperture_pool;  // freed buffers (address, capacity), reused
uint32_t g_next_aperture = 1;
std::mutex g_aperture_m;
// void GX2AllocateTilingApertureEx(GX2Surface*, u32 level, u32 slice, GX2EndianSwapMode, u32* handle, void** addr)
GX2_FUNC(GX2AllocateTilingApertureEx) {
    const Surf s = read_surface(base, arg(ctx, 0));
    const uint32_t level = arg(ctx, 1), slice = arg(ctx, 2);
    uint32_t handle = 0, addr = s.image;
    if (level == 0 && s.image && s.tile >= 2) {  // tiled: detile into a buffer
        std::lock_guard g(g_aperture_m);
        Aperture a;
        a.level = level0(s, slice);
        a.image = s.image;
        a.row_bytes = a.level.pitch * a.level.bpp / 8;
        const uint32_t need = a.row_bytes * a.level.height;
        auto it = std::find_if(g_aperture_pool.begin(), g_aperture_pool.end(), [&](const auto& p) { return p.second >= need; });
        if (it != g_aperture_pool.end()) {
            a.buffer = it->first;
            a.capacity = it->second;
            g_aperture_pool.erase(it);
        } else {
            a.buffer = rt().mem->alloc_hle(need, 256);
            a.capacity = need;
        }
        std::vector<uint8_t> lin(size_t{a.level.width} * a.level.height * (a.level.bpp / 8));
        detile(base + s.image, lin.data(), a.level);
        const size_t w_bytes = size_t{a.level.width} * (a.level.bpp / 8);
        for (uint32_t y = 0; y < a.level.height; ++y)
            std::memcpy(base + a.buffer + size_t{y} * a.row_bytes, lin.data() + y * w_bytes, w_bytes);
        handle = g_next_aperture++;
        addr = a.buffer;
        g_apertures[handle] = a;
    }
    if (arg(ctx, 4)) ppc::st32(base, arg(ctx, 4), handle);
    if (arg(ctx, 5)) ppc::st32(base, arg(ctx, 5), addr);
}
// void GX2FreeTilingAperture(u32 handle): what was written through the view goes back into the tiled surface
GX2_FUNC(GX2FreeTilingAperture) {
    std::lock_guard g(g_aperture_m);
    auto it = g_apertures.find(arg(ctx, 0));
    if (it == g_apertures.end()) return;
    const Aperture& a = it->second;
    const size_t w_bytes = size_t{a.level.width} * (a.level.bpp / 8);
    std::vector<uint8_t> lin(w_bytes * a.level.height);
    for (uint32_t y = 0; y < a.level.height; ++y)
        std::memcpy(lin.data() + y * w_bytes, base + a.buffer + size_t{y} * a.row_bytes, w_bytes);
    retile(lin.data(), base + a.image, a.level);
    g_aperture_pool.emplace_back(a.buffer, a.capacity);
    g_apertures.erase(it);
}


uint16_t half(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    int32_t e = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = (x >> 13) & 0x3FF;
    if (e <= 0) return static_cast<uint16_t>(sign);
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7C00);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(e) << 10) | m);
}
float from_half(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t x;
    if (e == 0) x = sign;
    else if (e == 31) x = sign | 0x7F800000 | (m << 13);
    else x = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
uint8_t unorm8(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }
uint32_t unorm(float v, uint32_t bits) {
    return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * static_cast<float>((1u << bits) - 1) + 0.5f);
}
void put_le32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// GPU element encoding of a clear colour (Latte stores multi-byte elements little-endian).
uint32_t encode_color(uint32_t format, const float c[4], uint8_t* px) {
    switch (format & 0xFFF) {
        case 0x01A: case 0x41A:  // RGBA8 unorm / srgb
            for (int k = 0; k < 4; ++k) px[k] = unorm8(c[k]);
            return 4;
        case 0x019:  // RGB10A2
            put_le32(px, unorm(c[0], 10) | unorm(c[1], 10) << 10 | unorm(c[2], 10) << 20 | unorm(c[3], 2) << 30);
            return 4;
        case 0x81F:  // RGBA16F
            for (int k = 0; k < 4; ++k) {
                const uint16_t v = half(c[k]);
                px[2 * k] = static_cast<uint8_t>(v);
                px[2 * k + 1] = static_cast<uint8_t>(v >> 8);
            }
            return 8;
        case 0x80E: {  // R32F
            uint32_t v;
            std::memcpy(&v, &c[0], 4);
            put_le32(px, v);
            return 4;
        }
        default: {
            const uint32_t n = format_info(format).bytes;
            for (uint32_t k = 0; k < n; ++k) px[k] = unorm8(c[k % 4]);
            return n;
        }
    }
}

// linear GPU elements -> opaque RGBA8 (scan-out ignores alpha)
void to_rgba8(uint32_t format, const uint8_t* src, uint8_t* dst, size_t n) {
    switch (format & 0xFFF) {
        case 0x019:
            for (size_t i = 0; i < n; ++i) {
                const uint32_t v = src[4 * i] | src[4 * i + 1] << 8 | src[4 * i + 2] << 16 | uint32_t{src[4 * i + 3]} << 24;
                dst[4 * i] = static_cast<uint8_t>(((v & 0x3FF) * 255 + 511) / 1023);
                dst[4 * i + 1] = static_cast<uint8_t>((((v >> 10) & 0x3FF) * 255 + 511) / 1023);
                dst[4 * i + 2] = static_cast<uint8_t>((((v >> 20) & 0x3FF) * 255 + 511) / 1023);
                dst[4 * i + 3] = 255;
            }
            break;
        case 0x81F:
            for (size_t i = 0; i < n; ++i)
                for (int k = 0; k < 4; ++k)
                    dst[4 * i + k] = k == 3 ? 255 : unorm8(from_half(static_cast<uint16_t>(src[8 * i + 2 * k] | src[8 * i + 2 * k + 1] << 8)));
            break;
        default: {  // RGBA8 and anything else 32-bit: shown as RGBA8
            const uint32_t* in = reinterpret_cast<const uint32_t*>(src);
            uint32_t* out = reinterpret_cast<uint32_t*>(dst);
            for (size_t i = 0; i < n; ++i) out[i] = in[i] | 0xFF000000u;
            break;
        }
    }
}

// ---- render state, captured draws, presented image
struct DrawInfo {
    uint32_t mode = 0, count = 0, first = 0, instances = 0, index_type = 0, indexed = 0;
    uint32_t vs = 0, ps = 0, fs = 0, vs_program = 0, vs_size = 0, ps_program = 0, ps_size = 0;
    uint32_t color0 = 0, depth = 0, tex0 = 0, textures = 0, attribs = 0;
    float viewport[4] = {};
};
std::mutex g_state_m;
// Holds g_state_m unless this thread already does: a display-list replay takes it once for all its commands (thousands a
// frame) instead of once per command.
thread_local bool t_state_locked = false;
struct StateLock {
    bool owns = !t_state_locked;
    StateLock() {
        if (!owns) return;
        g_state_m.lock();
        t_state_locked = true;
    }
    ~StateLock() {
        if (!owns) return;
        t_state_locked = false;
        g_state_m.unlock();
    }
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
};
uint32_t g_color[8] = {}, g_depth_buffer = 0, g_vs = 0, g_ps = 0, g_fs = 0, g_tex[16] = {}, g_attribs = 0;
float g_viewport[4] = {};
std::unique_ptr<Renderer> g_renderer;
DrawState g_ds;              // render state for the renderer
uint32_t g_gs = 0, g_shader_mode = 1;

GuestSurface guest_surface(const uint8_t* base, uint32_t a) {
    GuestSurface g;
    if (!a) return g;
    g.dim = ppc::ld32(base, a);
    g.width = ppc::ld32(base, a + 4);
    g.height = ppc::ld32(base, a + 8);
    g.depth = ppc::ld32(base, a + 0xC);
    g.mips = ppc::ld32(base, a + 0x10);
    g.format = ppc::ld32(base, a + 0x14);
    g.aa = ppc::ld32(base, a + 0x18);
    g.use = ppc::ld32(base, a + 0x1C);
    g.image_size = ppc::ld32(base, a + 0x20);
    g.image = ppc::ld32(base, a + 0x24);
    g.mip_size = ppc::ld32(base, a + 0x28);
    g.mipmaps = ppc::ld32(base, a + 0x2C);
    g.tile = ppc::ld32(base, a + 0x30);
    g.swizzle = ppc::ld32(base, a + 0x34);
    g.pitch = ppc::ld32(base, a + 0x3C);
    for (int i = 0; i < 13; ++i) g.mip_offset[i] = ppc::ld32(base, a + 0x40 + 4 * i);
    return g;
}
std::vector<DrawInfo> g_frame_draws, g_last_draws;
std::vector<std::string> g_frame_events, g_last_events;  // clears / copies / scan-outs, for frame dumps
uint64_t g_total_draws = 0, g_total_clears = 0, g_total_copies = 0;
std::vector<uint8_t> g_tv_image;
uint32_t g_tv_w = 0, g_tv_h = 0;
std::set<uint32_t> g_dump_frames;  // MH3U_DUMP_FRAME=N[,N...]
std::atomic<int> g_gamepad_view{0};  // see gx2.h cycle_gamepad_view
bool g_watch_targets = false;          // MH3U_WATCH_TARGETS: log CPU accesses to render-target memory
// host time spent in CPU surface work, by kind (clear, copy, scan-out)
std::atomic<uint64_t> g_ns_clear{0}, g_ns_copy{0}, g_ns_scan{0};
struct ScopedNs {
    std::atomic<uint64_t>& acc;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~ScopedNs() {
        acc += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
};

// void GX2SetColorBuffer(const GX2ColorBuffer*, GX2RenderTarget): the GX2Surface is the struct's first member
GX2_FUNC(GX2SetColorBuffer) {
    StateLock g;
    const uint32_t t = arg(ctx, 1);
    if (t >= 8) return;
    g_color[t] = arg(ctx, 0);
    g_ds.color[t] = guest_surface(base, arg(ctx, 0));
    g_ds.color_bound[t] = arg(ctx, 0) != 0;
}
GX2_FUNC(GX2SetDepthBuffer) {
    StateLock g;
    g_depth_buffer = arg(ctx, 0);
    g_ds.depth = guest_surface(base, arg(ctx, 0));
    g_ds.depth_bound = arg(ctx, 0) != 0;
}
GX2_FUNC(GX2SetVertexShader) {
    StateLock g;
    g_vs = g_ds.vs = arg(ctx, 0);
}
GX2_FUNC(GX2SetPixelShader) {
    StateLock g;
    g_ps = g_ds.ps = arg(ctx, 0);
}
GX2_FUNC(GX2SetGeometryShader) {
    StateLock g;
    g_gs = arg(ctx, 0);
}
// void GX2SetShaderModeEx(GX2ShaderMode, vsGprs, vsStack, gsGprs, gsStack, psGprs, psStack): mode 2 = geometry
GX2_FUNC(GX2SetShaderModeEx) {
    StateLock g;
    g_shader_mode = arg(ctx, 0);
}
GX2_FUNC(GX2SetFetchShader) {
    StateLock g;
    const uint32_t fs = g_fs = arg(ctx, 0);
    g_ds.streams.clear();
    if (!fs) return;
    const uint32_t program = ppc::ld32(base, fs + 0xC), count = std::min(ppc::ld32(base, fs + 0x10), 32u);
    for (uint32_t i = 0; i < count && program; ++i) {
        const uint32_t a = program + i * 0x20;
        AttribStream st;
        st.location = ppc::ld32(base, a);
        st.buffer = ppc::ld32(base, a + 4);
        st.offset = ppc::ld32(base, a + 8);
        st.format = ppc::ld32(base, a + 0xC);
        st.type = ppc::ld32(base, a + 0x10);
        st.divisor = ppc::ld32(base, a + 0x14);
        st.mask = ppc::ld32(base, a + 0x18);
        st.endian = ppc::ld32(base, a + 0x1C);
        g_ds.streams.push_back(st);
    }
}
// void GX2SetPixelTexture(const GX2Texture*, u32 unit): GX2Texture = GX2Surface + view fields, compSel at +0x84
GX2_FUNC(GX2SetPixelTexture) {
    StateLock g;
    const uint32_t unit = arg(ctx, 1);
    if (unit < 16) g_tex[unit] = arg(ctx, 0);
    if (unit >= 18) return;
    TextureBinding& t = g_ds.stage[kStagePixel].tex[unit];
    t.bound = arg(ctx, 0) != 0;
    t.surf = guest_surface(base, arg(ctx, 0));
    if (t.bound) t.comp_sel = ppc::ld32(base, arg(ctx, 0) + 0x84);
}
// void GX2SetPixelSampler(const GX2Sampler*, u32 unit)
GX2_FUNC(GX2SetPixelSampler) {
    StateLock g;
    if (arg(ctx, 1) >= 18 || !arg(ctx, 0)) return;
    SamplerState& smp = g_ds.stage[kStagePixel].samp[arg(ctx, 1)];
    smp.bound = true;
    for (int k = 0; k < 3; ++k) smp.words[k] = ppc::ld32(base, arg(ctx, 0) + 4 * k);
}
// void GX2Set{Vertex,Pixel,Geometry}UniformBlock(u32 location, u32 size, const void* data)
void set_uniform_block(int stage, ppc::Context& ctx) {
    StateLock g;
    if (arg(ctx, 0) < 16) g_ds.stage[stage].ubo[arg(ctx, 0)] = {arg(ctx, 2), arg(ctx, 1)};
}
GX2_FUNC(GX2SetVertexUniformBlock) { set_uniform_block(kStageVertex, ctx); }
GX2_FUNC(GX2SetPixelUniformBlock) { set_uniform_block(kStagePixel, ctx); }
GX2_FUNC(GX2SetGeometryUniformBlock) { set_uniform_block(kStageGeometry, ctx); }
// void GX2SetAttribBuffer(u32 index, u32 size, u32 stride, const void* buffer)
GX2_FUNC(GX2SetAttribBuffer) {
    StateLock g;
    if (arg(ctx, 0) < 32) g_attribs |= 1u << arg(ctx, 0);
    if (arg(ctx, 0) < 16) g_ds.attribs[arg(ctx, 0)] = {arg(ctx, 3), arg(ctx, 1), arg(ctx, 2)};
}
// void GX2SetViewport(float x, float y, float w, float h, float near, float far)
GX2_FUNC(GX2SetViewport) {
    StateLock g;
    for (int k = 0; k < 4; ++k) g_viewport[k] = static_cast<float>(ctx.f[1 + k].ps0);
    for (int k = 0; k < 6; ++k) g_ds.viewport[k] = static_cast<float>(ctx.f[1 + k].ps0);
}
// void GX2SetScissor(u32 x, u32 y, u32 w, u32 h)
GX2_FUNC(GX2SetScissor) {
    StateLock g;
    g_ds.scissor_set = true;
    for (int k = 0; k < 4; ++k) g_ds.scissor[k] = arg(ctx, k);
}
// void GX2SetColorControl(GX2LogicOp, u8 blendEnableMask, BOOL multiWrite, BOOL colorWrite)
GX2_FUNC(GX2SetColorControl) {
    StateLock g;
    g_ds.blend_enable_mask = arg(ctx, 1) & 0xFF;
    g_ds.color_write = arg(ctx, 3) != 0;
}
// void GX2SetBlendControl(target, colorSrc, colorDst, colorCombine, BOOL useAlphaBlend, alphaSrc, alphaDst, alphaCombine)
GX2_FUNC(GX2SetBlendControl) {
    StateLock g;
    if (arg(ctx, 0) >= 8) return;
    BlendTarget& b = g_ds.blend[arg(ctx, 0)];
    b = {arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 5), arg(ctx, 6), arg(ctx, 7), arg(ctx, 4) != 0};
}
// void GX2SetTargetChannelMasks(t0 .. t7): GX2ChannelMask bits R 1, G 2, B 4, A 8 (= VkColorComponentFlags)
GX2_FUNC(GX2SetTargetChannelMasks) {
    StateLock g;
    for (int k = 0; k < 8; ++k) g_ds.write_mask[k] = arg(ctx, k) & 0xF;
}
GX2_FUNC(GX2SetBlendConstantColor) {
    StateLock g;
    for (int k = 0; k < 4; ++k) g_ds.blend_constant[k] = static_cast<float>(ctx.f[1 + k].ps0);
}
// void GX2SetDepthOnlyControl(BOOL depthTest, BOOL depthWrite, GX2CompareFunction)
GX2_FUNC(GX2SetDepthOnlyControl) {
    StateLock g;
    g_ds.depth_test = arg(ctx, 0) != 0;
    g_ds.depth_write = arg(ctx, 1) != 0;
    g_ds.depth_func = arg(ctx, 2);
}
// Arguments past the eighth are in the caller's parameter save area (r1 + 8, one word each).
uint32_t stack_arg(const ppc::Context& ctx, const uint8_t* base, int i) { return ppc::ld32(base, ctx.r[1] + 8 + 4 * (i - 8)); }
// void GX2SetDepthStencilControl(depthTest, depthWrite, depthFunc, stencilTest, backStencilTest, frontFunc,
//     frontZPass, frontZFail, frontFail, backFunc, backZPass, backZFail, backFail): args 8-12 on the stack
GX2_FUNC(GX2SetDepthStencilControl) {
    StateLock g;
    g_ds.depth_test = arg(ctx, 0) != 0;
    g_ds.depth_write = arg(ctx, 1) != 0;
    g_ds.depth_func = arg(ctx, 2);
    g_ds.stencil_test = arg(ctx, 3) != 0;
    g_ds.back_stencil = arg(ctx, 4) != 0;
    StencilFace& f = g_ds.stencil_front;
    StencilFace& b = g_ds.stencil_back;
    f.func = arg(ctx, 5) & 7;
    f.pass = arg(ctx, 6) & 7;
    f.depth_fail = arg(ctx, 7) & 7;
    f.fail = stack_arg(ctx, base, 8) & 7;
    b.func = stack_arg(ctx, base, 9) & 7;
    b.pass = stack_arg(ctx, base, 10) & 7;
    b.depth_fail = stack_arg(ctx, base, 11) & 7;
    b.fail = stack_arg(ctx, base, 12) & 7;
}
// void GX2SetStencilMask(u8 compareMaskFront, u8 writeMaskFront, u8 refFront, u8 compareMaskBack, u8 writeMaskBack, u8 refBack)
GX2_FUNC(GX2SetStencilMask) {
    StateLock g;
    StencilFace* faces[2] = {&g_ds.stencil_front, &g_ds.stencil_back};
    for (int k = 0; k < 2; ++k) {
        faces[k]->compare_mask = arg(ctx, 3 * k) & 0xFF;
        faces[k]->write_mask = arg(ctx, 3 * k + 1) & 0xFF;
        faces[k]->ref = arg(ctx, 3 * k + 2) & 0xFF;
    }
}
// void GX2SetCullOnlyControl(GX2FrontFace, BOOL cullFront, BOOL cullBack); GX2SetPolygonControl starts the same
GX2_FUNC(GX2SetCullOnlyControl) {
    StateLock g;
    g_ds.front_face = arg(ctx, 0);
    g_ds.cull_front = arg(ctx, 1) != 0;
    g_ds.cull_back = arg(ctx, 2) != 0;
}
GX2_FUNC(GX2SetPolygonControl) { hle_gx2_GX2SetCullOnlyControl(ctx, base); }
// void GX2SetAlphaTest(BOOL enable, GX2CompareFunction, float ref)
GX2_FUNC(GX2SetAlphaTest) {
    StateLock g;
    g_ds.alpha_test = arg(ctx, 0) != 0;
    g_ds.alpha_func = arg(ctx, 1) & 7;
    g_ds.alpha_ref = static_cast<float>(ctx.f[1].ps0);
}
// void GX2SetPointSize(float width, float height): GX2 sizes are in pixels (diameter)
GX2_FUNC(GX2SetPointSize) {
    StateLock g;
    g_ds.point_size = static_cast<float>(ctx.f[1].ps0);
}
// void GX2SetRasterizerClipControlEx(BOOL rasteriser, BOOL zClip, BOOL halfZ)
GX2_FUNC(GX2SetRasterizerClipControlEx) {
    StateLock g;
    g_ds.half_z = arg(ctx, 2) != 0;
}

void capture_draw(uint32_t mode, uint32_t count, uint32_t first, uint32_t instances, bool indexed, uint32_t index_type,
                  uint32_t index_addr = 0, uint32_t base_vertex = 0) {
    uint8_t* base = os::gbase();
    StateLock g;
    DrawInfo d;
    d.mode = mode;
    d.count = count;
    d.first = first;
    d.instances = instances;
    d.indexed = indexed;
    d.index_type = index_type;
    d.vs = g_vs;
    d.ps = g_ps;
    d.fs = g_fs;
    // program pointer / size: GX2VertexShader +0xD0/+0xD4, GX2PixelShader +0xA4/+0xA8 (public header layout)
    if (g_vs) { d.vs_size = ppc::ld32(base, g_vs + 0xD0); d.vs_program = ppc::ld32(base, g_vs + 0xD4); }
    if (g_ps) { d.ps_size = ppc::ld32(base, g_ps + 0xA4); d.ps_program = ppc::ld32(base, g_ps + 0xA8); }
    d.color0 = g_color[0];
    d.depth = g_depth_buffer;
    d.tex0 = g_tex[0];
    for (uint32_t t : g_tex) d.textures += t != 0;
    d.attribs = g_attribs;
    std::memcpy(d.viewport, g_viewport, sizeof d.viewport);
    g_frame_events.push_back("draw " + std::to_string(g_frame_draws.size()));
    g_frame_draws.push_back(d);
    ++g_total_draws;
    if (g_renderer) {
        DrawCall call{mode, count, first, std::max(1u, instances), indexed, index_type, index_addr, base_vertex};
        g_ds.gs = g_shader_mode == 2 ? g_gs : 0;
        g_renderer->draw(g_ds, call);
    }
}
// void GX2DrawEx(GX2PrimitiveMode, u32 count, u32 firstVertex, u32 numInstances)
GX2_FUNC(GX2DrawEx) { capture_draw(arg(ctx, 0), arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), false, 0); }
// void GX2DrawIndexedEx(GX2PrimitiveMode, u32 count, GX2IndexType, const void* indices, u32 baseVertex, u32 numInstances)
GX2_FUNC(GX2DrawIndexedEx) {
    capture_draw(arg(ctx, 0), arg(ctx, 1), arg(ctx, 4), arg(ctx, 5), true, arg(ctx, 2), arg(ctx, 3), arg(ctx, 4));
}

// void GX2ClearColor(GX2ColorBuffer*, float r, float g, float b, float a): fill level 0 of the target surface
GX2_FUNC(GX2ClearColor) {
    ScopedNs timer{g_ns_clear};
    const Surf s = read_surface(base, arg(ctx, 0));
    if (!s.image || !s.image_size) return;
    const float c[4] = {static_cast<float>(ctx.f[1].ps0), static_cast<float>(ctx.f[2].ps0),
                        static_cast<float>(ctx.f[3].ps0), static_cast<float>(ctx.f[4].ps0)};
    {
        char e[160];
        std::snprintf(e, sizeof e, "clear color %08X (image %08X) %ux%u fmt %03X tile %u -> (%.3f %.3f %.3f %.3f)", s.addr,
                      s.image, s.w, s.h, s.format, s.tile, c[0], c[1], c[2], c[3]);
        StateLock g;
        g_frame_events.emplace_back(e);
    }
    ++g_total_clears;
    if (g_renderer && g_renderer->clear_color(guest_surface(base, arg(ctx, 0)), c)) return;
    uint8_t px[16];
    const uint32_t n = encode_color(s.format, c, px);
    uint8_t* dst = base + s.image;
    if (n == 4) {
        uint32_t v;
        std::memcpy(&v, px, 4);
        std::fill_n(reinterpret_cast<uint32_t*>(dst), s.image_size / 4, v);  // images are 256-byte aligned
    } else {
        for (uint32_t o = 0; o + n <= s.image_size; o += n) std::memcpy(dst + o, px, n);
    }
}
// void GX2ClearDepthStencilEx(GX2DepthBuffer*, float depth, u8 stencil, GX2ClearFlags /* 1 depth, 2 stencil */)
GX2_FUNC(GX2ClearDepthStencilEx) {
    ScopedNs timer{g_ns_clear};
    const Surf s = read_surface(base, arg(ctx, 0));
    if (!s.image || !s.image_size) return;
    const float depth = static_cast<float>(ctx.f[1].ps0);
    const uint32_t stencil = arg(ctx, 1) & 0xFF, flags = arg(ctx, 2);
    if (g_renderer && g_renderer->clear_depth_stencil(guest_surface(base, arg(ctx, 0)), depth, stencil, flags)) return;
    uint8_t* dst = base + s.image;
    switch (s.format & 0xFFF) {
        case 0x011: case 0x811: {  // D24S8 (little-endian u32): depth bits 0-23, stencil 24-31
            uint32_t* p = reinterpret_cast<uint32_t*>(dst);
            const size_t n = s.image_size / 4;
            const uint32_t d = unorm(depth, 24), st = stencil << 24;
            if ((flags & 3) == 3) std::fill_n(p, n, d | st);
            else if (flags & 1) for (size_t i = 0; i < n; ++i) p[i] = (p[i] & 0xFF000000u) | d;
            else if (flags & 2) for (size_t i = 0; i < n; ++i) p[i] = (p[i] & 0x00FFFFFFu) | st;
            break;
        }
        case 0x806: case 0x80E:  // D32F
            if (flags & 1) {
                uint32_t v;
                std::memcpy(&v, &depth, 4);
                std::fill_n(reinterpret_cast<uint32_t*>(dst), s.image_size / 4, v);
            }
            break;
        case 0x005:  // D16
            if (flags & 1)
                for (uint32_t o = 0; o + 2 <= s.image_size; o += 2) {
                    const uint32_t v = unorm(depth, 16);
                    dst[o] = static_cast<uint8_t>(v);
                    dst[o + 1] = static_cast<uint8_t>(v >> 8);
                }
            break;
        default:
            break;
    }
}
// void GX2CopySurface(const GX2Surface* src, u32 srcMip, u32 srcSlice, GX2Surface* dst, u32 dstMip, u32 dstSlice)
GX2_FUNC(GX2CopySurface) {
    ScopedNs timer{g_ns_copy};
    const Surf src = read_surface(base, arg(ctx, 0)), dst = read_surface(base, arg(ctx, 3));
    if (arg(ctx, 1) != 0 || arg(ctx, 4) != 0 || !src.image || !dst.image) return;  // mip levels > 0: not yet
    const SurfaceLevel a = level0(src, arg(ctx, 2)), b = level0(dst, arg(ctx, 5));
    ++g_total_copies;
    {
        char e[160];
        std::snprintf(e, sizeof e, "copy %08X %ux%u fmt %03X slice %u -> %08X %ux%u fmt %03X slice %u", src.image, src.w, src.h,
                      src.format, arg(ctx, 2), dst.image, dst.w, dst.h, dst.format, arg(ctx, 5));
        StateLock g;
        g_frame_events.emplace_back(e);
    }
    if (g_renderer && g_renderer->copy_surface(guest_surface(base, arg(ctx, 0)), arg(ctx, 2), guest_surface(base, arg(ctx, 3)), arg(ctx, 5)))
        return;
    if (a.width != b.width || a.height != b.height || a.bpp != b.bpp) return;
    if (a.tile_mode == b.tile_mode && a.pitch == b.pitch && a.swizzle == b.swizzle && a.slice == 0 && b.slice == 0 &&
        src.mips <= 1 && dst.mips <= 1 && src.image_size == dst.image_size) {
        std::memmove(base + dst.image, base + src.image, src.image_size);  // same layout: raw copy
        return;
    }
    std::vector<uint8_t> tmp(size_t{a.width} * a.height * (a.bpp / 8));
    detile(base + src.image, tmp.data(), a);
    retile(tmp.data(), base + dst.image, b);
}
// void GX2CopyColorBufferToScanBuffer(const GX2ColorBuffer*, GX2ScanTarget /* 1 TV, 4 DRC */)
GX2_FUNC(GX2CopyColorBufferToScanBuffer) {
    ScopedNs timer{g_ns_scan};
    const Surf s = read_surface(base, arg(ctx, 0));
    if (!s.image || !s.w || !s.h) return;
    if (arg(ctx, 1) == 4) {  // GamePad: GPU only (shown inset or instead of the TV, cycle_gamepad_view)
        if (g_renderer && g_renderer->scan_out(guest_surface(base, arg(ctx, 0)), true)) {
            StateLock g;
            char e[160];
            std::snprintf(e, sizeof e, "scan-out GamePad %08X (image %08X) %ux%u fmt %03X (GPU)", s.addr, s.image, s.w, s.h, s.format);
            g_frame_events.emplace_back(e);
        }
        return;
    }
    if (arg(ctx, 1) != 1) return;
    if (g_renderer && g_renderer->scan_out(guest_surface(base, arg(ctx, 0)))) {
        StateLock g;
        char e[160];
        std::snprintf(e, sizeof e, "scan-out %08X (image %08X) %ux%u fmt %03X (GPU)", s.addr, s.image, s.w, s.h, s.format);
        g_frame_events.emplace_back(e);
        return;
    }
    const SurfaceLevel l = level0(s);
    std::vector<uint8_t> lin(size_t{l.width} * l.height * (l.bpp / 8));
    detile(base + s.image, lin.data(), l);
    StateLock g;
    char e[160];
    std::snprintf(e, sizeof e, "scan-out %08X %ux%u fmt %03X tile %u pitch %u swizzle %X", s.addr, s.w, s.h, s.format,
                  s.tile, s.pitch, s.swizzle);
    g_frame_events.emplace_back(e);
    g_tv_image.resize(size_t{l.width} * l.height * 4);
    to_rgba8(s.format, lin.data(), g_tv_image.data(), size_t{l.width} * l.height);
    g_tv_w = l.width;
    g_tv_h = l.height;
}

void write_bmp(const std::string& path, const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    std::FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return;
    const uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
    uint8_t hdr[54] = {'B', 'M'};
    auto le = [&](int off, uint32_t v) { for (int k = 0; k < 4; ++k) hdr[off + k] = static_cast<uint8_t>(v >> (8 * k)); };
    le(2, size);
    le(10, 54);
    le(14, 40);
    le(18, w);
    le(22, h);
    hdr[26] = 1;
    hdr[28] = 24;
    le(34, row * h);
    std::fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row);
    for (uint32_t y = h; y-- > 0;) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t{y} * w + x) * 4];
            line[x * 3] = p[2];
            line[x * 3 + 1] = p[1];
            line[x * 3 + 2] = p[0];
        }
        std::fwrite(line.data(), 1, row, f);
    }
    std::fclose(f);
}

std::string g_trace;  // renderer trace of the dumped frame

void dump_frame(uint32_t frame) {
    const std::string stem = "build/frame_" + std::to_string(frame);
    if (!g_tv_image.empty()) write_bmp(stem + ".bmp", g_tv_image, g_tv_w, g_tv_h);
    std::FILE* f = nullptr;
    if (fopen_s(&f, (stem + "_draws.txt").c_str(), "w") != 0 || !f) {  // relative to the working directory: run from the repo root
        std::fprintf(stderr, "frame dump: cannot write %s_draws.txt\n", stem.c_str());
        return;
    }
    std::fprintf(f, "frame %u: %zu draws, TV image %ux%u\n", frame, g_last_draws.size(), g_tv_w, g_tv_h);
    for (const std::string& e : g_last_events) std::fprintf(f, "%s\n", e.c_str());
    std::fputs(g_trace.c_str(), f);
    std::fprintf(f, "#  mode count first inst idx  vs        vs_prog/size        ps        ps_prog/size        "
                    "fetch     color0    depth     tex0      ntex attribs   viewport\n");
    int n = 0;
    for (const DrawInfo& d : g_last_draws)
        std::fprintf(f, "%-3d %4u %5u %5u %4u %3u  %08X  %08X/%-6X  %08X  %08X/%-6X  %08X  %08X  %08X  %08X  %4u %08X  %.0f,%.0f %.0fx%.0f\n",
                     n++, d.mode, d.count, d.first, d.instances, d.indexed ? d.index_type : 0, d.vs, d.vs_program,
                     d.vs_size, d.ps, d.ps_program, d.ps_size, d.fs, d.color0, d.depth, d.tex0, d.textures, d.attribs,
                     d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3]);
    std::fclose(f);
    std::printf("dumped %s.bmp and %s_draws.txt\n", stem.c_str(), stem.c_str());
}

// void GX2SwapScanBuffers(): flip on the next vsync and present the TV scan buffer
GX2_FUNC(GX2SwapScanBuffers) {
    // the flip happens on a vsync: swap interval N = every Nth 60 Hz vsync (the game paces itself on this). The frame
    // is submitted and presented first and the pacing sleep comes last, so the GPU renders while this thread sleeps.
    struct PaceAtExit {
        ~PaceAtExit() {
            using namespace std::chrono;
            static auto next_flip = steady_clock::now();
            if (g_target_fps < 0) return;  // unlimited
            next_flip += g_target_fps > 0 ? duration_cast<steady_clock::duration>(duration<double>(1.0 / g_target_fps))
                                          : duration_cast<steady_clock::duration>(microseconds(16667) * std::max(1u, g_swap_interval));
            const auto now = steady_clock::now();
            if (next_flip < now) next_flip = now;
            std::this_thread::sleep_until(next_flip);
        }
    } pace;
    ++g_swap_count;
    ++g_timestamp;
    const uint32_t n = ++rt().frames;
    {
        StateLock g;
        g_last_draws.swap(g_frame_draws);
        g_frame_draws.clear();
        g_last_events.swap(g_frame_events);
        g_frame_events.clear();
        Renderer::Frame f;
        if (g_renderer) {
            g_renderer->set_gamepad_view(g_gamepad_view);
            f = g_renderer->end_frame();
        }
        if (g_renderer && n % 1800 == 0) g_renderer->save_caches();  // new pipelines survive a crash or kill
        if (rt().presenter) {
            if (f.tv_image) rt().presenter->present_vk_image(static_cast<VkImage>(f.tv_image), f.width, f.height);
            else if (!g_tv_image.empty()) rt().presenter->present_image(g_tv_image.data(), g_tv_w, g_tv_h);
            else rt().presenter->present_clear(0.08f, 0.10f, 0.14f);
        }
        if (g_dump_frames.count(n)) {
            if (f.tv_image) g_renderer->read_tv(g_tv_image, g_tv_w, g_tv_h);
            if (g_renderer) g_trace = g_renderer->take_trace();
            dump_frame(n);
            // MH3U_DUMP_TARGET=<hex image address>: also write that render target
            char* t = nullptr;
            size_t tl = 0;
            if (g_renderer && _dupenv_s(&t, &tl, "MH3U_DUMP_TARGET") == 0 && t) {
                std::vector<uint8_t> px;
                uint32_t w = 0, h = 0;
                const uint32_t image = static_cast<uint32_t>(std::strtoul(t, nullptr, 16));
                if (g_renderer->read_target(image, px, w, h)) {
                    char path[64];
                    std::snprintf(path, sizeof path, "build/target_%08X.bmp", image);
                    write_bmp(path, px, w, h);
                    std::printf("dumped %s\n", path);
                }
            }
            std::free(t);
        }
        if (g_renderer) g_renderer->set_trace(g_dump_frames.count(n + 1) != 0);
        if (g_renderer && g_watch_targets)
            for (const auto& [addr, size] : g_renderer->target_ranges()) rt().mem->watch(addr, size, addr);
    }
    if (rt().exit_after_frames && n >= rt().exit_after_frames) rt().exit_requested = true;
}

// ---- display lists
struct Cmd {
    ppc::GuestFunction fn;
    uint32_t r[8];
    double f[8];
    uint32_t stack[8];  // arguments 9+ (caller's parameter save area)
};
struct Recording {
    uint32_t buf = 0, size = 0;
    std::vector<Cmd> cmds;
};
thread_local std::unique_ptr<Recording> t_rec;
std::mutex g_dl_m;
std::map<uint32_t, std::shared_ptr<const std::vector<Cmd>>> g_lists;  // by display-list buffer address
uint64_t g_replayed = 0;
// Command vectors are recycled: a frame's lists hold thousands of commands (hundreds of KB), and allocating and
// freeing them every frame went to the OS each time (VirtualAlloc/VirtualFree + zeroed pages).
std::vector<std::vector<Cmd>> g_spare_cmds;  // guarded by g_dl_m

std::shared_ptr<const std::vector<Cmd>> share_cmds(std::vector<Cmd>&& v) {
    // the deleter takes g_dl_m: the last reference must never be dropped while it is held
    return {new std::vector<Cmd>(std::move(v)), [](const std::vector<Cmd>* p) {
                std::vector<Cmd> cmds = std::move(*const_cast<std::vector<Cmd>*>(p));
                delete p;
                cmds.clear();
                std::lock_guard g(g_dl_m);
                if (g_spare_cmds.size() < 16) g_spare_cmds.push_back(std::move(cmds));
            }};
}

template <ppc::GuestFunction F>
void recordable(ppc::Context& ctx, uint8_t* base) {
    if (t_rec) {
        Cmd c{F, {}, {}, {}};
        for (int k = 0; k < 8; ++k) {
            c.r[k] = ctx.r[3 + k];
            c.f[k] = ctx.f[1 + k].ps0;
            c.stack[k] = ppc::ld32(base, ctx.r[1] + 8 + 4 * k);
        }
        t_rec->cmds.push_back(c);
        return;
    }
    F(ctx, base);
}

// void GX2BeginDisplayListEx(void* buffer, u32 size, BOOL profiling)
GX2_FUNC(GX2BeginDisplayListEx) {
    t_rec = std::make_unique<Recording>();
    t_rec->buf = arg(ctx, 0);
    t_rec->size = arg(ctx, 1);
    std::lock_guard g(g_dl_m);
    if (!g_spare_cmds.empty()) {
        t_rec->cmds = std::move(g_spare_cmds.back());
        g_spare_cmds.pop_back();
    }
}
// u32 GX2EndDisplayList(void* buffer) -> bytes used (a nominal 16 bytes per command, capped to the buffer)
GX2_FUNC(GX2EndDisplayList) {
    if (!t_rec) { ret(ctx, 0); return; }
    const uint32_t used = std::min(t_rec->size, std::max<uint32_t>(0x20, static_cast<uint32_t>(t_rec->cmds.size()) * 16));
    std::shared_ptr<const std::vector<Cmd>> list = share_cmds(std::move(t_rec->cmds));
    {
        std::lock_guard g(g_dl_m);
        g_lists[t_rec->buf].swap(list);  // `list` now holds the replaced one: released below, outside the lock
    }
    list.reset();
    t_rec.reset();
    ret(ctx, used);
}
// void GX2DirectCallDisplayList(const void* buffer, u32 size): replay on the calling thread
GX2_FUNC(GX2DirectCallDisplayList) {
    std::shared_ptr<const std::vector<Cmd>> list;
    {
        std::lock_guard g(g_dl_m);
        auto it = g_lists.find(arg(ctx, 0));
        if (it == g_lists.end()) return;
        list = it->second;
    }
    // replayed calls see their stack arguments in a parameter area below the caller's (unused) stack space
    const uint32_t sp = (ctx.r[1] - 0x100) & ~0xFu;
    StateLock state;  // once for the whole list
    for (const Cmd& c : *list) {
        ppc::Context cc{};
        cc.r[1] = sp;
        for (int k = 0; k < 8; ++k) {
            cc.r[3 + k] = c.r[k];
            cc.f[1 + k].ps0 = c.f[k];
            ppc::st32(base, sp + 8 + 4 * k, c.stack[k]);
        }
        c.fn(cc, base);
        ++g_replayed;
    }
}

// Remaining command-emitting calls: recordable, no effect yet.
#define GX2_STATE_ONLY(X)                                                                                         \
    X(GX2Invalidate) X(GX2SetAlphaToMask) X(GX2SetGeometryShaderInputRingBuffer)                                    \
    X(GX2SetGeometryShaderOutputRingBuffer) X(GX2SetLineWidth) X(GX2SetPolygonOffset) X(GX2SetPrimitiveRestartIndex)
#define X(name) GX2_FUNC(name) {}
GX2_STATE_ONLY(X)
#undef X

}  // namespace

void cycle_gamepad_view() {
    const int v = (g_gamepad_view + 1) % 3;
    g_gamepad_view = v;
    static const char* names[] = {"hidden", "picture-in-picture", "GamePad only"};
    std::printf("GamePad screen: %s\n", names[v]);
    // applied by GX2SwapScanBuffers on the guest thread (the renderer is not thread-safe)
}

void save_gx2_caches() {
    StateLock g;
    if (g_renderer) g_renderer->save_caches();
}

void print_gx2_stats() {
    if (g_renderer) std::printf("%s\n", g_renderer->stats().c_str());
    std::printf("gx2: %llu draws, %llu clears, %llu surface "
                "copies, %llu display-list commands replayed; last frame %zu draws, TV image %ux%u\n",
                static_cast<unsigned long long>(g_total_draws), static_cast<unsigned long long>(g_total_clears),
                static_cast<unsigned long long>(g_total_copies), static_cast<unsigned long long>(g_replayed),
                g_last_draws.size(), g_tv_w, g_tv_h);
    std::printf("gx2: CPU surface work %.0f ms clears, %.0f ms copies, %.0f ms scan-out\n", g_ns_clear / 1e6,
                g_ns_copy / 1e6, g_ns_scan / 1e6);
}

// Builds every pipeline earlier runs used, so none is compiled mid-game. Called on the guest thread before the game
// starts; the launcher shows the progress as a toast.
void precompile_pipelines(const std::function<void(size_t, size_t)>& progress) {
    if (!g_renderer) return;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t built = g_renderer->precompile(progress);
    if (built)
        std::printf("pipelines: %zu built from earlier runs in %.1f s\n", built,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
}

void set_effects(bool tri_look, bool ambient_occlusion, bool depth_of_field, bool vignette, bool global_illumination) {
    if (g_renderer) g_renderer->set_effects(tri_look, ambient_occlusion, depth_of_field, vignette, global_illumination);
}

void set_texture_packs(bool on) {
    if (g_renderer) g_renderer->set_texture_packs(on);
}

void set_texture_dump(bool on) {
    if (g_renderer) g_renderer->set_texture_dump(on);
}

uint64_t texture_dumps() { return g_renderer ? g_renderer->texture_dumps() : 0; }

void dump_next_frame() {
    StateLock g;
    g_dump_frames.insert(rt().frames + 2);  // + 2: the renderer's trace is switched on a frame ahead
}

void register_gx2(os::Runtime& r) {
    if (r.presenter && r.presenter->vk().device) {
        std::string err;
        g_renderer = Renderer::create(r.presenter->vk(), r.mem->base(), r.shader_dir, &err);
        if (!g_renderer) std::fprintf(stderr, "renderer unavailable (%s): CPU surface paths only\n", err.c_str());
    }
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_DUMP_FRAME") == 0 && env)
        for (char* p = env; *p;) {
            char* end = nullptr;
            const unsigned long f = std::strtoul(p, &end, 10);
            if (end == p) break;
            g_dump_frames.insert(static_cast<uint32_t>(f));
            p = *end ? end + 1 : end;
        }
    std::free(env);
    if (_dupenv_s(&env, &len, "MH3U_WATCH_TARGETS") == 0 && env) g_watch_targets = true;
    std::free(env);
    // MH3U_FPS: "game" (the swap interval: 30 FPS), "max" or 0 (unlimited), or a number. Default: the display's refresh
    // rate in a window; headless keeps the game's pacing so frame-numbered input scripts stay in step with game time.
    env = nullptr;
    const bool windowed = r.presenter && r.presenter->window_handle();
    g_target_fps = windowed ? r.presenter->refresh_rate() : 0;
    if (_dupenv_s(&env, &len, "MH3U_FPS") == 0 && env) {
        const std::string v = env;
        g_target_fps = v == "game" ? 0 : v == "max" ? -1 : std::strtod(env, nullptr) > 0 ? std::strtod(env, nullptr) : -1;
    }
    std::free(env);
    if (g_target_fps > 0) std::printf("frame pacing: %.0f FPS (MH3U_FPS)\n", g_target_fps);
    else std::printf("frame pacing: %s\n", g_target_fps < 0 ? "unlimited" : "game (swap interval)");
    env = nullptr;
    if (_dupenv_s(&env, &len, "MH3U_GAMEPAD_VIEW") == 0 && env) {
        const std::string v = env;
        g_gamepad_view = v == "pip" ? 1 : v == "only" ? 2 : 0;
    }
    std::free(env);
#define R(name) ppc::register_import("gx2." #name, &hle_gx2_##name);
#define RR(name) ppc::register_import("gx2." #name, &recordable<&hle_gx2_##name>);
    R(GX2Init) R(GX2Shutdown) R(GX2TempGetGPUVersion) R(GX2GetSystemTVAspectRatio) R(GX2CalcTVSize) R(GX2CalcDRCSize)
    R(GX2SetTVBuffer) R(GX2SetDRCBuffer) R(GX2SetTVScale) R(GX2SetDRCScale) R(GX2SetTVEnable) R(GX2SetDRCEnable)
    R(GX2SetTVGamma) R(GX2SetDRCGamma) R(GX2SetSwapInterval) R(GX2GetSwapInterval) R(GX2GetGPUTimeout)
    R(GX2PrintGPUStatus) R(GX2SetupContextStateEx) R(GX2CalcSurfaceSizeAndAlignment)
    R(GX2GetSurfaceFormatBits) R(GX2GetSurfaceMipPitch) R(GX2CalcDepthBufferHiZInfo) R(GX2InitColorBufferRegs)
    R(GX2InitDepthBufferRegs) R(GX2InitDepthBufferHiZEnable) R(GX2InitTextureRegs)
    R(GX2ConvertDepthBufferToTextureSurface) R(GX2RCreateSurface) R(GX2RCreateSurfaceUserMemory)
    R(GX2RDestroySurfaceEx) R(GX2RLockSurfaceEx) R(GX2RUnlockSurfaceEx) R(GX2RInvalidateSurface)
    R(GX2AllocateTilingApertureEx) R(GX2FreeTilingAperture) R(GX2CalcFetchShaderSizeEx) R(GX2InitFetchShaderEx)
    R(GX2CalcGeometryShaderInputRingBufferSize) R(GX2CalcGeometryShaderOutputRingBufferSize) R(GX2InitSampler)
    R(GX2InitSamplerBorderType) R(GX2InitSamplerClamping) R(GX2InitSamplerDepthCompare) R(GX2InitSamplerFilterAdjust)
    R(GX2InitSamplerLOD) R(GX2InitSamplerXYFilter) R(GX2InitSamplerZMFilter) R(GX2GetLastSubmittedTimeStamp)
    R(GX2WaitTimeStamp) R(GX2SampleBottomGPUCycle) R(GX2GPUTimeToCPUTime) R(GX2Flush) R(GX2DrawDone)
    R(GX2WaitForVsync) R(GX2GetSwapStatus) R(GX2SwapScanBuffers) R(GX2BeginDisplayListEx) R(GX2EndDisplayList)
    // command-emitting: recorded into display lists when one is open on the calling thread
    RR(GX2SetContextState) RR(GX2DirectCallDisplayList) RR(GX2SetColorBuffer) RR(GX2SetDepthBuffer)
    RR(GX2SetVertexShader) RR(GX2SetPixelShader) RR(GX2SetFetchShader) RR(GX2SetPixelTexture) RR(GX2SetAttribBuffer)
    RR(GX2SetViewport) RR(GX2DrawEx) RR(GX2DrawIndexedEx) RR(GX2ClearColor) RR(GX2ClearDepthStencilEx)
    RR(GX2CopySurface) RR(GX2CopyColorBufferToScanBuffer) RR(GX2SetGeometryShader) RR(GX2SetShaderModeEx)
    RR(GX2SetPixelSampler) RR(GX2SetVertexUniformBlock) RR(GX2SetPixelUniformBlock) RR(GX2SetGeometryUniformBlock)
    RR(GX2SetScissor) RR(GX2SetColorControl) RR(GX2SetBlendControl) RR(GX2SetTargetChannelMasks)
    RR(GX2SetBlendConstantColor) RR(GX2SetDepthOnlyControl) RR(GX2SetDepthStencilControl) RR(GX2SetCullOnlyControl)
    RR(GX2SetPolygonControl) RR(GX2SetAlphaTest) RR(GX2SetPointSize) RR(GX2SetRasterizerClipControlEx)
    RR(GX2SetStencilMask)
#define X(name) RR(name)
    GX2_STATE_ONLY(X)
#undef X
#undef RR
#undef R
}

}  // namespace mh3u::gfx
