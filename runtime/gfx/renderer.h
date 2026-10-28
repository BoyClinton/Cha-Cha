// Vulkan backend for GX2 (rendering, stage 2): executes draws with the translated shaders
// (tools/shaderc-latte/translate.h, <cache>/shaders/<key>.<stage>.spv) and keeps render targets in device memory.
//
// - Render targets live in a surface cache keyed by guest image address. Clears, GX2CopySurface and TV scan-out of
//   cached surfaces run on the GPU; guest memory is not written back (nothing on the CPU reads render targets).
// - Textures are detiled (level 0) and uploaded, re-uploaded when their contents' hash changes; a texture whose image
//   is a cached render target samples that image.
// - Vertex data is converted on the CPU from the fetch shader's attribute streams into the GPR values the shader
//   expects (uvec4 per attribute), uniform blocks (already little-endian in guest memory) are copied into a per-frame upload buffer.
// - GX2 state is collected by gx2.cpp into DrawState; the renderer turns each draw into a cached pipeline + draw.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mh3u::gfx {

struct VulkanContext;

// GX2Surface fields (see gx2.cpp for offsets).
struct GuestSurface {
    uint32_t dim = 1, width = 0, height = 0, depth = 1, mips = 1, format = 0, aa = 0, use = 0, image_size = 0, image = 0;
    uint32_t mip_size = 0, mipmaps = 0, tile = 0, swizzle = 0, pitch = 0;
    uint32_t mip_offset[13] = {};
};

struct TextureBinding {
    GuestSurface surf;
    uint32_t comp_sel = 0x00010203;  // GX2_COMP_SEL: one byte per output channel (0-3 = x..w, 4 = 0, 5 = 1)
    bool bound = false;
};
// Our own encoding of GX2Sampler (gx2.cpp GX2InitSampler*): the guest struct is only read by our HLE.
struct SamplerState {
    uint32_t words[3] = {};
    bool bound = false;
};
struct UniformBinding {
    uint32_t addr = 0, size = 0;
};
struct AttribBuffer {
    uint32_t addr = 0, size = 0, stride = 0;
};
// GX2AttribStream (0x20 bytes)
struct AttribStream {
    uint32_t location = 0, buffer = 0, offset = 0, format = 0, type = 0, divisor = 0, mask = 0, endian = 0;
};
// One face of GX2SetDepthStencilControl + GX2SetStencilMask (enums equal VkCompareOp / VkStencilOp).
struct StencilFace {
    uint32_t func = 7, pass = 0, depth_fail = 0, fail = 0;  // ALWAYS, KEEP x3
    uint32_t compare_mask = 0xFF, write_mask = 0xFF, ref = 0;
};
struct BlendTarget {
    uint32_t color_src = 1, color_dst = 0, color_op = 0, alpha_src = 1, alpha_dst = 0, alpha_op = 0;
    bool separate_alpha = false;
};

constexpr int kStageVertex = 0, kStagePixel = 1, kStageGeometry = 2;

struct StageBindings {
    TextureBinding tex[18];
    SamplerState samp[18];
    UniformBinding ubo[16];
};

struct DrawState {
    uint32_t vs = 0, ps = 0, gs = 0;  // guest GX2VertexShader / GX2PixelShader / GX2GeometryShader
    std::vector<AttribStream> streams;
    AttribBuffer attribs[16];
    StageBindings stage[3];
    GuestSurface color[8];
    bool color_bound[8] = {};
    GuestSurface depth;
    bool depth_bound = false;
    float viewport[6] = {0, 0, 1, 1, 0, 1};  // x, y, w, h, near, far
    bool scissor_set = false;
    uint32_t scissor[4] = {};
    uint32_t blend_enable_mask = 0;
    BlendTarget blend[8];
    uint32_t write_mask[8] = {0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF, 0xF};
    bool color_write = true;
    float blend_constant[4] = {};
    bool depth_test = false, depth_write = false;
    uint32_t depth_func = 1;  // GX2CompareFunction (= VkCompareOp)
    bool stencil_test = false, back_stencil = false;  // back = separate state for back faces
    StencilFace stencil_front, stencil_back;
    uint32_t front_face = 0;  // 0 CCW, 1 CW
    bool cull_front = false, cull_back = false;
    bool alpha_test = false;
    uint32_t alpha_func = 7;
    float alpha_ref = 0.0f;
    float point_size = 1.0f;
    bool half_z = false;  // GX2SetRasterizerClipControlEx: clip-space z in 0..1 instead of -1..1
};

struct DrawCall {
    uint32_t mode = 0, count = 0, first = 0, instances = 1;
    bool indexed = false;
    uint32_t index_type = 0, index_addr = 0, base_vertex = 0;
};

class Renderer {
public:
    // Null (with a message) if the device lacks something essential.
    static std::unique_ptr<Renderer> create(const VulkanContext& vk, uint8_t* guest_base,
                                            const std::filesystem::path& shader_dir, std::string* error);
    virtual ~Renderer() = default;

    // Each returns false when it cannot handle the surface (format), so the caller falls back to the CPU path.
    virtual bool clear_color(const GuestSurface& s, const float rgba[4]) = 0;
    virtual bool clear_depth_stencil(const GuestSurface& s, float depth, uint32_t stencil, uint32_t flags) = 0;
    virtual bool copy_surface(const GuestSurface& src, uint32_t src_slice, const GuestSurface& dst, uint32_t dst_slice) = 0;
    // Copies a cached render target to the TV (or GamePad) scan-out image; false if the surface is not cached.
    virtual bool scan_out(const GuestSurface& s, bool gamepad = false) = 0;
    // How end_frame shows the GamePad image: 0 not at all (default), 1 inset in the TV image's bottom-right corner,
    // 2 instead of the TV image.
    virtual void set_gamepad_view(int mode) = 0;
    virtual void draw(const DrawState& state, const DrawCall& call) = 0;

    // Submits the frame. If a scan-out reached the GPU TV image this frame, returns it for presenting.
    struct Frame {
        void* tv_image = nullptr;  // VkImage, TRANSFER_SRC_OPTIMAL
        uint32_t width = 0, height = 0;
    };
    virtual Frame end_frame() = 0;
    // Reads the GPU TV image back as RGBA8 (waits for the GPU); false if there is none.
    virtual bool read_tv(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) = 0;
    // Reads a cached render target (by guest image address) back as RGBA8, converted by a blit; for debugging.
    virtual bool read_target(uint32_t image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) = 0;
    virtual std::string stats() const = 0;
    // Guest image ranges (address, bytes) of the cached render targets, for MH3U_WATCH_TARGETS.
    virtual std::vector<std::pair<uint32_t, uint32_t>> target_ranges() const = 0;
    // Per-draw detail (shaders, attributes, textures, uniforms) for frame dumps: enable, then take the text.
    virtual void set_trace(bool on) = 0;
    virtual std::string take_trace() = 0;

    // Pipelines across runs (cache folder = the shader folder's parent): builds every pipeline listed by earlier runs,
    // reporting (done, total) about every 16 ms; returns how many were built. save_caches() writes the driver's
    // pipeline cache if pipelines were added since the last save.
    virtual size_t precompile(const std::function<void(size_t, size_t)>& progress) = 0;
    virtual void save_caches() = 0;
    // Our full-screen effects on/off (any thread; applied from the next frame).
    virtual void set_effects(bool tri_look, bool ambient_occlusion, bool depth_of_field, bool vignette, bool global_illumination) = 0;
    // Texture packs (<data>/texture_replacements, see texture_pack.h) on/off; any thread, from the next draw.
    virtual void set_texture_packs(bool on) = 0;
    // Texture dumping (tex_<w>x<h>_<crc>.dds into <data>/texture_dump) on/off, and how many files it wrote.
    virtual void set_texture_dump(bool on) = 0;
    virtual uint64_t texture_dumps() const = 0;
};

}  // namespace mh3u::gfx
