#include "renderer.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

#include "guest_memory.h"
#include "gx2_surface.h"
#include "presenter.h"
#include "shader_abi.h"
#include "texture_pack.h"
#include "translate.h"
#include "vk_compat.h"

namespace mh3u::gfx {
namespace {

namespace abi = shader_abi;

uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t le32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }

uint64_t fnv(const void* data, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
// Fast content hash for guest data (textures, vertex sources; not cryptographic): four independent 64-bit lanes, so
// the multiplies overlap instead of waiting on each other.
uint64_t hash_words(const uint8_t* p, size_t n) {
    uint64_t h[4] = {0x9E3779B97F4A7C15ull ^ n, 0xC2B2AE3D27D4EB4Full, 0x165667B19E3779F9ull, 0x27D4EB2F165667C5ull};
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w[4];
        std::memcpy(w, p + i, 32);
        for (int k = 0; k < 4; ++k) {
            h[k] = (h[k] ^ w[k]) * 0x100000001B3ull;
            h[k] ^= h[k] >> 29;
        }
    }
    uint64_t r = h[0] ^ (h[1] * 31) ^ (h[2] * 961) ^ (h[3] * 29791);
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, p + i, 8);
        r = (r ^ w) * 0x100000001B3ull;
        r ^= r >> 29;
    }
    for (; i < n; ++i) r = (r ^ p[i]) * 0x100000001B3ull;
    return r;
}

float half_to_float(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    uint32_t x;
    if (e == 0) {
        if (!m) x = sign;
        else {  // denormal
            float f = std::ldexp(static_cast<float>(m), -24);
            std::memcpy(&x, &f, 4);
            x |= sign;
        }
    } else if (e == 31) x = sign | 0x7F800000 | (m << 13);
    else x = sign | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    std::memcpy(&f, &x, 4);
    return f;
}
uint32_t fbits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// ---- formats --------------------------------------------------------------------------------------------------
struct FormatDesc {
    VkFormat vk = VK_FORMAT_UNDEFINED;
    uint32_t bytes = 0;  // per element (4x4 block for BC)
    bool block = false;
    VkComponentMapping swizzle{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY};
};

// GX2SurfaceFormat: low 6 bits element type, 0x100 UINT, 0x200 SNORM, 0x300 SINT, 0x400 SRGB, 0x800 FLOAT.
FormatDesc color_format(uint32_t f) {
    const uint32_t kind = f & 0xF00;
    const bool srgb = kind == 0x400, snorm = kind == 0x200, uint_ = kind == 0x100, sint = kind == 0x300, flt = kind == 0x800;
    FormatDesc d;
    switch (f & 0x3F) {
        case 0x01: d = {uint_ ? VK_FORMAT_R8_UINT : snorm ? VK_FORMAT_R8_SNORM : VK_FORMAT_R8_UNORM, 1}; break;
        case 0x05: d = {uint_ ? VK_FORMAT_R16_UINT : flt ? VK_FORMAT_R16_SFLOAT : VK_FORMAT_R16_UNORM, 2}; break;
        case 0x06: d = {VK_FORMAT_R16_SFLOAT, 2}; break;
        case 0x07: d = {uint_ ? VK_FORMAT_R8G8_UINT : snorm ? VK_FORMAT_R8G8_SNORM : VK_FORMAT_R8G8_UNORM, 2}; break;
        case 0x08: d = {VK_FORMAT_B5G6R5_UNORM_PACK16, 2}; break;  // R in the low bits
        case 0x0A:  // R5G5B5A1, R low: Vulkan's A1R5G5B5 has B low -> swap R/B in the view
            d = {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2};
            d.swizzle = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
            break;
        case 0x0B:  // R4G4B4A4, R low: Vulkan's R4G4B4A4 has R high
            d = {VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2};
            d.swizzle = {VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R};
            break;
        case 0x0D: d = {sint ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT, 4}; break;
        case 0x0E: d = {flt ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R32_UINT, 4}; break;
        case 0x0F: d = {uint_ ? VK_FORMAT_R16G16_UINT : flt ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R16G16_UNORM, 4}; break;
        case 0x10: d = {VK_FORMAT_R16G16_SFLOAT, 4}; break;
        case 0x16: d = {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4}; break;
        case 0x19: d = {uint_ ? VK_FORMAT_A2B10G10R10_UINT_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4}; break;
        case 0x1A:
            d = {srgb ? VK_FORMAT_R8G8B8A8_SRGB : uint_ ? VK_FORMAT_R8G8B8A8_UINT : snorm ? VK_FORMAT_R8G8B8A8_SNORM
                                                                                              : VK_FORMAT_R8G8B8A8_UNORM, 4};
            break;
        case 0x1D: d = {VK_FORMAT_R32G32_UINT, 8}; break;
        case 0x1E: d = {VK_FORMAT_R32G32_SFLOAT, 8}; break;
        case 0x1F: d = {flt ? VK_FORMAT_R16G16B16A16_SFLOAT : uint_ ? VK_FORMAT_R16G16B16A16_UINT : VK_FORMAT_R16G16B16A16_UNORM, 8}; break;
        case 0x20: d = {VK_FORMAT_R16G16B16A16_SFLOAT, 8}; break;
        case 0x22: d = {flt ? VK_FORMAT_R32G32B32A32_SFLOAT : VK_FORMAT_R32G32B32A32_UINT, 16}; break;
        case 0x23: d = {VK_FORMAT_R32G32B32A32_SFLOAT, 16}; break;
        case 0x31: d = {srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, true}; break;
        case 0x32: d = {srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK, 16, true}; break;
        case 0x33: d = {srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK, 16, true}; break;
        case 0x34: d = {snorm ? VK_FORMAT_BC4_SNORM_BLOCK : VK_FORMAT_BC4_UNORM_BLOCK, 8, true}; break;
        case 0x35: d = {snorm ? VK_FORMAT_BC5_SNORM_BLOCK : VK_FORMAT_BC5_UNORM_BLOCK, 16, true}; break;
        default: break;
    }
    return d;
}

VkFormat depth_format_for(uint32_t f, VkFormat d24s8) {
    switch (f & 0xFFF) {
        case 0x011: case 0x811: return d24s8;
        case 0x806: case 0x80E: return VK_FORMAT_D32_SFLOAT;
        case 0x005: return VK_FORMAT_D16_UNORM;
        case 0x81C: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

// ---- GX2 enums -> Vulkan ----------------------------------------------------------------------------------------
VkBlendFactor blend_factor(uint32_t m) {
    static const VkBlendFactor t[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        // SRC1 factors need a second PS output, which the translated shaders do not have
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
    return m < std::size(t) ? t[m] : VK_BLEND_FACTOR_ONE;
}
VkBlendOp blend_op(uint32_t c) {
    switch (c) {
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
    }
}
VkPrimitiveTopology topology(uint32_t mode) {
    switch (mode) {
        case 0x01: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case 0x02: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case 0x03: case 0x12: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 0x05: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 0x06: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // TRIANGLES, QUADS / QUAD_STRIP (expanded)
    }
}
VkSamplerAddressMode address_mode(uint32_t c) {
    switch (c) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2: case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3: case 5: case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        case 6: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}
VkComponentSwizzle comp_sel(uint32_t s, VkComponentSwizzle identity) {
    switch (s) {
        case 0: return VK_COMPONENT_SWIZZLE_R;
        case 1: return VK_COMPONENT_SWIZZLE_G;
        case 2: return VK_COMPONENT_SWIZZLE_B;
        case 3: return VK_COMPONENT_SWIZZLE_A;
        case 4: return VK_COMPONENT_SWIZZLE_ZERO;
        case 5: return VK_COMPONENT_SWIZZLE_ONE;
        default: return identity;
    }
}
// Applies a base format swizzle under a GX2 comp_sel (view = format swizzle then the texture's selection).
VkComponentSwizzle compose(const VkComponentMapping& base, VkComponentSwizzle sel) {
    switch (sel) {
        case VK_COMPONENT_SWIZZLE_R: return base.r == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R : base.r;
        case VK_COMPONENT_SWIZZLE_G: return base.g == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_G : base.g;
        case VK_COMPONENT_SWIZZLE_B: return base.b == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_B : base.b;
        case VK_COMPONENT_SWIZZLE_A: return base.a == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_A : base.a;
        default: return sel;
    }
}

// ---- vertex attributes ----------------------------------------------------------------------------------------
// GX2AttribFormat: low byte element type, 0x100 integer, 0x200 signed, 0x800 scaled (int -> float, not normalised).
struct AttribLayout {
    uint8_t comps, bits;  // component count and bits per component; bits 0 = packed (see type)
    bool flt;
};
AttribLayout attrib_layout(uint32_t type) {
    switch (type) {
        case 0x00: return {1, 8, false};
        case 0x01: return {2, 4, false};
        case 0x02: return {1, 16, false};
        case 0x03: return {1, 16, true};
        case 0x04: return {2, 8, false};
        case 0x05: return {1, 32, false};
        case 0x06: return {1, 32, true};
        case 0x07: return {2, 16, false};
        case 0x08: return {2, 16, true};
        case 0x09: return {3, 0, true};  // 10_11_11 float (packed)
        case 0x0A: return {4, 8, false};
        case 0x0B: return {4, 0, false};  // 10_10_10_2 (packed)
        case 0x0C: return {2, 32, false};
        case 0x0D: return {2, 32, true};
        case 0x0E: return {4, 16, false};
        case 0x0F: return {4, 16, true};
        case 0x10: return {3, 32, false};
        case 0x11: return {3, 32, true};
        case 0x12: return {4, 32, false};
        case 0x13: return {4, 32, true};
        default: return {0, 0, false};
    }
}
uint32_t attrib_bytes(uint32_t type) {
    const AttribLayout l = attrib_layout(type);
    if (!l.bits) return 4;
    return std::max(1u, l.comps * l.bits / 8u);
}

// One attribute element -> the four 32-bit GPR values the fetch shader would produce (before the dest selection).
void decode_attrib(const uint8_t* p, uint32_t format, uint32_t endian, uint32_t out[4]) {
    const uint32_t type = format & 0xFF;
    const bool integer = format & 0x100, sign = format & 0x200, scaled = format & 0x800;
    const AttribLayout l = attrib_layout(type);
    const bool swap16 = endian == 1 || endian == 3, swap32 = endian == 2 || endian == 3;
    uint32_t raw[4] = {0, 0, 0, 0};
    uint32_t bits[4] = {l.bits, l.bits, l.bits, l.bits};
    if (type == 0x0B) {  // 10_10_10_2, x in the low bits
        const uint32_t w = swap32 ? be32(p) : le32(p);
        raw[0] = w & 0x3FF, raw[1] = (w >> 10) & 0x3FF, raw[2] = (w >> 20) & 0x3FF, raw[3] = w >> 30;
        bits[0] = bits[1] = bits[2] = 10, bits[3] = 2;
    } else if (type == 0x01) {
        raw[0] = p[0] & 0xF, raw[1] = p[0] >> 4;
    } else if (l.bits) {
        for (int c = 0; c < l.comps; ++c) {
            const uint8_t* q = p + c * (l.bits / 8);
            raw[c] = l.bits == 8 ? q[0] : l.bits == 16 ? (swap16 ? be16(q) : static_cast<uint32_t>(q[0] | q[1] << 8))
                                                       : (swap32 ? be32(q) : le32(q));
        }
    }
    const uint32_t one = integer ? 1u : 0x3F800000u;
    for (int c = 0; c < 4; ++c) {
        if (c >= l.comps) {
            out[c] = c == 3 ? one : 0;
            continue;
        }
        if (l.flt) {
            out[c] = l.bits == 16 ? fbits(half_to_float(static_cast<uint16_t>(raw[c]))) : raw[c];
            continue;
        }
        const uint32_t n = bits[c];
        int64_t v = raw[c];
        if (sign && n < 32 && (raw[c] >> (n - 1)) & 1) v -= int64_t{1} << n;
        if (integer) out[c] = static_cast<uint32_t>(v);
        else if (scaled) out[c] = fbits(static_cast<float>(v));
        else if (sign) out[c] = fbits(std::max(-1.0f, static_cast<float>(v) / static_cast<float>((int64_t{1} << (n - 1)) - 1)));
        else out[c] = fbits(static_cast<float>(v) / static_cast<float>((uint64_t{1} << n) - 1));
    }
}

// ---- shaders --------------------------------------------------------------------------------------------------
struct ShaderInfo {
    bool ok = false;
    uint64_t key = 0;
    VkShaderModule module = VK_NULL_HANDLE;
    std::vector<std::pair<uint32_t, uint32_t>> ubos;      // block, vec4 count
    std::vector<std::pair<uint32_t, uint32_t>> samplers;  // slot, GX2SamplerVarType
    std::vector<uint32_t> inputs;                         // attribute semantics (VS)
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
};

// Everything a graphics pipeline depends on, in terms that stay valid across runs (shader content keys, not module
// handles), so the pipelines a game session used can be listed on disk and built again before the next one starts.
struct PipelineDesc {
    static constexpr uint32_t kMaxAttribs = 32;
    uint64_t shader[3] = {};  // vs, ps, gs content keys (gs 0 = none)
    uint32_t topology = 0, neg_one = 0;
    uint32_t n_loc_v = 0, n_loc_i = 0, loc_v[kMaxAttribs] = {}, loc_i[kMaxAttribs] = {};
    uint32_t ncolor = 0;
    uint32_t color_format[8] = {};  // VK_FORMAT_UNDEFINED: target not written
    uint32_t blend_enable[8] = {}, blend[8][6] = {}, separate_alpha[8] = {}, write_mask[8] = {};
    uint32_t depth_format = 0, depth_test = 0, depth_write = 0, depth_func = 0;
    uint32_t stencil = 0, stencil_ops[2][4] = {};  // front, back: func, pass, depth_fail, fail
    uint32_t cull_front = 0, cull_back = 0, front_face = 0;
};

// One field order for both directions: KeyWriter appends the used fields as raw bytes (the pipeline map key and the
// record in pipelines.bin), KeyReader parses them back. Bump kPipelineListVersion when the order changes.
constexpr uint32_t kPipelineListVersion = 1;
template <class IO>
bool serialize(IO& io, PipelineDesc& d) {
    for (uint64_t& k : d.shader) io(k);
    io(d.topology);
    io(d.neg_one);
    io(d.n_loc_v);
    io(d.n_loc_i);
    if (d.n_loc_v > PipelineDesc::kMaxAttribs || d.n_loc_i > PipelineDesc::kMaxAttribs) return false;
    for (uint32_t i = 0; i < d.n_loc_v; ++i) io(d.loc_v[i]);
    for (uint32_t i = 0; i < d.n_loc_i; ++i) io(d.loc_i[i]);
    io(d.ncolor);
    if (d.ncolor > 8) return false;
    for (uint32_t t = 0; t < d.ncolor; ++t) {
        io(d.color_format[t]);
        io(d.blend_enable[t]);
        for (uint32_t& v : d.blend[t]) io(v);
        io(d.separate_alpha[t]);
        io(d.write_mask[t]);
    }
    io(d.depth_format);
    io(d.depth_test);
    io(d.depth_write);
    io(d.depth_func);
    io(d.stencil);
    if (d.stencil)
        for (auto& face : d.stencil_ops)
            for (uint32_t& v : face) io(v);
    io(d.cull_front);
    io(d.cull_back);
    io(d.front_face);
    return io.ok();
}
struct KeyWriter {
    std::string& out;
    template <class T>
    void operator()(const T& v) { out.append(reinterpret_cast<const char*>(&v), sizeof v); }
    bool ok() const { return true; }
};
struct KeyReader {
    const char* p;
    size_t left;
    bool good = true;
    template <class T>
    void operator()(T& v) {
        if (left < sizeof v) {
            good = false;
            v = T{};
            return;
        }
        std::memcpy(&v, p, sizeof v);
        p += sizeof v;
        left -= sizeof v;
    }
    bool ok() const { return good && left == 0; }
};

// Guest shader struct layouts: register block size = offset of the program size; program pointer follows.
constexpr uint32_t kRegsBytes[3] = {0xD0, 0xA4, 0x4C};
const char* kStageExt[3] = {"vert", "frag", "geom"};

// ---- device resources -----------------------------------------------------------------------------------------
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageType type = VK_IMAGE_TYPE_2D;
    uint32_t width = 0, height = 0, layers = 1, levels = 1;
    bool cube = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    std::map<uint64_t, VkImageView> views;
    GuestSurface surf;
    VkComponentMapping base_swizzle{};
    uint64_t hash = 0, checked_frame = ~0ull;
    uint32_t seen_epoch = 0;  // guest-memory write epoch when the contents were last checked (texture())
    std::unique_ptr<Image> depth_alias;  // R32F render target sampled with depth compare
    std::unique_ptr<Image> snapshot;     // copy for draws that sample their own render target
    std::unique_ptr<Image> replacement;  // texture pack image sampled instead (replace_texture())
    uint64_t uploaded_frame = ~0ull;     // last upload: a texture re-uploaded every frame is not looked up in packs
    // bumped by transition() into a writable layout (every render, clear, copy or upload goes through one);
    // depth_alias is refreshed only when this changed since its last copy
    uint64_t writes = 0, alias_writes = ~0ull;
};

struct Chunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceSize size = 0, used = 0;
};
struct Alloc {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceSize offset = 0;
    uint8_t* ptr = nullptr;
};

struct FrameCtx {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> extra_pools;
    std::vector<Chunk> chunks;
    std::vector<Chunk> retired;  // vertex arena chunks to free once this submission is done
    std::vector<std::unique_ptr<Image>> garbage;
    bool timed = false;  // GPU timestamps written for this submission (read back when the slot is reused)
};

class VkRenderer final : public Renderer {
public:
    VkRenderer(const VulkanContext& vk, uint8_t* base, std::filesystem::path dir) : vk_(vk), base_(base), dir_(std::move(dir)) {}
    ~VkRenderer() override;
    bool init(std::string* error);

    bool clear_color(const GuestSurface& s, const float rgba[4]) override;
    bool clear_depth_stencil(const GuestSurface& s, float depth, uint32_t stencil, uint32_t flags) override;
    bool copy_surface(const GuestSurface& src, uint32_t src_slice, const GuestSurface& dst, uint32_t dst_slice) override;
    bool scan_out(const GuestSurface& s, bool gamepad) override;
    void set_gamepad_view(int mode) override { gamepad_view_ = mode; }
    void draw(const DrawState& state, const DrawCall& call) override;
    Frame end_frame() override;
    bool read_tv(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;
    bool read_target(uint32_t image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) override;
    std::string stats() const override;
    std::vector<std::pair<uint32_t, uint32_t>> target_ranges() const override {
        std::vector<std::pair<uint32_t, uint32_t>> r;
        for (const auto& [addr, img] : targets_) r.emplace_back(addr, img->surf.image_size);
        return r;
    }
    void set_trace(bool on) override { trace_on_ = on; }
    std::string take_trace() override { return std::move(trace_); }

private:
    bool trace_on_ = false;
    uint32_t debug_ = 0;  // MH3U_RENDER_DEBUG flags
    std::string trace_;
    void tracef(const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        trace_ += buf;
    }
    VulkanContext vk_;
    uint8_t* base_;
    std::filesystem::path dir_;
    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memprops_{};
    VkDeviceSize ubo_align_ = 256;
    VkFormat d24s8_ = VK_FORMAT_D24_UNORM_S8_UINT;
    VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
    FrameCtx frames_[2];
    uint64_t frame_ = 0;
    bool recording_ = false;
    // dynamic rendering in progress
    bool rendering_ = false;
    std::vector<Image*> active_color_;
    Image* active_depth_ = nullptr;
    std::unordered_map<uint32_t, std::unique_ptr<Image>> targets_;   // render targets by guest image address
    // other views (format/size) of the same address, kept for reuse (target())
    std::unordered_map<uint32_t, std::vector<std::unique_ptr<Image>>> spare_targets_;
    std::unordered_map<uint64_t, std::unique_ptr<Image>> textures_;  // uploaded textures by (address, params)
    std::unique_ptr<Image> tv_, drc_;
    bool tv_written_ = false, drc_written_ = false;
    int gamepad_view_ = 0;
    std::map<std::tuple<int, uint32_t, uint32_t, uint32_t>, ShaderInfo*> shaders_;  // by guest struct + program
    std::map<std::pair<int, uint64_t>, std::unique_ptr<ShaderInfo>> shaders_by_key_;  // by stage + content key
    std::map<std::string, VkDescriptorSetLayout> set_layouts_;
    VkDescriptorSetLayout empty_layout_ = VK_NULL_HANDLE;
    std::map<std::tuple<VkDescriptorSetLayout, VkDescriptorSetLayout, VkDescriptorSetLayout>, VkPipelineLayout> pipe_layouts_;
    std::unordered_map<std::string, VkPipeline> pipelines_;  // by encoded PipelineDesc
    std::string pipe_key_;  // draw(): pipeline key buffer, reused
    PipelineDesc pipe_desc_;
    // Pipelines persist across runs: the driver's compiled code in a VkPipelineCache (pipeline_cache.bin) and the list
    // of every pipeline the game has used (pipelines.bin, one encoded PipelineDesc per record), rebuilt by precompile().
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    std::filesystem::path cache_dir_;
    size_t pipelines_saved_ = 0;  // pipelines_.size() when pipeline_cache.bin was last written
    // draw(): scratch vectors, reused (cleared each draw)
    std::vector<uint32_t> scratch_indices_, scratch_loc_v_, scratch_loc_i_, scratch_key_;
    std::vector<const AttribStream*> scratch_str_v_, scratch_str_i_;
    std::vector<VkDescriptorBufferInfo> scratch_bufs_;
    std::vector<VkDescriptorImageInfo> scratch_imgs_;
    std::vector<VkWriteDescriptorSet> scratch_writes_;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, bool>, VkSampler> samplers_;
    std::unique_ptr<Image> dummy_[3];  // 1D, 2D, cube: opaque black
    std::unique_ptr<Image> dummy_shadow_;
    std::set<std::string> warned_;
    uint64_t n_draws_ = 0, n_skipped_ = 0, n_uploads_ = 0, n_target_recreates_ = 0;
    // per-submission GPU time (timestamp queries, 2 per frame slot) and bytes streamed through alloc(), for stats()
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    double ts_period_ns_ = 1.0;
    std::vector<float> gpu_ms_, stream_mb_;
    uint64_t bytes_vertex_ = 0, bytes_ubo_ = 0;  // streamed totals by kind
    // converted vertex data by stream layout + range (draw()'s fill), valid while the source bytes hash the same
    struct VertexCacheEntry {
        uint64_t src = 0;
        std::vector<uint8_t> data;           // converted, CPU copy
        VkBuffer gpu = VK_NULL_HANDLE;       // static entries: resident copy in the vertex arena
        VkDeviceSize gpu_offset = 0;
        uint64_t converted_frame = ~0ull;
        bool dynamic = false;                // source changed on consecutive frames: streamed every draw instead
    };
    std::unordered_map<uint64_t, VertexCacheEntry> vcache_;
    // rebased indices by index buffer (draw()), valid while the raw index bytes hash the same
    struct IndexCacheEntry {
        bool valid = false, dynamic = false;
        uint64_t src = 0, converted_frame = ~0ull;
        uint32_t vmin = 0, vmax = 0;  // before base_vertex; vmin > vmax: only restart indices
        std::vector<uint32_t> indices;
        VkBuffer gpu = VK_NULL_HANDLE;
        VkDeviceSize gpu_offset = 0;
    };
    std::unordered_map<uint64_t, IndexCacheEntry> icache_;
    uint64_t n_index_converts_ = 0;
    size_t vcache_bytes_ = 0;
    std::vector<Chunk> varena_;
    VkDeviceSize varena_bytes_ = 0;
    uint64_t n_vertex_converts_ = 0;
    std::vector<core::GuestMemory::Range> watched_;  // pages of cached textures (merged), given to the write watch
    bool watched_stale_ = true;                      // a texture was added: rebuild watched_
    uint64_t scanned_frame_ = ~0ull;                 // frame of the last scan_writes()
    uint64_t n_texture_hashes_ = 0;
    std::map<std::string, uint64_t> skip_reasons_;
    // texture packs (MH3U_TEXTURE_DIR, on/off MH3U_TEXTURE_PACKS / set_texture_packs) and dumping (MH3U_DUMP_TEXTURES)
    texpack::Library packs_;
    std::filesystem::path dump_dir_;
    std::set<uint64_t> dumped_;
    std::atomic<bool> want_packs_{true};
    uint64_t n_replaced_ = 0;
    std::atomic<uint64_t> n_dumped_{0};
    std::atomic<bool> want_dump_{false};
    bool dumping_ = false;  // this frame's (want_dump_; switching it on re-checks every cached texture)
    void set_texture_dump(bool on) override { want_dump_ = on; }
    uint64_t texture_dumps() const override { return n_dumped_; }
    void init_packs();
    void replace_texture(Image& img, const GuestSurface& s, const FormatDesc& fd, size_t image_bytes, const std::vector<uint8_t>& level0,
                         uint32_t row_elements);
    void set_texture_packs(bool on) override { want_packs_ = on; }

    void warn(const std::string& w) {
        if (warned_.insert(w).second) std::fprintf(stderr, "renderer: %s\n", w.c_str());
    }
    void skip(const std::string& why) {
        ++n_skipped_;
        ++skip_reasons_[why];
    }
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags props) const {
        for (uint32_t i = 0; i < memprops_.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memprops_.memoryTypes[i].propertyFlags & props) == props) return i;
        return UINT32_MAX;
    }
    FrameCtx& fc() { return frames_[frame_ % 2]; }
    VkCommandBuffer cmd() {
        begin();
        return fc().cmd;
    }
    void begin();
    void end_rendering();
    Alloc alloc(VkDeviceSize size, VkDeviceSize align);
    Alloc arena_alloc(VkDeviceSize size);
    void retire_vertex_arena();
    Alloc upload_static(const void* data, size_t bytes, VkPipelineStageFlags2 stage, VkAccessFlags2 access);
    std::unique_ptr<Image> create_image(VkFormat format, VkImageType type, uint32_t w, uint32_t h, uint32_t layers,
                                        bool cube, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t levels = 1);
    void destroy_image(Image& img);
    void transition(Image& img, VkImageLayout to);
    VkImageView view(Image& img, VkImageViewType type, VkComponentMapping swz, VkImageAspectFlags aspect = 0);
    VkFormatFeatureFlags features(VkFormat f) const {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(vk_.phys, f, &p);
        return p.optimalTilingFeatures;
    }
    Image* target(const GuestSurface& s, bool depth, bool upload);
    Image* texture(const TextureBinding& t, uint32_t sampler_type);
    // level0 (optional): also returns level 0 / layer 0 untiled, rows of *row_elements elements (texture dumps)
    void upload_surface(Image& img, const GuestSurface& s, const FormatDesc& fd, uint32_t layers, std::vector<uint8_t>* level0 = nullptr,
                        uint32_t* row_elements = nullptr);
    Image* depth_alias(Image& color);
    void scan_writes();
    Image* snapshot(Image& img);
    VkSampler sampler(const SamplerState& s, bool compare);
    ShaderInfo* shader(int stage, uint32_t addr);
    ShaderInfo* shader_by_key(int stage, uint64_t key);
    VkDescriptorSetLayout set_layout(const ShaderInfo* s);
    VkPipelineLayout pipeline_layout(const ShaderInfo* vs, const ShaderInfo* ps, const ShaderInfo* gs);
    VkPipeline create_pipeline(const PipelineDesc& d, VkPipelineLayout layout, const ShaderInfo* const stages[3]) const;
    void record_pipeline(const std::string& key);  // appends to the pipeline list (precompiled on the next start)

public:
    size_t precompile(const std::function<void(size_t, size_t)>& progress) override;
    void save_caches() override;

private:
    VkDescriptorSet descriptor_set(VkDescriptorSetLayout layout);
    void fill_dummies();
    // "Look" post pass on the TV image (MH3U_LOOK=tri: the Wii-era grade); null pipeline = off
    int look_ = 0;
    // internal resolution (MH3U_RENDER_SCALE): render targets are this multiple of their guest size; viewports,
    // scissors and point sizes follow. Shaders sample with normalised coordinates, so they need no change.
    float scale_ = 1.0f;
    uint32_t scaled(uint32_t v) const { return std::max(1u, static_cast<uint32_t>(std::lround(v * scale_))); }
    VkPipeline post_pipe_ = VK_NULL_HANDLE;
    std::unique_ptr<Image> post_;
    bool init_post(std::string* error);
    Image* post_process(Image& src);
    // our full-screen passes (look, ambient occlusion, depth of field): shared layouts, pipelines by (shader, format, blend)
    enum class FxBlend { None, Multiply, Ease };
    VkDescriptorSetLayout fx_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout fx_layout_ = VK_NULL_HANDLE;
    VkSampler post_sampler_ = VK_NULL_HANDLE, fx_nearest_ = VK_NULL_HANDLE;
    VkShaderModule fx_vs_ = VK_NULL_HANDLE, ao_fs_ = VK_NULL_HANDLE, ao_apply_fs_ = VK_NULL_HANDLE, focus_fs_ = VK_NULL_HANDLE,
                   dof_fs_ = VK_NULL_HANDLE, tri_fs_ = VK_NULL_HANDLE, gi_fs_ = VK_NULL_HANDLE, gi_apply_fs_ = VK_NULL_HANDLE;
    std::vector<VkShaderModule> fx_modules_;
    std::map<std::tuple<VkShaderModule, VkFormat, FxBlend>, VkPipeline> fx_pipes_;
    VkShaderModule fx_module(const std::string& glsl, const char* name, bool vertex, std::string* error);
    VkPipeline fx_pipeline(VkShaderModule fs, VkFormat fmt, FxBlend blend);
    void fx_pass(VkPipeline pipe, Image& out, std::initializer_list<Image*> tex, const float push[8]);
    // scene effects (MH3U_AO, MH3U_DOF; tunables MH3U_AO_RADIUS etc.): once a frame on the TV-sized scene target
    bool ao_ = false, dof_ = false, gi_ = false;  // this frame's (from want_*, set_effects)
    std::atomic<bool> want_look_{false}, want_ao_{false}, want_dof_{false}, want_vignette_{false}, want_gi_{false};
    bool tri_ = false, vignette_ = false;  // this frame's Tri scene pass / vignette post pass (look_ = the vignette)
    void set_effects(bool tri_look, bool ambient_occlusion, bool depth_of_field, bool vignette, bool global_illumination) override {
        want_vignette_ = vignette;
        want_look_ = tri_look;
        want_ao_ = ambient_occlusion;
        want_dof_ = depth_of_field;
        want_gi_ = global_illumination;
    }
    // GI radius in near-plane units and bounce strength (MH3U_GI_RADIUS, MH3U_GI_STRENGTH)
    float gi_radius_ = 600.0f, gi_strength_ = 0.6f;
    std::unique_ptr<Image> gi_img_;
    // Tri: glow radius (px at 1080p), haze strength / start / per unit (near-plane units), glow strength (MH3U_TRI_*)
    float tri_glow_radius_ = 18.0f, tri_haze_ = 0.30f, tri_haze_start_ = 3000.0f, tri_haze_k_ = 1.0f / 25000.0f, tri_glow_ = 0.16f;
    // AO radius in near-plane units (MH3U's near plane is small: the hunter is ~1500 away in a village shot)
    float ao_radius_ = 200.0f, ao_strength_ = 1.4f;
    // px at 1080p; z / focus - 1. The focus is the hunter in the field, so the blur starts well behind them (from 4x
    // their distance, full at 16x): with 0.8 / 5 most of a map was blurred (user 2026-10-28, "the dof is too close")
    float dof_radius_ = 5.0f, dof_start_ = 3.0f, dof_range_ = 12.0f, dof_near_ = 0.0f;
    uint32_t tv_guest_w_ = 0, tv_guest_h_ = 0;  // guest size of the last TV scan-out
    uint32_t scene_color_ = 0, scene_depth_ = 0;  // this frame's scene targets (guest image addresses)
    bool scene_done_ = false;
    float fx_debug_ = 0;  // MH3U_FX_DEBUG=1: the DOF pass shows depth instead
    std::unique_ptr<Image> ao_img_, focus_img_;
    void scene_hook(const DrawState& st);
    void scene_effects(Image& color, Image& depth);
};

VkRenderer::~VkRenderer() {
    if (!dev_) return;
    vkDeviceWaitIdle(dev_);
    for (FrameCtx& f : frames_) {
        for (auto& g : f.garbage) destroy_image(*g);
        for (const auto* list : {&f.chunks, &f.retired})
            for (const Chunk& c : *list) {
                vkDestroyBuffer(dev_, c.buffer, nullptr);
                vkFreeMemory(dev_, c.mem, nullptr);
            }
        vkDestroyDescriptorPool(dev_, f.pool, nullptr);
        for (VkDescriptorPool p : f.extra_pools) vkDestroyDescriptorPool(dev_, p, nullptr);
        vkDestroyFence(dev_, f.fence, nullptr);
    }
    if (ts_pool_) vkDestroyQueryPool(dev_, ts_pool_, nullptr);
    for (const Chunk& c : varena_) {
        vkDestroyBuffer(dev_, c.buffer, nullptr);
        vkFreeMemory(dev_, c.mem, nullptr);
    }
    for (auto& [k, t] : targets_) destroy_image(*t);
    for (auto& [k, v] : spare_targets_)
        for (auto& t : v) destroy_image(*t);
    for (auto& [k, t] : textures_) destroy_image(*t);
    for (auto& d : dummy_)
        if (d) destroy_image(*d);
    if (dummy_shadow_) destroy_image(*dummy_shadow_);
    if (tv_) destroy_image(*tv_);
    for (auto& [k, p] : pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& [k, l] : pipe_layouts_) vkDestroyPipelineLayout(dev_, l, nullptr);
    for (auto& [k, l] : set_layouts_) vkDestroyDescriptorSetLayout(dev_, l, nullptr);
    for (auto& [k, s] : samplers_) vkDestroySampler(dev_, s, nullptr);
    for (auto& [k, s] : shaders_by_key_)
        if (s->module) vkDestroyShaderModule(dev_, s->module, nullptr);
    if (pipeline_cache_) vkDestroyPipelineCache(dev_, pipeline_cache_, nullptr);
    for (Image* i : {post_.get(), ao_img_.get(), focus_img_.get()})
        if (i) destroy_image(*i);
    for (auto& [k, p] : fx_pipes_)
        if (p) vkDestroyPipeline(dev_, p, nullptr);
    for (VkShaderModule m : fx_modules_) vkDestroyShaderModule(dev_, m, nullptr);
    if (fx_layout_) vkDestroyPipelineLayout(dev_, fx_layout_, nullptr);
    if (fx_set_layout_) vkDestroyDescriptorSetLayout(dev_, fx_set_layout_, nullptr);
    for (VkSampler s : {post_sampler_, fx_nearest_})
        if (s) vkDestroySampler(dev_, s, nullptr);
    vkDestroyCommandPool(dev_, cmd_pool_, nullptr);
}

VkDescriptorPool make_pool(VkDevice dev) {
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384}};
    VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ci.maxSets = 8192;
    ci.poolSizeCount = 2;
    ci.pPoolSizes = sizes;
    VkDescriptorPool p = VK_NULL_HANDLE;
    vkCreateDescriptorPool(dev, &ci, nullptr, &p);
    return p;
}

bool VkRenderer::init(std::string* error) {
    char* dbg = nullptr;
    size_t dbg_len = 0;
    if (_dupenv_s(&dbg, &dbg_len, "MH3U_RENDER_DEBUG") == 0 && dbg) debug_ = static_cast<uint32_t>(std::strtoul(dbg, nullptr, 0));
    std::free(dbg);  // MH3U_RENDER_DEBUG: 1 no alpha test, 2 no blending, 8 only geometry-shader draws, 16 log write watch
    dev_ = vk_.device;
    // pipeline cache next to the shader cache (cache/), seeded from the last run; the driver rejects a blob from
    // another GPU or driver version by itself (header check), so a stale file only costs the rebuild
    cache_dir_ = dir_.parent_path();
    {
        std::ifstream cf(cache_dir_ / "pipeline_cache.bin", std::ios::binary);
        const std::vector<char> blob((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = blob.size();
        ci.pInitialData = blob.empty() ? nullptr : blob.data();
        if (vkCreatePipelineCache(dev_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
            ci.initialDataSize = 0;
            ci.pInitialData = nullptr;
            vkCreatePipelineCache(dev_, &ci, nullptr, &pipeline_cache_);
        }
    }
    vkGetPhysicalDeviceMemoryProperties(vk_.phys, &memprops_);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk_.phys, &props);
    ubo_align_ = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
    if (!(features(d24s8_) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) d24s8_ = VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk_.queue_family;
    if (vkCreateCommandPool(dev_, &pci, nullptr, &cmd_pool_) != VK_SUCCESS) {
        if (error) *error = "vkCreateCommandPool failed";
        return false;
    }
    for (FrameCtx& f : frames_) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = cmd_pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev_, &cai, &f.cmd);
        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev_, &fi, nullptr, &f.fence);
        f.pool = make_pool(dev_);
    }
    VkPhysicalDeviceProperties dev_props{};
    vkGetPhysicalDeviceProperties(vk_.phys, &dev_props);
    if (dev_props.limits.timestampPeriod > 0) {
        ts_period_ns_ = dev_props.limits.timestampPeriod;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qci.queryCount = 4;  // {begin, end} per frame slot
        vkCreateQueryPool(dev_, &qci, nullptr, &ts_pool_);
    }
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    vkCreateDescriptorSetLayout(dev_, &lci, nullptr, &empty_layout_);
    set_layouts_[""] = empty_layout_;
    fill_dummies();
    init_packs();
    if (std::string err; !init_post(&err)) std::fprintf(stderr, "renderer: %s (that effect is off)\n", err.c_str());
    return true;
}

// ---- frame / command recording ----------------------------------------------------------------------------------
void VkRenderer::begin() {
    if (recording_) return;
    FrameCtx& f = fc();
    vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev_, 1, &f.fence);
    const uint32_t q = static_cast<uint32_t>(frame_ % 2) * 2;
    if (f.timed && ts_pool_) {
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(dev_, ts_pool_, q, 2, sizeof ts, ts, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            gpu_ms_.push_back(static_cast<float>(static_cast<double>(ts[1] - ts[0]) * ts_period_ns_ / 1e6));
        VkDeviceSize used = 0;
        for (const Chunk& c : f.chunks) used += c.used;
        stream_mb_.push_back(static_cast<float>(static_cast<double>(used) / (1 << 20)));
    }
    f.timed = false;
    for (auto& g : f.garbage) destroy_image(*g);
    f.garbage.clear();
    for (const Chunk& c : f.retired) {
        vkDestroyBuffer(dev_, c.buffer, nullptr);
        vkFreeMemory(dev_, c.mem, nullptr);
    }
    f.retired.clear();
    for (Chunk& c : f.chunks) c.used = 0;
    vkResetDescriptorPool(dev_, f.pool, 0);
    for (VkDescriptorPool p : f.extra_pools) vkDestroyDescriptorPool(dev_, p, nullptr);
    f.extra_pools.clear();
    vkResetCommandBuffer(f.cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(f.cmd, &bi);
    if (ts_pool_) {
        vkCmdResetQueryPool(f.cmd, ts_pool_, q, 2);
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool_, q);
    }
    recording_ = true;
}

void VkRenderer::end_rendering() {
    if (!rendering_) return;
    vkc::cmd_end_rendering(fc().cmd);
    rendering_ = false;
    active_color_.clear();
    active_depth_ = nullptr;
}

Alloc VkRenderer::alloc(VkDeviceSize size, VkDeviceSize align) {
    FrameCtx& f = fc();
    for (Chunk& c : f.chunks) {
        const VkDeviceSize off = (c.used + align - 1) / align * align;
        if (off + size <= c.size) {
            c.used = off + size;
            return {c.buffer, off, c.map + off};
        }
    }
    Chunk c;
    c.size = std::max<VkDeviceSize>(32ull << 20, size);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = c.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    vkCreateBuffer(dev_, &bci, nullptr, &c.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, c.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev_, &mai, nullptr, &c.mem);
    vkBindBufferMemory(dev_, c.buffer, c.mem, 0);
    void* p = nullptr;
    vkMapMemory(dev_, c.mem, 0, VK_WHOLE_SIZE, 0, &p);
    c.map = static_cast<uint8_t*>(p);
    c.used = size;
    f.chunks.push_back(c);
    return {c.buffer, 0, c.map};
}

// Device-local vertex arena (static converted vertex data, draw()'s fill): bump allocation in 64 MB chunks, never
// freed piecemeal – retire_vertex_arena() drops the whole arena and the cache that points into it.
Alloc VkRenderer::arena_alloc(VkDeviceSize size) {
    constexpr VkDeviceSize kAlign = 256;
    for (Chunk& c : varena_) {
        const VkDeviceSize off = (c.used + kAlign - 1) / kAlign * kAlign;
        if (off + size <= c.size) {
            c.used = off + size;
            return {c.buffer, off, nullptr};
        }
    }
    Chunk c;
    c.size = std::max<VkDeviceSize>(64ull << 20, size);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = c.size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    vkCreateBuffer(dev_, &bci, nullptr, &c.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, c.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(dev_, &mai, nullptr, &c.mem);
    vkBindBufferMemory(dev_, c.buffer, c.mem, 0);
    c.used = size;
    varena_.push_back(c);
    varena_bytes_ += c.size;
    return {c.buffer, 0, nullptr};
}

void VkRenderer::retire_vertex_arena() {
    for (Chunk& c : varena_) fc().retired.push_back(c);  // freed once the current submission is done
    varena_.clear();
    varena_bytes_ = 0;
    vcache_.clear();
    vcache_bytes_ = 0;
    icache_.clear();
}

// Copies converted static data into the arena once (outside rendering) and makes it visible to `stage`/`access`.
Alloc VkRenderer::upload_static(const void* data, size_t bytes, VkPipelineStageFlags2 stage, VkAccessFlags2 access) {
    const Alloc g = arena_alloc(bytes);
    const Alloc staging = alloc(bytes, 16);
    std::memcpy(staging.ptr, data, bytes);
    end_rendering();
    const VkBufferCopy cp{staging.offset, g.offset, bytes};
    vkCmdCopyBuffer(cmd(), staging.buffer, g.buffer, 1, &cp);
    VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.dstStageMask = stage;
    bb.dstAccessMask = access;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = g.buffer;
    bb.offset = g.offset;
    bb.size = bytes;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &bb;
    vkc::cmd_pipeline_barrier2(cmd(), dep);
    return g;
}

std::unique_ptr<Image> VkRenderer::create_image(VkFormat format, VkImageType type, uint32_t w, uint32_t h, uint32_t layers,
                                                bool cube, VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t levels) {
    auto img = std::make_unique<Image>();
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.imageType = type;
    ici.format = format;
    ici.extent = {std::max(1u, w), type == VK_IMAGE_TYPE_1D ? 1u : std::max(1u, h), 1};
    ici.mipLevels = std::max(1u, levels);
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(dev_, &ici, nullptr, &img->image) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev_, img->image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev_, &mai, nullptr, &img->mem) != VK_SUCCESS) {
        vkDestroyImage(dev_, img->image, nullptr);
        return nullptr;
    }
    vkBindImageMemory(dev_, img->image, img->mem, 0);
    img->format = format;
    img->type = type;
    img->width = ici.extent.width;
    img->height = ici.extent.height;
    img->layers = layers;
    img->levels = ici.mipLevels;
    img->cube = cube;
    img->aspect = aspect;
    return img;
}

void VkRenderer::destroy_image(Image& img) {
    for (auto& [k, v] : img.views) {
        vkc::forget_view(v);
        vkDestroyImageView(dev_, v, nullptr);
    }
    img.views.clear();
    if (img.depth_alias) destroy_image(*img.depth_alias);
    if (img.snapshot) destroy_image(*img.snapshot);
    if (img.replacement) destroy_image(*img.replacement);
    if (img.image) vkDestroyImage(dev_, img.image, nullptr);
    if (img.mem) vkFreeMemory(dev_, img.mem, nullptr);
    img.image = VK_NULL_HANDLE;
    img.mem = VK_NULL_HANDLE;
}

void VkRenderer::transition(Image& img, VkImageLayout to) {
    const bool attachment = to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL || to == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (attachment || to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) ++img.writes;
    if (img.layout == to) {
        if (!attachment && to != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) return;  // read-only: nothing to order
        if (attachment && rendering_ &&
            (active_depth_ == &img || std::find(active_color_.begin(), active_color_.end(), &img) != active_color_.end()))
            return;  // in use by the current rendering
    }
    end_rendering();  // barriers (and the transfer that follows) cannot be inside dynamic rendering
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = img.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {img.aspect, 0, img.levels, 0, img.layers};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkc::cmd_pipeline_barrier2(cmd(), dep);
    img.layout = to;
}

VkImageView VkRenderer::view(Image& img, VkImageViewType type, VkComponentMapping swz, VkImageAspectFlags aspect) {
    if (!aspect) aspect = img.aspect;
    const uint64_t key = fnv(&swz, sizeof swz, fnv(&type, sizeof type, aspect));
    auto it = img.views.find(key);
    if (it != img.views.end()) return it->second;
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = img.image;
    ci.viewType = type;
    ci.format = img.format;
    ci.components = swz;
    ci.subresourceRange = {aspect, 0, img.levels, 0, type == VK_IMAGE_VIEW_TYPE_CUBE ? 6u : 1u};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(dev_, &ci, nullptr, &v);
    img.views[key] = v;
    return v;
}

void VkRenderer::fill_dummies() {
    const VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    dummy_[0] = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_1D, 1, 1, 1, false, u, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy_[1] = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, 1, 1, 1, false, u, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy_[2] = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, 1, 1, 6, true, u, VK_IMAGE_ASPECT_COLOR_BIT);
    dummy_shadow_ = create_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, 1, 1, 1, false, u, VK_IMAGE_ASPECT_DEPTH_BIT);
    for (auto& d : dummy_) {
        transition(*d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue c{};
        c.float32[3] = 1.0f;
        const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, d->layers};
        vkCmdClearColorImage(cmd(), d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        transition(*d, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    transition(*dummy_shadow_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue dv{1.0f, 0};
    const VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd(), dummy_shadow_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &dr);
    transition(*dummy_shadow_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// ---- surfaces -------------------------------------------------------------------------------------------------
addrlib::SurfaceDesc surface_desc(const GuestSurface& s) {
    addrlib::SurfaceDesc d;
    d.dim = s.dim;
    d.width = s.width;
    d.height = s.height;
    d.depth = s.depth;
    d.levels = s.mips;
    d.format = s.format;
    d.aa = s.aa;
    d.use = s.use;
    d.image_size = s.image_size;
    d.mip_size = s.mip_size;
    d.tile_mode = s.tile;
    d.swizzle = s.swizzle;
    d.pitch = s.pitch;
    std::copy(std::begin(s.mip_offset), std::end(s.mip_offset), d.mip_offset);
    return d;
}

// Levels a texture can use: the guest's mip count, if it has a mip chain, capped to the full chain.
uint32_t texture_levels(const GuestSurface& s) {
    if (s.mips <= 1 || !s.mipmaps) return 1;
    const uint32_t full = 32 - static_cast<uint32_t>(std::countl_zero(std::max({s.width, s.height, 1u})));
    return std::min({s.mips, full, 14u});
}

// Detiles every level and layer of a guest surface (addressing from runtime/gfx/addrlib) into a staging allocation and
// copies it into the image.
void VkRenderer::upload_surface(Image& img, const GuestSurface& s, const FormatDesc& fd, uint32_t layers, std::vector<uint8_t>* level0,
                                uint32_t* row_elements) {
    const addrlib::SurfaceDesc d = surface_desc(s);
    std::vector<addrlib::Level> lv;
    size_t total = 0;
    for (uint32_t level = 0; level < img.levels; ++level)
        for (uint32_t layer = 0; layer < layers; ++layer) {
            addrlib::Level l;
            if (!addrlib::level_layout(d, s.image, s.mipmaps, level, layer, l) || uint64_t{l.base} + l.span > (uint64_t{1} << 32)) return;
            total += size_t{l.width} * l.height * l.bytes;
            lv.push_back(l);
        }
    const Alloc a = alloc(std::max<size_t>(total, 16), 16);
    std::vector<VkBufferImageCopy> copies;
    size_t at = 0;
    for (uint32_t level = 0; level < img.levels; ++level) {
        VkBufferImageCopy c{};
        c.bufferOffset = a.offset + at;
        for (uint32_t layer = 0; layer < layers; ++layer) {
            const addrlib::Level& l = lv[size_t{level} * layers + layer];
            const uint8_t* src = base_ + l.base;
            const uint32_t* off = l.offsets->data();
            const size_t n = l.offsets->size();
            uint8_t* dst = a.ptr + at;
            if (l.bytes == 4) {
                uint32_t* d32 = reinterpret_cast<uint32_t*>(dst);
                for (size_t i = 0; i < n; ++i) std::memcpy(&d32[i], src + off[i], 4);
            } else {
                for (size_t i = 0; i < n; ++i) std::memcpy(dst + i * l.bytes, src + off[i], l.bytes);
            }
            if (layer == 0) {
                c.bufferRowLength = fd.block ? l.width * 4 : l.width;
                c.bufferImageHeight = img.type == VK_IMAGE_TYPE_1D ? 0 : fd.block ? l.height * 4 : l.height;
                if (level == 0 && level0) {
                    level0->assign(dst, dst + n * l.bytes);
                    if (row_elements) *row_elements = l.width;
                }
            }
            at += n * l.bytes;
        }
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
        c.imageExtent = {std::max(1u, img.width >> level), std::max(1u, img.height >> level), 1};
        copies.push_back(c);
    }
    transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd(), a.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()),
                           copies.data());
    ++n_uploads_;
}

Image* VkRenderer::target(const GuestSurface& s, bool depth, bool upload) {
    if (!s.image || !s.width || !s.height) return nullptr;
    VkFormat format;
    FormatDesc fd;
    if (depth) {
        format = depth_format_for(s.format, d24s8_);
    } else {
        fd = color_format(s.format);
        format = fd.vk;
        if (format != VK_FORMAT_UNDEFINED && !(features(format) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) format = VK_FORMAT_UNDEFINED;
    }
    if (format == VK_FORMAT_UNDEFINED) {
        char w[96];
        std::snprintf(w, sizeof w, "%s render target format 0x%03X unsupported", depth ? "depth" : "colour", s.format);
        warn(w);
        return nullptr;
    }
    auto it = targets_.find(s.image);
    if (it != targets_.end()) {
        Image& img = *it->second;
        if (img.format == format && img.surf.width == s.width && img.surf.height == s.height) return &img;
        // The game reuses scratch memory for different views within a frame (MH3U: 0x15CF0000 is a 1024x1024 shadow
        // depth and a 480x270 RGBA8 target every frame). Park the other view instead of destroying it, and take a
        // parked image of this view if there is one: no reallocation, and no upload – guest memory does not hold
        // what the GPU rendered, and the game clears these before use.
        auto& spares = spare_targets_[s.image];
        spares.push_back(std::move(it->second));
        targets_.erase(it);
        for (auto sp = spares.begin(); sp != spares.end(); ++sp) {
            if ((*sp)->format != format || (*sp)->surf.width != s.width || (*sp)->surf.height != s.height) continue;
            Image* p = sp->get();
            p->surf = s;
            targets_[s.image] = std::move(*sp);
            spares.erase(sp);
            return p;
        }
        ++n_target_recreates_;
        constexpr size_t kMaxSpares = 4;  // views kept per address; older ones are freed
        if (spares.size() > kMaxSpares) {
            fc().garbage.push_back(std::move(spares.front()));
            spares.erase(spares.begin());
        }
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    usage |= depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    if (depth) aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    auto img = create_image(format, VK_IMAGE_TYPE_2D, scaled(s.width), scaled(s.height), 1, false, usage, aspect);
    if (!img) return nullptr;
    img->surf = s;
    img->base_swizzle = fd.swizzle;
    if (upload && !depth && !fd.block) {
        if (scale_ == 1.0f) {
            upload_surface(*img, s, fd, 1);
        } else {  // guest pixels at guest size, then scaled into the target
            auto staging = create_image(format, VK_IMAGE_TYPE_2D, s.width, s.height, 1, false,
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, aspect);
            if (staging) {
                upload_surface(*staging, s, fd, 1);
                transition(*staging, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkImageBlit bl{};
                bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                bl.srcOffsets[1] = {static_cast<int32_t>(s.width), static_cast<int32_t>(s.height), 1};
                bl.dstOffsets[1] = {static_cast<int32_t>(img->width), static_cast<int32_t>(img->height), 1};
                vkCmdBlitImage(cmd(), staging->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img->image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
                fc().garbage.push_back(std::move(staging));
            }
        }
    }
    Image* p = img.get();
    targets_[s.image] = std::move(img);
    return p;
}

// Keeps GuestMemory's write-watch scanner (a background thread) on the pages of the cached textures; texture() asks
// it which were written. A texture the CPU writes just before or during rendering is seen a frame later.
void VkRenderer::scan_writes() {
    if (scanned_frame_ == frame_) return;
    scanned_frame_ = frame_;
    core::GuestMemory* gm = core::GuestMemory::current();
    if (!gm) return;
    if (watched_stale_) {
        watched_stale_ = false;
        std::vector<std::pair<uint64_t, uint64_t>> r;  // page-aligned [begin, end)
        const auto add = [&](uint64_t a, uint64_t n) {
            if (n) r.emplace_back(a & ~0xFFFull, std::min<uint64_t>((a + n + 0xFFF) & ~0xFFFull, 1ull << 32));
        };
        for (const auto& [k, t] : textures_) {
            add(t->surf.image, t->surf.image_size);
            if (t->levels > 1) add(t->surf.mipmaps & ~0x700u, t->surf.mip_size);
        }
        std::sort(r.begin(), r.end());
        watched_.clear();
        constexpr uint64_t kGap = 256 << 10;  // join ranges this close: fewer scans, a few extra pages each
        for (size_t i = 0; i < r.size();) {
            uint64_t b = r[i].first, e = r[i].second;
            for (++i; i < r.size() && r[i].first <= e + kGap; ++i) e = std::max(e, r[i].second);
            watched_.push_back({static_cast<uint32_t>(b), static_cast<uint32_t>(e - b)});
        }
        if (debug_ & 16) {  // MH3U_RENDER_DEBUG=16: log the write-watched texture memory
            uint64_t total = 0;
            for (const auto& w : watched_) total += w.size;
            std::printf("write watch: %zu ranges, %llu MB (%zu textures)\n", watched_.size(),
                        static_cast<unsigned long long>(total >> 20), textures_.size());
        }
        gm->set_watched(watched_);
    }
}

Image* VkRenderer::depth_alias(Image& color) {
    // Depth-compare sampling needs a depth format: copy the R32F target into a D32F image through a buffer.
    if (!color.depth_alias)
        color.depth_alias = create_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_TYPE_2D, color.width, color.height, 1, false,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    Image& d = *color.depth_alias;
    // MH3U samples the shadow map in ~100 draws a frame: copy once per change, not per draw (4 MB each)
    if (color.alias_writes == color.writes) return &d;
    color.alias_writes = color.writes;
    end_rendering();
    const Alloc a = alloc(VkDeviceSize{color.width} * color.height * 4, 16);
    transition(color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = a.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {color.width, color.height, 1};
    vkCmdCopyImageToBuffer(cmd(), color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a.buffer, 1, &c);
    VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    bb.srcStageMask = bb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = a.buffer;
    bb.offset = a.offset;
    bb.size = VkDeviceSize{color.width} * color.height * 4;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &bb;
    vkc::cmd_pipeline_barrier2(cmd(), dep);
    transition(d, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyBufferToImage(cmd(), a.buffer, d.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return &d;
}

// A draw that samples the colour target it renders to reads the pre-draw contents (Latte has no feedback hazard
// for the pixel being shaded): sample a copy taken just before the draw.
Image* VkRenderer::snapshot(Image& img) {
    if (!img.snapshot)
        img.snapshot = create_image(img.format, VK_IMAGE_TYPE_2D, img.width, img.height, 1, false,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, img.aspect);
    Image& s = *img.snapshot;
    s.base_swizzle = img.base_swizzle;
    end_rendering();
    transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(s, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = {img.aspect, 0, 0, 1};
    c.dstSubresource = {img.aspect, 0, 0, 1};
    c.extent = {img.width, img.height, 1};
    vkCmdCopyImage(cmd(), img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    return &s;
}

// ---- texture packs (texture_pack.h)------------------------------------------------------------------------------
VkFormat dxgi_format(uint32_t dxgi, bool srgb) {
    switch (dxgi) {
        case 71: case 72: return srgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case 74: case 75: return srgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
        case 77: case 78: return srgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
        case 80: return VK_FORMAT_BC4_UNORM_BLOCK;
        case 81: return VK_FORMAT_BC4_SNORM_BLOCK;
        case 83: return VK_FORMAT_BC5_UNORM_BLOCK;
        case 84: return VK_FORMAT_BC5_SNORM_BLOCK;
        case 98: case 99: return srgb ? VK_FORMAT_BC7_SRGB_BLOCK : VK_FORMAT_BC7_UNORM_BLOCK;
        case 28: case 29: return srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        case 87: case 91: return srgb ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}
// game texture format -> DXGI, for dumps (0: not dumped)
uint32_t dump_dxgi(VkFormat f) {
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return 71;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return 72;
        case VK_FORMAT_BC2_UNORM_BLOCK: return 74;
        case VK_FORMAT_BC2_SRGB_BLOCK: return 75;
        case VK_FORMAT_BC3_UNORM_BLOCK: return 77;
        case VK_FORMAT_BC3_SRGB_BLOCK: return 78;
        case VK_FORMAT_BC4_UNORM_BLOCK: return 80;
        case VK_FORMAT_BC5_UNORM_BLOCK: return 83;
        case VK_FORMAT_R8G8B8A8_UNORM: return 28;
        case VK_FORMAT_R8G8B8A8_SRGB: return 29;
        default: return 0;
    }
}

void VkRenderer::init_packs() {
    char* env = nullptr;
    size_t len = 0;
    std::filesystem::path dir;
    if (_dupenv_s(&env, &len, "MH3U_TEXTURE_DIR") == 0 && env && *env) dir = std::filesystem::path(reinterpret_cast<const char8_t*>(env));
    std::free(env);
    if (_dupenv_s(&env, &len, "MH3U_TEXTURE_PACKS") == 0 && env) want_packs_ = std::atof(env) != 0;
    std::free(env);
    // dumps go next to the packs (<data>/texture_dump) unless MH3U_DUMP_TEXTURES names a folder; on from the start when set
    dump_dir_ = dir.empty() ? std::filesystem::path("texture_dump") : dir.parent_path() / "texture_dump";
    if (_dupenv_s(&env, &len, "MH3U_DUMP_TEXTURES") == 0 && env && *env && std::string(env) != "0") {
        want_dump_ = true;
        if (std::string(env) != "1") dump_dir_ = std::filesystem::path(reinterpret_cast<const char8_t*>(env));
    }
    std::free(env);
    if (!dir.empty()) {
        const auto t0 = std::chrono::steady_clock::now();
        const size_t n = packs_.scan(dir);
        if (n)
            std::printf("renderer: %zu replacement textures in %s (%.0f ms)%s\n", n, reinterpret_cast<const char*>(dir.u8string().c_str()),
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                        want_packs_ ? "" : ", switched off");
    }
    if (want_dump_) std::printf("renderer: dumping textures to %s\n", reinterpret_cast<const char*>(dump_dir_.u8string().c_str()));
}

// After an upload: the texture's level-0 CRC names its replacement (tex_<w>x<h>_<crc>.dds) and its dump file.
void VkRenderer::replace_texture(Image& img, const GuestSurface& s, const FormatDesc& fd, size_t image_bytes,
                                 const std::vector<uint8_t>& level0, uint32_t row_elements) {
    const uint32_t crc = texpack::crc32(base_ + s.image, image_bytes);
    if (dumping_ && !level0.empty()) {
        const uint32_t dxgi = dump_dxgi(fd.vk);
        const uint64_t k = uint64_t{s.width} << 48 | uint64_t{s.height} << 32 | crc;
        if (dxgi && dumped_.insert(k).second) {
            const std::filesystem::path file = dump_dir_ / texpack::file_name(s.width, s.height, crc);
            std::error_code ec;
            if (!std::filesystem::exists(file, ec)) {
                // crop the padded rows to the texture's own size
                const uint32_t ew = fd.block ? (s.width + 3) / 4 : s.width, eh = fd.block ? (s.height + 3) / 4 : s.height;
                std::vector<uint8_t> out(size_t{ew} * eh * fd.bytes);
                for (uint32_t y = 0; y < eh && size_t{y + 1} * row_elements * fd.bytes <= level0.size(); ++y)
                    std::memcpy(&out[size_t{y} * ew * fd.bytes], &level0[size_t{y} * row_elements * fd.bytes], size_t{ew} * fd.bytes);
                if (texpack::write_dds(file, dxgi, s.width, s.height, out.data(), out.size())) ++n_dumped_;
                else warn("can't write texture dumps to " + dump_dir_.string());
            }
        }
    }
    const std::filesystem::path* file = packs_.find(s.width, s.height, crc);
    if (!file) return;
    texpack::Dds dds;
    std::string err;
    if (!texpack::read_dds(*file, dds, &err)) {
        warn(file->filename().string() + ": " + err);
        return;
    }
    const VkFormat vf = dxgi_format(dds.dxgi, (s.format & 0x400) != 0);  // the game's sRGB-ness, whatever the file says
    if (vf == VK_FORMAT_UNDEFINED || !(features(vf) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        warn(file->filename().string() + ": DDS format unsupported on this GPU");
        return;
    }
    auto r = create_image(vf, VK_IMAGE_TYPE_2D, dds.width, dds.height, 1, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_ASPECT_COLOR_BIT, dds.levels);
    if (!r) return;
    const Alloc a = alloc(dds.data.size(), 16);
    std::memcpy(a.ptr, dds.data.data(), dds.data.size());
    std::vector<VkBufferImageCopy> copies;
    for (uint32_t level = 0; level < dds.levels; ++level) {
        VkBufferImageCopy c{};
        c.bufferOffset = a.offset + dds.offsets[level];
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        c.imageExtent = {std::max(1u, dds.width >> level), std::max(1u, dds.height >> level), 1};
        copies.push_back(c);
    }
    transition(*r, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cmd(), a.buffer, r->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(copies.size()),
                           copies.data());
    r->surf = s;
    r->base_swizzle = fd.swizzle;
    img.replacement = std::move(r);
    ++n_replaced_;
}

Image* VkRenderer::texture(const TextureBinding& t, uint32_t sampler_type) {
    const GuestSurface& s = t.surf;
    if (!s.image || !s.width) return nullptr;
    // render-to-texture: sample the cached target
    auto rt = targets_.find(s.image);
    if (rt != targets_.end() && sampler_type != 0 && sampler_type != 4) {
        Image& img = *rt->second;
        if (sampler_type == 6) return img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? &img : depth_alias(img);
        return &img;
    }
    const FormatDesc fd = color_format(s.format);
    if (fd.vk == VK_FORMAT_UNDEFINED || !(features(fd.vk) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        char w[64];
        std::snprintf(w, sizeof w, "texture format 0x%03X unsupported", s.format);
        warn(w);
        return nullptr;
    }
    const bool cube = sampler_type == 4;
    const VkImageType type = sampler_type == 0 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    const uint32_t layers = cube ? 6 : 1;
    const uint32_t levels = texture_levels(s);
    struct {
        uint32_t image, format, w, h, tile, swizzle, pitch, type, mipmaps, levels;
    } k{s.image, s.format, s.width, s.height, s.tile, s.swizzle, s.pitch, sampler_type, s.mipmaps, levels};
    const uint64_t key = fnv(&k, sizeof k);
    auto it = textures_.find(key);
    Image* img = it != textures_.end() ? it->second.get() : nullptr;
    if (!img) {
        auto n = create_image(fd.vk, type, s.width, s.height, layers, cube,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, levels);
        if (!n) return nullptr;
        n->surf = s;
        n->base_swizzle = fd.swizzle;
        img = n.get();
        textures_[key] = std::move(n);
        watched_stale_ = true;
    }
    if (img->checked_frame != frame_) {  // contents may change between frames (CPU-written LUTs etc.)
        img->checked_frame = frame_;
        scan_writes();
        const uint32_t mips = s.mipmaps & ~0x700u;  // bits 8-10: swizzle of a macro-tiled chain
        const size_t image_bytes = std::min<size_t>(s.image_size, (size_t{1} << 32) - s.image);
        const size_t mip_bytes = levels > 1 ? std::min<size_t>(s.mip_size, (size_t{1} << 32) - mips) : 0;
        // hash only memory that was written since the last check (OS write watch): most textures never change
        core::GuestMemory* gm = core::GuestMemory::current();
        const uint32_t epoch = gm ? gm->current_epoch() : 0;  // before the page checks (see current_epoch)
        const bool written = !gm || img->layout == VK_IMAGE_LAYOUT_UNDEFINED ||
                             gm->written_since(s.image, static_cast<uint32_t>(image_bytes), img->seen_epoch) ||
                             (mip_bytes && gm->written_since(mips, static_cast<uint32_t>(mip_bytes), img->seen_epoch));
        img->seen_epoch = epoch;
        uint64_t h = img->hash;
        if (written) {
            ++n_texture_hashes_;
            h = hash_words(base_ + s.image, image_bytes);
            if (mip_bytes) h ^= hash_words(base_ + mips, mip_bytes) * 31;
        }
        if (h != img->hash || img->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
            img->hash = h;
            // packs replace plain 2D textures; one re-uploaded in consecutive frames is CPU-animated: not looked up
            const bool dynamic = img->uploaded_frame + 1 == frame_;
            const bool lookup = (packs_.size() || dumping_) && type == VK_IMAGE_TYPE_2D && !cube && !dynamic;
            std::vector<uint8_t> level0;
            uint32_t row = 0;
            upload_surface(*img, s, fd, type == VK_IMAGE_TYPE_1D ? 1 : layers, lookup && dumping_ ? &level0 : nullptr, &row);
            img->uploaded_frame = frame_;
            if (img->replacement && (dynamic || lookup)) fc().garbage.push_back(std::move(img->replacement));  // may be in flight
            if (lookup) replace_texture(*img, s, fd, image_bytes, level0, row);
        }
    }
    if (img->replacement && want_packs_) return img->replacement.get();
    return img;
}

VkSampler VkRenderer::sampler(const SamplerState& s, bool compare) {
    const auto key = std::make_tuple(s.words[0], s.words[1], s.words[2], compare);
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    // gx2.cpp encoding: w0 clamp x/y/z (3 bits each), border (2), compare func (3), mag (1), min (1), mip (2),
    // aniso (3); w1 min/max LOD in 1/256 (16 bits each); w2 LOD bias (float bits)
    const uint32_t w0 = s.words[0];
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.addressModeU = address_mode(w0 & 7);
    ci.addressModeV = address_mode((w0 >> 3) & 7);
    ci.addressModeW = address_mode((w0 >> 6) & 7);
    const uint32_t border = (w0 >> 9) & 3;
    ci.borderColor = border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                                                                    : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    ci.compareEnable = compare;
    ci.compareOp = static_cast<VkCompareOp>((w0 >> 11) & 7);
    ci.magFilter = (w0 >> 14) & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (w0 >> 15) & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    // GX2TexMipFilterMode: 0 none (level 0 only), 1 point, 2 linear; LODs clamp to the image's levels
    const uint32_t mip = (w0 >> 16) & 3;
    ci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.minLod = mip ? static_cast<float>(s.words[1] >> 16) / 256.0f : 0.0f;
    ci.maxLod = mip ? static_cast<float>(s.words[1] & 0xFFFF) / 256.0f : 0.0f;
    float bias;
    std::memcpy(&bias, &s.words[2], 4);
    ci.mipLodBias = std::isfinite(bias) ? bias : 0.0f;
    VkSampler smp = VK_NULL_HANDLE;
    vkCreateSampler(dev_, &ci, nullptr, &smp);
    samplers_[key] = smp;
    return smp;
}

// ---- shaders --------------------------------------------------------------------------------------------------
ShaderInfo* VkRenderer::shader(int stage, uint32_t addr) {
    if (!addr) return nullptr;
    const uint32_t regs = kRegsBytes[stage];
    const uint32_t size = be32(base_ + addr + regs), prog = be32(base_ + addr + regs + 4);
    auto& slot = shaders_[{stage, addr, prog, size}];
    // same key as tools/shaderc-latte (gfd.cpp shader_key): FNV-1a 64 over regs (as in memory) then program
    if (!slot) slot = shader_by_key(stage, fnv(base_ + prog, size, fnv(base_ + addr, regs)));
    return slot->ok ? slot : nullptr;
}

// The translated shader <key>.<stage>.spv (+ .meta), loaded once per key; never null (ok = false if missing).
ShaderInfo* VkRenderer::shader_by_key(int stage, uint64_t key) {
    auto& slot = shaders_by_key_[{stage, key}];
    if (slot) return slot.get();
    slot = std::make_unique<ShaderInfo>();
    ShaderInfo& s = *slot;
    s.key = key;
    char name[64];
    std::snprintf(name, sizeof name, "%016llx.%s", static_cast<unsigned long long>(s.key), kStageExt[stage]);
    std::ifstream spv(dir_ / (std::string(name) + ".spv"), std::ios::binary);
    if (!spv) {
        warn(std::string("no translated shader ") + name + " (the shader cache is incomplete)");
        return &s;
    }
    const std::vector<char> code((std::istreambuf_iterator<char>(spv)), std::istreambuf_iterator<char>());
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t*>(code.data());
    if (vkCreateShaderModule(dev_, &ci, nullptr, &s.module) != VK_SUCCESS) {
        warn(std::string("bad SPIR-V ") + name);
        return &s;
    }
    std::ifstream meta(dir_ / (std::string(name) + ".meta"));
    for (std::string kind; meta >> kind;) {
        uint32_t a = 0, b = 0;
        if (kind == "ubo" && meta >> a >> b) s.ubos.emplace_back(a, b);
        else if (kind == "sampler" && meta >> a >> b) s.samplers.emplace_back(a, b);
        else if (kind == "input" && meta >> a) s.inputs.push_back(a);
    }
    s.layout = set_layout(&s);
    s.ok = true;
    return &s;
}

VkDescriptorSetLayout VkRenderer::set_layout(const ShaderInfo* s) {
    std::string key;
    std::vector<VkDescriptorSetLayoutBinding> b;
    for (auto [block, n] : s->ubos) {
        key += "u" + std::to_string(block);
        b.push_back({abi::uniform_block_binding(block), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    }
    for (auto [slot, type] : s->samplers) {
        key += "s" + std::to_string(slot);
        b.push_back({abi::sampler_binding(slot), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr});
    }
    auto it = set_layouts_.find(key);
    if (it != set_layouts_.end()) return it->second;
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<uint32_t>(b.size());
    ci.pBindings = b.data();
    VkDescriptorSetLayout l = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(dev_, &ci, nullptr, &l);
    set_layouts_[key] = l;
    return l;
}

VkDescriptorSet VkRenderer::descriptor_set(VkDescriptorSetLayout layout) {
    FrameCtx& f = fc();
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    ai.descriptorPool = f.extra_pools.empty() ? f.pool : f.extra_pools.back();
    if (vkAllocateDescriptorSets(dev_, &ai, &set) != VK_SUCCESS) {
        f.extra_pools.push_back(make_pool(dev_));
        ai.descriptorPool = f.extra_pools.back();
        vkAllocateDescriptorSets(dev_, &ai, &set);
    }
    return set;
}

// ---- pipelines --------------------------------------------------------------------------------------------------
VkPipelineLayout VkRenderer::pipeline_layout(const ShaderInfo* vs, const ShaderInfo* ps, const ShaderInfo* gs) {
    const auto pl_key = std::make_tuple(vs->layout, ps->layout, gs ? gs->layout : empty_layout_);
    VkPipelineLayout& layout = pipe_layouts_[pl_key];
    if (!layout) {
        const VkDescriptorSetLayout set_layouts[3] = {vs->layout, ps->layout, gs ? gs->layout : empty_layout_};
        const VkPushConstantRange ranges[2] = {{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(abi::PixelPushConstants)},
                                               {VK_SHADER_STAGE_VERTEX_BIT, abi::kVertexPushOffset, sizeof(abi::VertexPushConstants)}};
        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 3;
        ci.pSetLayouts = set_layouts;
        ci.pushConstantRangeCount = 2;
        ci.pPushConstantRanges = ranges;
        vkCreatePipelineLayout(dev_, &ci, nullptr, &layout);
    }
    return layout;
}

// Thread-safe (precompile() calls it from several threads): reads only the description, the modules and the device.
VkPipeline VkRenderer::create_pipeline(const PipelineDesc& d, VkPipelineLayout layout, const ShaderInfo* const stages[3]) const {
    VkPipelineShaderStageCreateInfo stg[3] = {};
    uint32_t nst = 0;
    for (int s = 0; s < 3; ++s) {
        if (!stages[s]) continue;
        stg[nst] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stg[nst].stage = s == 0 ? VK_SHADER_STAGE_VERTEX_BIT : s == 1 ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_GEOMETRY_BIT;
        stg[nst].module = stages[s]->module;
        stg[nst].pName = "main";
        ++nst;
    }
    VkVertexInputBindingDescription bindings[2];
    VkVertexInputAttributeDescription attrs[2 * PipelineDesc::kMaxAttribs];
    uint32_t nb = 0, na = 0;
    if (d.n_loc_v) bindings[nb++] = {0, d.n_loc_v * 16, VK_VERTEX_INPUT_RATE_VERTEX};
    if (d.n_loc_i) bindings[nb++] = {1, d.n_loc_i * 16, VK_VERTEX_INPUT_RATE_INSTANCE};
    for (uint32_t i = 0; i < d.n_loc_v; ++i) attrs[na++] = {d.loc_v[i], 0, VK_FORMAT_R32G32B32A32_UINT, i * 16};
    for (uint32_t i = 0; i < d.n_loc_i; ++i) attrs[na++] = {d.loc_i[i], 1, VK_FORMAT_R32G32B32A32_UINT, i * 16};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = bindings;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attrs;
    const auto topo = static_cast<VkPrimitiveTopology>(d.topology);
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo;
    ia.primitiveRestartEnable = topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topo == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP ||
                                topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    VkPipelineViewportDepthClipControlCreateInfoEXT clip{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    clip.negativeOneToOne = VK_TRUE;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    if (d.neg_one) vp.pNext = &clip;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = (d.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (d.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
    rs.frontFace = d.front_face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    const bool depth = d.depth_format != VK_FORMAT_UNDEFINED;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = depth && d.depth_test;
    ds.depthWriteEnable = depth && d.depth_write;
    ds.depthCompareOp = static_cast<VkCompareOp>(d.depth_func & 7);
    ds.stencilTestEnable = d.stencil;
    const auto op_state = [](const uint32_t f[4]) {
        VkStencilOpState o{};
        o.compareOp = static_cast<VkCompareOp>(f[0]);
        o.passOp = static_cast<VkStencilOp>(f[1]);
        o.depthFailOp = static_cast<VkStencilOp>(f[2]);
        o.failOp = static_cast<VkStencilOp>(f[3]);
        return o;
    };
    ds.front = op_state(d.stencil_ops[0]);
    ds.back = op_state(d.stencil_ops[1]);
    VkPipelineColorBlendAttachmentState att[8] = {};
    VkFormat color_formats[8] = {};
    for (uint32_t t = 0; t < d.ncolor; ++t) {
        const uint32_t* b = d.blend[t];
        const bool written = d.color_format[t] != VK_FORMAT_UNDEFINED;
        color_formats[t] = static_cast<VkFormat>(d.color_format[t]);
        att[t].blendEnable = written && !(debug_ & 2) && d.blend_enable[t] &&
                             (features(color_formats[t]) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT);
        att[t].srcColorBlendFactor = blend_factor(b[0]);
        att[t].dstColorBlendFactor = blend_factor(b[1]);
        att[t].colorBlendOp = blend_op(b[2]);
        att[t].srcAlphaBlendFactor = blend_factor(d.separate_alpha[t] ? b[3] : b[0]);
        att[t].dstAlphaBlendFactor = blend_factor(d.separate_alpha[t] ? b[4] : b[1]);
        att[t].alphaBlendOp = blend_op(d.separate_alpha[t] ? b[5] : b[2]);
        att[t].colorWriteMask = written ? d.write_mask[t] & 0xF : 0;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = d.ncolor;
    cb.pAttachments = att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                                  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                  VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = static_cast<uint32_t>(std::size(dyn));
    dy.pDynamicStates = dyn;
    const auto depth_format = static_cast<VkFormat>(d.depth_format);
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = d.ncolor;
    ri.pColorAttachmentFormats = color_formats;
    ri.depthAttachmentFormat = depth_format;
    ri.stencilAttachmentFormat = depth && has_stencil(depth_format) ? depth_format : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &ri;
    pci.stageCount = nst;
    pci.pStages = stg;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dy;
    pci.layout = layout;
    vkc::pipeline_rendering(pci, ri);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &pci, nullptr, &pipe) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pipe;
}

namespace {
constexpr char kPipelineListMagic[8] = {'M', 'H', '3', 'U', 'P', 'I', 'P', 'E'};
}  // namespace

void VkRenderer::record_pipeline(const std::string& key) {
    if (cache_dir_.empty()) return;
    const auto path = cache_dir_ / "pipelines.bin";
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(path, ec);
    std::ofstream f(path, std::ios::binary | std::ios::app);
    if (fresh) {
        f.write(kPipelineListMagic, sizeof kPipelineListMagic);
        f.write(reinterpret_cast<const char*>(&kPipelineListVersion), 4);
    }
    const auto n = static_cast<uint32_t>(key.size());
    f.write(reinterpret_cast<const char*>(&n), 4);
    f.write(key.data(), static_cast<std::streamsize>(key.size()));
}

size_t VkRenderer::precompile(const std::function<void(size_t, size_t)>& progress) {
    if (cache_dir_.empty()) return 0;
    std::ifstream f(cache_dir_ / "pipelines.bin", std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    uint32_t version = 0;
    if (data.size() < 12 || std::memcmp(data.data(), kPipelineListMagic, 8) != 0) return 0;
    std::memcpy(&version, data.data() + 8, 4);
    if (version != kPipelineListVersion) {  // made by another version of the port: start a new list
        std::error_code ec;
        std::filesystem::remove(cache_dir_ / "pipelines.bin", ec);
        return 0;
    }
    // records -> descriptions with their shaders and layouts (serial: these fill the renderer's maps)
    struct Job {
        std::string key;
        PipelineDesc desc;
        const ShaderInfo* stages[3] = {};
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipe = VK_NULL_HANDLE;
    };
    std::vector<Job> jobs;
    size_t bad = 0;
    for (size_t p = 12; p + 4 <= data.size();) {
        uint32_t n = 0;
        std::memcpy(&n, data.data() + p, 4);
        p += 4;
        if (p + n > data.size()) break;
        Job j;
        j.key.assign(data.data() + p, n);
        p += n;
        if (pipelines_.count(j.key)) continue;  // duplicate record
        KeyReader r{j.key.data(), j.key.size()};
        bool ok = serialize(r, j.desc);
        for (int s = 0; ok && s < 3; ++s) {
            if (s == 2 && !j.desc.shader[2]) break;
            const ShaderInfo* sh = shader_by_key(s, j.desc.shader[s]);
            ok = sh->ok;
            j.stages[s] = sh;
        }
        if (!ok) {
            ++bad;
            continue;
        }
        j.layout = pipeline_layout(j.stages[0], j.stages[1], j.stages[2]);
        pipelines_[j.key] = VK_NULL_HANDLE;  // reserve: duplicates later in the file are skipped
        jobs.push_back(std::move(j));
    }
    // vkCreateGraphicsPipelines on several threads (the pipeline cache is internally synchronised)
    std::atomic<size_t> next{0}, done{0};
    std::vector<std::thread> pool;
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency() - 1);
    for (unsigned t = 0; t < threads; ++t)
        pool.emplace_back([&] {
            for (size_t i; (i = next++) < jobs.size(); ++done) jobs[i].pipe = create_pipeline(jobs[i].desc, jobs[i].layout, jobs[i].stages);
        });
    while (done < jobs.size()) {
        if (progress) progress(done, jobs.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    for (std::thread& t : pool) t.join();
    size_t built = 0;
    for (Job& j : jobs) {
        if (j.pipe) ++built;
        pipelines_[j.key] = j.pipe;  // a failed one stays null and is retried (and reported) by draw()
    }
    if (progress) progress(jobs.size(), jobs.size());
    if (bad) warn(std::to_string(bad) + " recorded pipelines refer to shaders that are not in the cache");
    pipelines_saved_ = pipelines_.size();
    return built;
}

void VkRenderer::save_caches() {
    if (cache_dir_.empty() || !pipeline_cache_ || pipelines_.size() == pipelines_saved_) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(dev_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size) return;
    std::vector<char> blob(size);
    if (vkGetPipelineCacheData(dev_, pipeline_cache_, &size, blob.data()) != VK_SUCCESS) return;
    const auto tmp = cache_dir_ / "pipeline_cache.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(blob.data(), static_cast<std::streamsize>(size));
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, cache_dir_ / "pipeline_cache.bin", ec);  // replace atomically
    pipelines_saved_ = pipelines_.size();
}

// ---- GPU work ---------------------------------------------------------------------------------------------------
bool VkRenderer::clear_color(const GuestSurface& s, const float rgba[4]) {
    Image* img = target(s, false, false);
    if (!img) return false;
    transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue c;
    std::memcpy(c.float32, rgba, 16);
    const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd(), img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
    return true;
}

bool VkRenderer::clear_depth_stencil(const GuestSurface& s, float depth, uint32_t stencil, uint32_t flags) {
    Image* img = target(s, true, false);
    if (!img) return false;
    VkImageAspectFlags aspect = 0;
    if (flags & 1) aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (flags & 2) aspect |= img->aspect & VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspect) return true;
    transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue v{depth, stencil};
    const VkImageSubresourceRange r{aspect, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd(), img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
    return true;
}

bool VkRenderer::copy_surface(const GuestSurface& src, uint32_t src_slice, const GuestSurface& dst, uint32_t dst_slice) {
    auto it = targets_.find(src.image);
    if (it == targets_.end() || src_slice || dst_slice) {
        // the CPU copy rewrites guest memory: retire a stale GPU copy of dst (after this frame's commands)
        auto d = targets_.find(dst.image);
        if (d != targets_.end()) {
            fc().garbage.push_back(std::move(d->second));
            targets_.erase(d);
        }
        return false;
    }
    Image& a = *it->second;
    end_rendering();
    const bool depth = (a.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    Image* b = target(dst, depth, false);
    if (!b) return false;
    transition(a, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(*b, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (a.format == b->format && a.width == b->width && a.height == b->height) {
        VkImageCopy c{};
        c.srcSubresource = {a.aspect, 0, 0, 1};
        c.dstSubresource = {b->aspect, 0, 0, 1};
        c.extent = {a.width, a.height, 1};
        vkCmdCopyImage(cmd(), a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    } else if (!depth) {
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {static_cast<int32_t>(a.width), static_cast<int32_t>(a.height), 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[1] = {static_cast<int32_t>(b->width), static_cast<int32_t>(b->height), 1};
        vkCmdBlitImage(cmd(), a.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    }
    return true;
}

bool VkRenderer::scan_out(const GuestSurface& s, bool gamepad) {
    auto it = targets_.find(s.image);
    if (it == targets_.end()) return false;
    Image& src = *it->second;
    if (!gamepad) {
        tv_guest_w_ = s.width;
        tv_guest_h_ = s.height;
    }
    std::unique_ptr<Image>& out = gamepad ? drc_ : tv_;
    if (!out || out->width != src.width || out->height != src.height) {
        if (out) fc().garbage.push_back(std::move(out));
        out = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, src.width, src.height, 1, false,
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT);  // sampled: the look post pass reads it
        if (!out) return false;
    }
    end_rendering();
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(*out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {static_cast<int32_t>(src.width), static_cast<int32_t>(src.height), 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[1] = bl.srcOffsets[1];
    vkCmdBlitImage(cmd(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    transition(*out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    (gamepad ? drc_written_ : tv_written_) = true;
    return true;
}

void VkRenderer::draw(const DrawState& st, const DrawCall& call) {
    if (!call.count || !call.instances) return;
    if ((ao_ || dof_ || gi_ || tri_) && !scene_done_) scene_hook(st);
    // the vertex/index caches point into the vertex arena: bound them here, before any entry is in use
    if (varena_bytes_ > (256ull << 20) || vcache_bytes_ > (512ull << 20) || icache_.size() > 65536)  // bound: start over
        retire_vertex_arena();
    ShaderInfo* vs = shader(kStageVertex, st.vs);
    ShaderInfo* ps = shader(kStagePixel, st.ps);
    ShaderInfo* gs = st.gs ? shader(kStageGeometry, st.gs) : nullptr;
    if (!vs || !ps || (st.gs && !gs)) return skip("shader missing");
    if ((debug_ & 8) && !gs) return skip("MH3U_RENDER_DEBUG: non-GS draw");

    // ---- vertex range and indices
    std::vector<uint32_t>& indices = scratch_indices_;  // member buffers: no allocation per draw
    indices.clear();
    uint32_t vmin = call.first, vmax = call.first + call.count - 1;
    // Indexed draws: indices rebased to the vertex range, cached like vertex data (by index buffer, while the raw bytes
    // hash the same) and kept resident when static. Modes that rewrite the index list take a copy instead.
    const bool rewrites = call.mode == 0x11 || call.mode == 0x12 || call.mode == 0x13 || call.mode == 0x14;
    IndexCacheEntry* ientry = nullptr;
    if (call.indexed) {
        const bool u32 = call.index_type == 1 || call.index_type == 9, be = call.index_type >= 4;
        const uint8_t* p = base_ + call.index_addr;
        const uint32_t kw[3] = {call.index_addr, call.count, call.index_type};
        const uint64_t key = hash_words(reinterpret_cast<const uint8_t*>(kw), sizeof kw);
        const uint64_t src = hash_words(p, size_t{call.count} * (u32 ? 4 : 2));
        IndexCacheEntry& ie = icache_[key];
        if (!ie.valid || ie.src != src) {
            ++n_index_converts_;
            if (ie.valid && ie.converted_frame + 2 >= frame_) ie.dynamic = true;  // changes every frame or so
            ie.converted_frame = frame_;
            ie.valid = true;
            ie.src = src;
            ie.gpu = VK_NULL_HANDLE;
            ie.indices.resize(call.count);
            ie.vmin = UINT32_MAX;
            ie.vmax = 0;
            // all-ones index = primitive restart (GX2's default restart index); kept as 0xFFFFFFFF for Vulkan
            const uint32_t restart = u32 ? 0xFFFFFFFFu : 0xFFFFu;
            for (uint32_t i = 0; i < call.count; ++i) {
                uint32_t v = u32 ? (be ? be32(p + 4 * i) : le32(p + 4 * i)) : (be ? be16(p + 2 * i) : static_cast<uint32_t>(p[2 * i] | p[2 * i + 1] << 8));
                if (v == restart) {
                    ie.indices[i] = UINT32_MAX;
                    continue;
                }
                ie.indices[i] = v;
                ie.vmin = std::min(ie.vmin, v);
                ie.vmax = std::max(ie.vmax, v);
            }
            if (ie.vmin <= ie.vmax)
                for (uint32_t& v : ie.indices)
                    if (v != UINT32_MAX) v -= ie.vmin;
        }
        if (ie.vmin > ie.vmax) return;  // only restarts
        vmin = ie.vmin + call.base_vertex;
        vmax = ie.vmax + call.base_vertex;
        if (rewrites) indices.assign(ie.indices.begin(), ie.indices.end());
        else ientry = &ie;
    }
    if (vmax - vmin > (1u << 22)) return skip("vertex range too large");
    const uint32_t nverts = vmax - vmin + 1;
    // QUADS / QUAD_STRIP / LINE_LOOP -> index lists
    if (call.mode == 0x13 || call.mode == 0x14 || call.mode == 0x12) {
        std::vector<uint32_t> src = indices;
        if (src.empty())
            for (uint32_t i = 0; i < call.count; ++i) src.push_back(i);
        indices.clear();
        if (call.mode == 0x13)
            for (size_t q = 0; q + 3 < src.size(); q += 4)
                indices.insert(indices.end(), {src[q], src[q + 1], src[q + 2], src[q], src[q + 2], src[q + 3]});
        else if (call.mode == 0x14)
            for (size_t q = 0; q + 3 < src.size(); q += 2)
                indices.insert(indices.end(), {src[q], src[q + 1], src[q + 2], src[q + 2], src[q + 1], src[q + 3]});
        else {
            indices = src;
            indices.push_back(src[0]);
        }
    }

    // ---- vertex attributes (GPR values per VS input semantic)
    std::vector<uint32_t>& loc_v = scratch_loc_v_;
    std::vector<uint32_t>& loc_i = scratch_loc_i_;
    std::vector<const AttribStream*>& str_v = scratch_str_v_;
    std::vector<const AttribStream*>& str_i = scratch_str_i_;
    loc_v.clear();
    loc_i.clear();
    str_v.clear();
    str_i.clear();
    for (uint32_t sem : vs->inputs) {
        const AttribStream* found = nullptr;
        for (const AttribStream& a : st.streams)
            if (a.location == sem) found = &a;
        if (found && found->type == 1) {
            loc_i.push_back(sem);
            str_i.push_back(found);
        } else {
            loc_v.push_back(sem);
            str_v.push_back(found);
        }
    }
    // Vertex data is expanded to one vec4 per attribute. Most of it is static (models), so conversions are cached by
    // stream layout + vertex range and reused while a hash of the raw guest bytes they came from is unchanged. Static
    // entries live in the device-local vertex arena and are bound directly; entries whose source changes on
    // consecutive frames (particles, UI) are streamed through the frame's upload memory. need_ptr: the caller reads
    // the converted vertices back (RECT expansion), so they must be in mapped memory.
    const auto fill = [&](const std::vector<const AttribStream*>& streams, uint32_t count, bool instanced, bool need_ptr,
                          Alloc& out) {
        const size_t stride = streams.size() * 16;
        const size_t bytes = std::max<size_t>(16, stride * count);
        const auto stream = [&](const uint8_t* data) {
            bytes_vertex_ += bytes;
            out = alloc(bytes, 16);
            std::memcpy(out.ptr, data, bytes);
        };
        // key: layout and range; source: the bytes of each buffer the streams read
        std::vector<uint32_t>& kw = scratch_key_;  // key fields as words, hashed at once
        kw.assign({count, instanced ? 1u : 0u, vmin});
        uint64_t lo[16], hi[16];
        std::fill(std::begin(lo), std::end(lo), UINT64_MAX);
        std::fill(std::begin(hi), std::end(hi), 0);
        for (const AttribStream* s : streams) {
            if (!s) {
                kw.push_back(~0u);
                continue;
            }
            kw.insert(kw.end(), {s->location, s->buffer, s->offset, s->format, s->type, s->divisor, s->mask, s->endian});
            if (s->buffer >= 16) continue;
            const AttribBuffer& buf = st.attribs[s->buffer];
            kw.insert(kw.end(), {buf.addr, buf.size, buf.stride});
            if (!buf.addr) continue;
            const uint64_t first = instanced ? 0 : vmin, last = instanced ? (count - 1) / std::max(1u, s->divisor) : vmin + count - 1;
            uint64_t b = buf.addr + first * buf.stride + s->offset, e = buf.addr + last * buf.stride + s->offset + attrib_bytes(s->format & 0xFF);
            if (buf.size) e = std::min<uint64_t>(e, uint64_t{buf.addr} + buf.size);
            e = std::min<uint64_t>(e, 1ull << 32);
            lo[s->buffer] = std::min(lo[s->buffer], b);
            hi[s->buffer] = std::max(hi[s->buffer], e);
        }
        uint64_t src = 0;
        for (int b = 0; b < 16; ++b)
            if (hi[b] > lo[b]) src = src * 31 + hash_words(base_ + lo[b], static_cast<size_t>(hi[b] - lo[b]));
        const uint64_t key = hash_words(reinterpret_cast<const uint8_t*>(kw.data()), kw.size() * 4);
        VertexCacheEntry& ce = vcache_[key];
        if (ce.data.size() == bytes && ce.src == src) {
            if (ce.gpu && !need_ptr) out = {ce.gpu, ce.gpu_offset, nullptr};
            else stream(ce.data.data());
            return;
        }
        ++n_vertex_converts_;
        if (!ce.data.empty() && ce.converted_frame + 2 >= frame_) ce.dynamic = true;  // changes every frame or so
        ce.converted_frame = frame_;
        ce.gpu = VK_NULL_HANDLE;
        vcache_bytes_ += bytes - std::min(bytes, ce.data.size());
        ce.src = src;
        ce.data.assign(bytes, 0);
        uint8_t* const dst_base = ce.data.data();
        for (size_t a = 0; a < streams.size(); ++a) {
            const AttribStream* s = streams[a];
            if (!s || s->buffer >= 16) continue;
            const AttribBuffer& buf = st.attribs[s->buffer];
            if (!buf.addr) continue;
            const uint32_t esize = attrib_bytes(s->format & 0xFF);
            for (uint32_t v = 0; v < count; ++v) {
                const uint32_t elem = instanced ? v / std::max(1u, s->divisor) : vmin + v;
                const uint64_t off = uint64_t{elem} * buf.stride + s->offset;
                if (buf.size && off + esize > buf.size) continue;
                uint32_t comps[4];
                decode_attrib(base_ + buf.addr + off, s->format, s->endian, comps);
                uint32_t* dst = reinterpret_cast<uint32_t*>(dst_base + v * stride + a * 16);
                for (int c = 0; c < 4; ++c) {
                    const uint32_t sel = (s->mask >> (24 - 8 * c)) & 0xFF;
                    const bool integer = (s->format & 0x100) != 0;
                    dst[c] = sel < 4 ? comps[sel] : sel == 5 ? (integer ? 1u : 0x3F800000u) : 0u;
                }
            }
        }
        if (ce.dynamic || need_ptr) {
            stream(dst_base);
            return;
        }
        out = upload_static(dst_base, bytes, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        ce.gpu = out.buffer;
        ce.gpu_offset = out.offset;
    };
    Alloc vb{}, ib{}, idx{};
    fill(str_v, nverts, false, call.mode == 0x11 || trace_on_, vb);  // RECT expansion and the trace read vb.ptr
    // RECTS: three corners per rectangle; the fourth (v1 + v2 - v0) is derived from the decoded float attributes,
    // exact when the vertex shader is affine in its inputs (full-screen / sprite passes), then two triangles.
    if (call.mode == 0x11) {
        std::vector<uint32_t> src = indices;
        if (src.empty())
            for (uint32_t i = 0; i < call.count; ++i) src.push_back(i);
        const size_t stride = loc_v.size() * 16, rects = src.size() / 3;
        Alloc nb = alloc(std::max<size_t>(16, stride * (nverts + rects)), 16);
        std::memcpy(nb.ptr, vb.ptr, stride * nverts);
        indices.clear();
        for (size_t r = 0; r < rects; ++r) {
            const uint32_t a = src[3 * r], b = src[3 * r + 1], c = src[3 * r + 2], d = nverts + static_cast<uint32_t>(r);
            if (a == UINT32_MAX || b == UINT32_MAX || c == UINT32_MAX) continue;
            for (size_t w = 0; w < stride / 4; ++w) {
                float fa, fb, fc;
                std::memcpy(&fa, nb.ptr + a * stride + 4 * w, 4);
                std::memcpy(&fb, nb.ptr + b * stride + 4 * w, 4);
                std::memcpy(&fc, nb.ptr + c * stride + 4 * w, 4);
                const float fd = fb + fc - fa;
                std::memcpy(nb.ptr + d * stride + 4 * w, &fd, 4);
            }
            indices.insert(indices.end(), {a, b, c, b, d, c});
        }
        vb = nb;
        if (indices.empty()) return;
    }
    if (trace_on_) {
        tracef("draw %llu: mode %X count %u inst %u vs %016llx ps %016llx gs %016llx verts %u..%u%s\n",
               static_cast<unsigned long long>(n_draws_ + n_skipped_), call.mode, call.count, call.instances,
               static_cast<unsigned long long>(vs->key), static_cast<unsigned long long>(ps->key),
               static_cast<unsigned long long>(gs ? gs->key : 0), vmin, vmax, call.indexed ? " indexed" : "");
        for (size_t a = 0; a < loc_v.size(); ++a) {
            const AttribStream* s = str_v[a];
            if (!s) {
                tracef("  attr sem %u: no stream\n", loc_v[a]);
                continue;
            }
            const AttribBuffer& b = st.attribs[s->buffer & 15];
            tracef("  attr sem %u: buf %u (%08X size %u stride %u) off %u fmt %03X type %u mask %08X endian %u:", loc_v[a],
                   s->buffer, b.addr, b.size, b.stride, s->offset, s->format, s->type, s->mask, s->endian);
            for (uint32_t v = 0; v < std::min(nverts, 4u); ++v) {
                const uint32_t* d = reinterpret_cast<const uint32_t*>(vb.ptr + v * loc_v.size() * 16 + a * 16);
                float f[4];
                std::memcpy(f, d, 16);
                tracef(" (%g %g %g %g)", f[0], f[1], f[2], f[3]);
            }
            tracef("\n");
        }
        for (uint32_t t = 0; t < 8; ++t)
            if (st.color_bound[t])
                tracef("  colour %u: image %08X %ux%u fmt %03X written %X\n", t, st.color[t].image, st.color[t].width,
                       st.color[t].height, st.color[t].format, (be32(base_ + st.ps + 0x94) >> (4 * t)) & 0xF);
        tracef("  stencil %d back %d func %u/%u ops %u%u%u ref %u mask %X/%X\n", st.stencil_test, st.back_stencil,
               st.stencil_front.func, st.stencil_back.func, st.stencil_front.fail, st.stencil_front.depth_fail,
               st.stencil_front.pass, st.stencil_front.ref, st.stencil_front.compare_mask, st.stencil_front.write_mask);
        tracef("  depth %s test %d write %d func %u; blend mask %X; cull %d/%d ff %u; viewport %g,%g %gx%g %g..%g; alpha %d %u %g\n",
               st.depth_bound ? "bound" : "none", st.depth_test, st.depth_write, st.depth_func, st.blend_enable_mask,
               st.cull_front, st.cull_back, st.front_face, st.viewport[0], st.viewport[1], st.viewport[2], st.viewport[3],
               st.viewport[4], st.viewport[5], st.alpha_test, st.alpha_func, st.alpha_ref);
        ShaderInfo* sh[3] = {vs, ps, gs};
        for (int s = 0; s < 3; ++s) {
            if (!sh[s]) continue;
            for (auto [slot, type] : sh[s]->samplers) {
                const TextureBinding& tb = st.stage[s].tex[slot];
                tracef("  %s tex %u (type %u): %s image %08X %ux%u fmt %03X tile %u compsel %08X%s\n", kStageExt[s], slot, type,
                       tb.bound ? "bound" : "unbound", tb.surf.image, tb.surf.width, tb.surf.height, tb.surf.format, tb.surf.tile,
                       tb.comp_sel, targets_.count(tb.surf.image) ? " (render target)" : "");
            }
            for (auto [block, n] : sh[s]->ubos) {
                const UniformBinding& u = st.stage[s].ubo[block];
                tracef("  %s ubo %u: %08X size %u (shader reads %u vec4):", kStageExt[s], block, u.addr, u.size, n);
                for (uint32_t w = 0; w < std::min(u.size / 4, 16u) && u.addr; ++w) {
                    const uint32_t v = le32(base_ + u.addr + 4 * w);
                    float f;
                    std::memcpy(&f, &v, 4);
                    tracef(" %g", f);
                }
                tracef("\n");
            }
        }
    }
    if (!loc_i.empty()) fill(str_i, call.instances, true, false, ib);
    uint32_t index_count = 0;
    if (ientry) {
        const size_t bytes = ientry->indices.size() * 4;
        index_count = static_cast<uint32_t>(ientry->indices.size());
        if (ientry->gpu) {
            idx = {ientry->gpu, ientry->gpu_offset, nullptr};
        } else if (ientry->dynamic) {
            idx = alloc(bytes, 4);
            std::memcpy(idx.ptr, ientry->indices.data(), bytes);
        } else {
            idx = upload_static(ientry->indices.data(), bytes, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
            ientry->gpu = idx.buffer;
            ientry->gpu_offset = idx.offset;
        }
    } else if (!indices.empty()) {
        index_count = static_cast<uint32_t>(indices.size());
        idx = alloc(indices.size() * 4, 4);
        std::memcpy(idx.ptr, indices.data(), indices.size() * 4);
    }

    // ---- render targets
    Image* colors[8] = {};
    uint32_t ps_mask = be32(base_ + st.ps + 0x94);  // cb_shader_mask: 4 bits per written target
    VkFormat color_formats[8] = {};
    uint32_t ncolor = 0, rw = UINT32_MAX, rh = UINT32_MAX;
    for (uint32_t t = 0; t < 8; ++t) {
        if (!st.color_bound[t] || !((ps_mask >> (4 * t)) & 0xF)) continue;
        colors[t] = target(st.color[t], false, true);
        if (!colors[t]) continue;
        color_formats[t] = colors[t]->format;
        ncolor = t + 1;
        rw = std::min(rw, colors[t]->width);
        rh = std::min(rh, colors[t]->height);
    }
    Image* depth = (st.depth_bound && (st.depth_test || st.depth_write || st.stencil_test)) ? target(st.depth, true, false) : nullptr;
    const bool stencil = depth && st.stencil_test && has_stencil(depth->format);
    const StencilFace& sback = st.back_stencil ? st.stencil_back : st.stencil_front;
    if (depth) {
        rw = std::min(rw, depth->width);
        rh = std::min(rh, depth->height);
    }
    if (!ncolor && !depth) return skip("no render target");

    // ---- descriptors (uploads and layout transitions happen before rendering starts)
    ShaderInfo* stages[3] = {vs, ps, gs};
    VkDescriptorSet sets[3] = {};
    for (int s = 0; s < 3; ++s) {
        ShaderInfo* sh = stages[s];
        if (!sh || (sh->ubos.empty() && sh->samplers.empty())) continue;
        sets[s] = descriptor_set(sh->layout);
        std::vector<VkDescriptorBufferInfo>& bufs = scratch_bufs_;
        std::vector<VkDescriptorImageInfo>& imgs = scratch_imgs_;
        std::vector<VkWriteDescriptorSet>& writes = scratch_writes_;
        bufs.assign(sh->ubos.size(), {});
        imgs.assign(sh->samplers.size(), {});
        writes.clear();
        for (size_t i = 0; i < sh->ubos.size(); ++i) {
            const auto [block, vec4s] = sh->ubos[i];
            const UniformBinding& u = st.stage[s].ubo[block];
            const uint32_t bytes = std::max<uint32_t>(vec4s * 16, (u.size + 15) & ~15u);
            bytes_ubo_ += bytes;
            const Alloc a = alloc(bytes, ubo_align_);
            std::memset(a.ptr, 0, bytes);
            if (u.addr) std::memcpy(a.ptr, base_ + u.addr, u.size);  // already GPU (little-endian) word order
            bufs[i] = {a.buffer, a.offset, bytes};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[s];
            w.dstBinding = abi::uniform_block_binding(block);
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &bufs[i];
            writes.push_back(w);
        }
        for (size_t i = 0; i < sh->samplers.size(); ++i) {
            const auto [slot, type] = sh->samplers[i];
            const TextureBinding& tb = st.stage[s].tex[slot];
            Image* img = tb.bound ? texture(tb, type) : nullptr;
            if (img && (img == depth || std::find(colors, colors + 8, img) != colors + 8)) img = snapshot(*img);
            const bool shadow = type == 6;
            VkImageViewType vt = type == 0 ? VK_IMAGE_VIEW_TYPE_1D : type == 4 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
            VkComponentMapping swz{};
            VkImageAspectFlags aspect = 0;
            if (!img) {
                img = shadow ? dummy_shadow_.get() : dummy_[type == 0 ? 0 : type == 4 ? 2 : 1].get();
            } else if (shadow) {
                aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
            } else {
                swz.r = compose(img->base_swizzle, comp_sel((tb.comp_sel >> 24) & 0xFF, VK_COMPONENT_SWIZZLE_R));
                swz.g = compose(img->base_swizzle, comp_sel((tb.comp_sel >> 16) & 0xFF, VK_COMPONENT_SWIZZLE_G));
                swz.b = compose(img->base_swizzle, comp_sel((tb.comp_sel >> 8) & 0xFF, VK_COMPONENT_SWIZZLE_B));
                swz.a = compose(img->base_swizzle, comp_sel(tb.comp_sel & 0xFF, VK_COMPONENT_SWIZZLE_A));
            }
            if (img->type == VK_IMAGE_TYPE_1D) vt = VK_IMAGE_VIEW_TYPE_1D;
            else if (vt == VK_IMAGE_VIEW_TYPE_1D) vt = VK_IMAGE_VIEW_TYPE_2D;  // mismatched target: avoid an invalid view
            if (vt == VK_IMAGE_VIEW_TYPE_CUBE && !img->cube) vt = VK_IMAGE_VIEW_TYPE_2D;
            transition(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            imgs[i] = {sampler(st.stage[s].samp[slot], shadow), view(*img, vt, swz, aspect), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[s];
            w.dstBinding = abi::sampler_binding(slot);
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &imgs[i];
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(dev_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // ---- pipeline: described by PipelineDesc, cached by its encoding (built every draw into reused buffers)
    if (loc_v.size() > PipelineDesc::kMaxAttribs || loc_i.size() > PipelineDesc::kMaxAttribs) return skip("too many attributes");
    PipelineDesc& d = pipe_desc_;
    d = PipelineDesc{};
    d.shader[0] = vs->key;
    d.shader[1] = ps->key;
    d.shader[2] = gs ? gs->key : 0;
    d.topology = gs ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : topology(call.mode);
    d.neg_one = vk_.depth_clip_control && !st.half_z;
    d.n_loc_v = static_cast<uint32_t>(loc_v.size());
    d.n_loc_i = static_cast<uint32_t>(loc_i.size());
    std::copy(loc_v.begin(), loc_v.end(), d.loc_v);
    std::copy(loc_i.begin(), loc_i.end(), d.loc_i);
    d.ncolor = ncolor;
    for (uint32_t t = 0; t < ncolor; ++t) {
        const BlendTarget& b = st.blend[t];
        d.color_format[t] = color_formats[t];
        d.blend_enable[t] = (st.blend_enable_mask >> t) & 1;
        const uint32_t bv[6] = {b.color_src, b.color_dst, b.color_op, b.alpha_src, b.alpha_dst, b.alpha_op};
        std::copy(bv, bv + 6, d.blend[t]);
        d.separate_alpha[t] = b.separate_alpha;
        d.write_mask[t] = st.color_write ? st.write_mask[t] : 0u;
    }
    d.depth_format = depth ? depth->format : VK_FORMAT_UNDEFINED;
    d.depth_test = st.depth_test;
    d.depth_write = st.depth_write;
    d.depth_func = st.depth_func;
    d.stencil = stencil;
    if (stencil) {
        const StencilFace* faces[2] = {&st.stencil_front, &sback};
        for (int f = 0; f < 2; ++f) {
            const uint32_t ops[4] = {faces[f]->func, faces[f]->pass, faces[f]->depth_fail, faces[f]->fail};
            std::copy(ops, ops + 4, d.stencil_ops[f]);
        }
    }
    d.cull_front = st.cull_front;
    d.cull_back = st.cull_back;
    d.front_face = st.front_face;
    std::string& key = pipe_key_;
    key.clear();
    KeyWriter kw{key};
    serialize(kw, d);
    const VkPipelineLayout layout = pipeline_layout(vs, ps, gs);
    VkPipeline& pipe = pipelines_[key];
    if (!pipe) {
        pipe = create_pipeline(d, layout, stages);
        if (!pipe) return skip("pipeline creation failed");
        record_pipeline(key);
    }

    // ---- rendering: restart only when the attachments change
    bool same = rendering_ && active_depth_ == depth && active_color_.size() == ncolor;
    for (uint32_t t = 0; same && t < ncolor; ++t) same = active_color_[t] == colors[t];
    if (!same) {
        end_rendering();
        for (uint32_t t = 0; t < ncolor; ++t)
            if (colors[t]) transition(*colors[t], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (depth) transition(*depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca[8] = {};
        for (uint32_t t = 0; t < ncolor; ++t) {
            ca[t] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (!colors[t]) continue;
            ca[t].imageView = view(*colors[t], VK_IMAGE_VIEW_TYPE_2D, {});
            ca[t].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            ca[t].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca[t].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (depth) {
            da.imageView = view(*depth, VK_IMAGE_VIEW_TYPE_2D, {});
            da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {rw, rh}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = ncolor;
        ri.pColorAttachments = ca;
        ri.pDepthAttachment = depth ? &da : nullptr;
        ri.pStencilAttachment = depth && has_stencil(depth->format) ? &da : nullptr;
        VkFormat formats[8] = {};
        for (uint32_t t = 0; t < ncolor; ++t)
            if (colors[t]) formats[t] = colors[t]->format;
        vkc::cmd_begin_rendering(cmd(), ri, formats, depth ? depth->format : VK_FORMAT_UNDEFINED);
        rendering_ = true;
        active_color_.assign(colors, colors + ncolor);
        active_depth_ = depth;
    }
    VkCommandBuffer c = cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    for (int s = 0; s < 3; ++s)
        if (sets[s]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, static_cast<uint32_t>(s), 1, &sets[s], 0, nullptr);
    // GX2 viewports are GL-style (y up): flip with a negative height
    // (guest pixels, times the internal resolution scale)
    VkViewport v{st.viewport[0] * scale_, (st.viewport[1] + st.viewport[3]) * scale_, st.viewport[2] * scale_, -st.viewport[3] * scale_,
                 std::clamp(st.viewport[4], 0.0f, 1.0f), std::clamp(st.viewport[5], 0.0f, 1.0f)};
    if (v.width <= 0.0f) v = {0.0f, static_cast<float>(rh), static_cast<float>(rw), -static_cast<float>(rh), 0.0f, 1.0f};
    vkCmdSetViewport(c, 0, 1, &v);
    VkRect2D sc{{0, 0}, {rw, rh}};
    if (st.scissor_set) {
        const uint32_t x = std::min(static_cast<uint32_t>(st.scissor[0] * scale_), rw),
                       y = std::min(static_cast<uint32_t>(st.scissor[1] * scale_), rh);
        sc = {{static_cast<int32_t>(x), static_cast<int32_t>(y)},
              {std::min(scaled(st.scissor[2]), rw - x), std::min(scaled(st.scissor[3]), rh - y)}};
    }
    vkCmdSetScissor(c, 0, 1, &sc);
    vkCmdSetBlendConstants(c, st.blend_constant);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, st.stencil_front.compare_mask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, st.stencil_front.write_mask);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, st.stencil_front.ref);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, sback.compare_mask);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, sback.write_mask);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, sback.ref);
    const abi::PixelPushConstants ppc{st.alpha_test && !(debug_ & 1) ? st.alpha_func : 7u, st.alpha_ref};
    vkCmdPushConstants(c, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof ppc, &ppc);
    const abi::VertexPushConstants vpc{st.point_size * scale_};
    vkCmdPushConstants(c, layout, VK_SHADER_STAGE_VERTEX_BIT, abi::kVertexPushOffset, sizeof vpc, &vpc);
    if (!loc_v.empty()) vkCmdBindVertexBuffers(c, 0, 1, &vb.buffer, &vb.offset);
    if (!loc_i.empty()) vkCmdBindVertexBuffers(c, 1, 1, &ib.buffer, &ib.offset);
    if (index_count) {
        vkCmdBindIndexBuffer(c, idx.buffer, idx.offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(c, index_count, call.instances, 0, 0, 0);
    } else {
        vkCmdDraw(c, call.count, call.instances, 0, 0);
    }
    ++n_draws_;
}

// ---- our full-screen passes: look grade, ambient occlusion, depth of field ---------------------------------------
// All share one vertex shader (a full-screen triangle), one descriptor set layout (three combined image samplers;
// depth is read with texelFetch) and one pipeline layout (32 bytes of fragment push constants).
//
// "Tri" (the look of Monster Hunter Tri on the Wii) and global illumination are scene passes too (kTriSceneFs,
// kGiFs), so the HUD stays untouched; the TV post pass only draws the vignette.
//
// Ambient occlusion and depth of field are not in the game (its uDOFFilter / uBokehFilter never draw from the intro
// to Moga Village, docs/research-log.md 2026-10-27). They run on the scene colour target once a frame, just before the
// first draw that samples it - in MH3U the bloom downsample, after all 3D and before the HUD - using the scene's depth
// buffer. The projection is unknown, so view space is rebuilt from depth alone: z' = 1 / (1 - d) is eye depth in
// units of the near plane (exact for a far plane much farther than the near one), with an assumed vertical FOV.
namespace {

const char* kPostVs = R"(#version 450
layout(location = 0) out vec2 uv;
void main() {
    uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The post pass on the TV image: the vignette, p.a.y its strength (the Tri look is a scene pass now, kTriSceneFs).
const char* kTriFs = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Params { vec4 a, b; } p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag;
void main() {  // darker towards the corners, by the distance from the centre in screen proportions
    vec3 c = texture(src, uv).rgb;
    vec2 d = (uv - 0.5) * vec2(1.0, 0.8);
    frag = vec4(c * (1.0 - p.a.y * smoothstep(0.25, 0.75, length(d) * 1.25)), 1.0);
}
)";

// Depth helpers shared by the scene passes (prepended to each).
const char* kDepthGlsl = R"(#version 450
layout(push_constant) uniform Params { vec4 a, b; } p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag;
const float kTanHalfFov = 0.45;  // assumed: ~48 degrees vertical
float lin(float d) { return 1.0 / max(1.0 - d, 1.0 / 60000.0); }
)";

// Ambient occlusion into an R8 image: normal-oriented hemisphere samples in a screen-space disk (12 taps, rotated per
// pixel), cosine-weighted with a range falloff. p.a = (world radius in near-plane units, strength, -, -).
const char* kAoFs = R"(
layout(set = 0, binding = 0) uniform sampler2D depth;
ivec2 ds;
vec3 pos(ivec2 c) {
    float z = lin(texelFetch(depth, clamp(c, ivec2(0), ds - 1), 0).r);
    vec2 ndc = (vec2(c) + 0.5) / vec2(ds) * 2.0 - 1.0;
    return vec3(ndc.x * kTanHalfFov * float(ds.x) / float(ds.y) * z, ndc.y * kTanHalfFov * z, z);
}
void main() {
    ds = textureSize(depth, 0);
    ivec2 c = ivec2(uv * vec2(ds));
    float d = texelFetch(depth, c, 0).r;
    frag = vec4(1.0);
    if (d >= 0.99999) return;  // sky
    vec3 P = pos(c);
    vec3 dx1 = pos(c + ivec2(1, 0)) - P, dx0 = P - pos(c - ivec2(1, 0));
    vec3 dy1 = pos(c + ivec2(0, 1)) - P, dy0 = P - pos(c - ivec2(0, 1));
    vec3 N = normalize(cross(abs(dx1.z) < abs(dx0.z) ? dx1 : dx0, abs(dy1.z) < abs(dy0.z) ? dy1 : dy0));
    if (dot(N, P) > 0.0) N = -N;  // towards the camera
    float R = p.a.x;
    float rpx = min(R * 0.5 * float(ds.y) / (kTanHalfFov * P.z), 0.1 * float(ds.y));
    if (rpx < 1.5) return;
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float sum = 0.0;
    for (int i = 0; i < 12; ++i) {
        float a = (float(i) + noise) * 2.3999632;
        float r = sqrt((float(i) + 0.5) / 12.0) * rpx;
        vec3 v = pos(c + ivec2(vec2(cos(a), sin(a)) * r)) - P;
        float vv = dot(v, v);
        sum += max(dot(v, N) * inversesqrt(vv + 1e-6) - 0.1, 0.0) * max(1.0 - vv / (R * R), 0.0);
    }
    frag = vec4(max(1.0 - p.a.y * sum / 12.0, 0.0));
}
)";

// Blurs the AO (4x4, depth-aware) and multiplies it into the scene colour (blend: dst * src).
const char* kAoApplyFs = R"(
layout(set = 0, binding = 0) uniform sampler2D ao;
layout(set = 0, binding = 1) uniform sampler2D depth;
void main() {
    ivec2 ds = textureSize(depth, 0);
    float zc = lin(texelFetch(depth, ivec2(uv * vec2(ds)), 0).r);
    vec2 at = 1.0 / vec2(textureSize(ao, 0));
    float s = 0.0, w = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x) {
            vec2 t = uv + (vec2(x, y) + 0.5) * at;
            float z = lin(texelFetch(depth, clamp(ivec2(t * vec2(ds)), ivec2(0), ds - 1), 0).r);
            float k = max(1.0 - abs(z - zc) / (0.04 * zc), 0.0) + 1e-3;
            s += texture(ao, t).r * k;
            w += k;
        }
    frag = vec4(vec3(s / w), 1.0);
}
)";

// Global illumination (screen space, one bounce) into an RGBA16F image: the colour of nearby surfaces that face this
// pixel, gathered like the AO's samples (16 taps in a screen-space disk, rotated per pixel) and weighted by the
// cosine and a range falloff. p.a = (world radius in near-plane units, -, -, -).
const char* kGiFs = R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D depth;
ivec2 ds;
vec3 pos(ivec2 c) {
    float z = lin(texelFetch(depth, clamp(c, ivec2(0), ds - 1), 0).r);
    vec2 ndc = (vec2(c) + 0.5) / vec2(ds) * 2.0 - 1.0;
    return vec3(ndc.x * kTanHalfFov * float(ds.x) / float(ds.y) * z, ndc.y * kTanHalfFov * z, z);
}
void main() {
    ds = textureSize(depth, 0);
    ivec2 c = ivec2(uv * vec2(ds));
    frag = vec4(0.0);
    if (texelFetch(depth, c, 0).r >= 0.99999) return;  // sky
    vec3 P = pos(c);
    vec3 dx1 = pos(c + ivec2(1, 0)) - P, dx0 = P - pos(c - ivec2(1, 0));
    vec3 dy1 = pos(c + ivec2(0, 1)) - P, dy0 = P - pos(c - ivec2(0, 1));
    vec3 N = normalize(cross(abs(dx1.z) < abs(dx0.z) ? dx1 : dx0, abs(dy1.z) < abs(dy0.z) ? dy1 : dy0));
    if (dot(N, P) > 0.0) N = -N;
    float R = p.a.x;
    float rpx = min(R * 0.5 * float(ds.y) / (kTanHalfFov * P.z), 0.12 * float(ds.y));
    if (rpx < 2.0) return;
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 16; ++i) {
        float a = (float(i) + noise) * 2.3999632;
        float r = sqrt((float(i) + 0.5) / 16.0) * rpx;
        ivec2 sc = c + ivec2(vec2(cos(a), sin(a)) * r);
        vec3 v = pos(sc) - P;
        float vv = dot(v, v);
        float w = max(dot(v, N) * inversesqrt(vv + 1e-6) - 0.05, 0.0) * max(1.0 - vv / (R * R), 0.0);
        sum += texture(src, (vec2(sc) + 0.5) / vec2(ds)).rgb * w;
    }
    frag = vec4(sum / 8.0, 1.0);
}
)";

// Blurs the GI (4x4, depth-aware) and adds it as bounce light, tinted by the surface's own colour: out = c + c * gi
// * strength. p.a = (strength, -, -, -).
const char* kGiApplyFs = R"(
layout(set = 0, binding = 0) uniform sampler2D gi;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D src;
void main() {
    ivec2 ds = textureSize(depth, 0);
    float zc = lin(texelFetch(depth, ivec2(uv * vec2(ds)), 0).r);
    vec2 at = 1.0 / vec2(textureSize(gi, 0));
    vec3 s = vec3(0.0);
    float w = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x) {
            vec2 t = uv + (vec2(x, y) + 0.5) * at;
            float z = lin(texelFetch(depth, clamp(ivec2(t * vec2(ds)), ivec2(0), ds - 1), 0).r);
            float k = max(1.0 - abs(z - zc) / (0.04 * zc), 0.0) + 1e-3;
            s += texture(gi, t).rgb * k;
            w += k;
        }
    vec4 c = texture(src, uv);
    frag = vec4(c.rgb + c.rgb * (s / w) * p.a.x, c.a);
}
)";

// "Tri" (Monster Hunter Tri on the Wii) on the 3D scene, before the game's own bloom and the HUD: muted, near-neutral
// colour, milky lifted shadows, a soft wide glow, and grey-blue haze growing with distance (atmospheric perspective). The first version graded the whole TV image instead (HUD included) with a ring-shaped
// glow, less saturation and lifted grey shadows: washed out with ghosted edges ("ugly", user 2026-10-28).
// p.a = (glow radius px, haze strength, haze start, haze per unit), p.b = (glow strength, -, -, -).
const char* kTriSceneFs = R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D depth;
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 bright(vec2 t) {  // soft threshold: what is above 0.62 glows, fading in
    vec3 c = texture(src, t).rgb;
    float l = luma(c);
    return c * smoothstep(0.62, 0.95, l);
}
void main() {
    ivec2 ds = textureSize(depth, 0);
    vec4 c4 = texture(src, uv);
    vec3 c = c4.rgb;
    float d = texelFetch(depth, ivec2(uv * vec2(ds)), 0).r;
    // haze towards the distance (not on the sky)
    if (d < 0.99999) {
        float z = lin(d);
        float f = p.a.y * (1.0 - exp(-max(z - p.a.z, 0.0) * p.a.w));
        c = mix(c, vec3(0.70, 0.74, 0.77), f);
    }
    // glow: 24 taps on a golden-angle spiral, weighted towards the centre
    vec2 px = 1.0 / vec2(textureSize(src, 0));
    vec3 glow = vec3(0.0);
    float gw = 0.0;
    for (int i = 0; i < 24; ++i) {
        float a = float(i) * 2.3999632, r = sqrt((float(i) + 0.5) / 24.0);
        float w = exp(-3.0 * r * r);
        glow += bright(uv + vec2(cos(a), sin(a)) * r * p.a.x * px) * w;
        gw += w;
    }
    glow /= gw;
    // grade, fitted to a Tri screenshot of Moga Village against the same view in MH3U (user's reference,
    // 2026-10-28): saturation 0.16 vs 0.26, near-neutral balance (MH3U is yellower), blacks lifted (2nd percentile
    // 0.114 vs 0.071), a little brighter (mean 0.344 vs 0.296), same spread
    // (this pass comes before the game's bloom and tone pass, which amplify lift and desaturation: the values are fitted
    // on the final frame, not taken from the measured ratios directly)
    c *= vec3(1.0, 1.0, 1.035);
    c = mix(vec3(luma(c)), c, 0.80);
    c = clamp(c, 0.0, 1.0) * 0.975 + 0.016;
    c += glow * vec3(1.0, 0.97, 0.90) * p.b.x;
    frag = vec4(clamp(c, 0.0, 1.0), c4.a);
}
)";

// Focus distance into a 1x1 R16F image, eased towards the nearest of 9 taps around the hunter (screen centre, a
// little low) by the pipeline's blend constants.
const char* kFocusFs = R"(
layout(set = 0, binding = 0) uniform sampler2D depth;
void main() {
    ivec2 ds = textureSize(depth, 0);
    float z = 60000.0;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            z = min(z, lin(texelFetch(depth, ivec2(vec2(0.45 + 0.05 * float(x), 0.45 + 0.1 * float(y)) * vec2(ds)), 0).r));
    frag = vec4(z);
}
)";

// Depth of field: background blur growing with distance beyond the focus (and, scaled by p.a.w, foreground blur in
// front of it). 24-tap golden-angle disk; nearer samples count only as far as their own blur reaches, so a sharp
// foreground does not bleed into the blurred background. p.a = (max radius in pixels, start, range, near).
const char* kDofFs = R"(
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D focus;
float coc(float z, float zf) {
    float far = clamp((z / zf - 1.0 - p.a.y) / p.a.z, 0.0, 1.0);
    float near = clamp((zf / z - 1.0 - p.a.y) / p.a.z, 0.0, 1.0) * p.a.w;
    return max(far, near) * p.a.x;
}
void main() {
    ivec2 ds = textureSize(depth, 0);
    float zf = texelFetch(focus, ivec2(0), 0).r;
    float zc = lin(texelFetch(depth, ivec2(uv * vec2(ds)), 0).r);
    vec4 c = texture(src, uv);
    float r = coc(zc, zf);
    frag = c;
    if (p.b.x > 0.0) {  // MH3U_FX_DEBUG: log2 eye depth (near-plane units) / 16, focus marker
        float d = texelFetch(depth, ivec2(uv * vec2(ds)), 0).r;
        frag = vec4(log2(zc) / 16.0, (1.0 - d) * 1000.0, abs(log2(zc) - log2(zf)) < 0.05 ? 1.0 : 0.0, 1.0);
        return;
    }
    if (r < 0.5) return;
    vec2 px = 1.0 / vec2(textureSize(src, 0));
    vec3 sum = c.rgb;
    float wsum = 1.0;
    for (int i = 0; i < 24; ++i) {
        float a = float(i) * 2.3999632, rr = sqrt((float(i) + 0.5) / 24.0) * r;
        vec2 t = uv + vec2(cos(a), sin(a)) * rr * px;
        float zs = lin(texelFetch(depth, clamp(ivec2(t * vec2(ds)), ivec2(0), ds - 1), 0).r);
        float w = zs < zc ? clamp(coc(zs, zf) - rr + 1.0, 0.0, 1.0) : 1.0;
        sum += texture(src, t).rgb * w;
        wsum += w;
    }
    frag = vec4(sum / wsum, c.a);
}
)";

float env_float(const char* name, float def) {
    char* env = nullptr;
    size_t len = 0;
    float v = def;
    if (_dupenv_s(&env, &len, name) == 0 && env && *env) v = std::strtof(env, nullptr);
    std::free(env);
    return v;
}

}  // namespace

VkShaderModule VkRenderer::fx_module(const std::string& glsl, const char* name, bool vertex, std::string* error) {
    std::string err;
    const std::vector<uint32_t> code = latte::compile_glsl(glsl.c_str(), vertex ? latte::Stage::Vertex : latte::Stage::Pixel, name, true, &err);
    if (code.empty()) {
        if (error) *error = std::string(name) + ": " + err;
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev_, &ci, nullptr, &m);
    fx_modules_.push_back(m);
    return m;
}

VkPipeline VkRenderer::fx_pipeline(VkShaderModule fs, VkFormat fmt, FxBlend blend) {
    const auto key = std::make_tuple(fs, fmt, blend);
    if (auto it = fx_pipes_.find(key); it != fx_pipes_.end()) return it->second;
    VkPipelineShaderStageCreateInfo stg[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}, {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stg[0].module = fx_vs_;
    stg[0].pName = "main";
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stg[1].module = fs;
    stg[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    if (blend == FxBlend::Multiply) {  // rgb = dst * src, alpha kept
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        att.colorWriteMask = 0x7;
    } else if (blend == FxBlend::Ease) {  // dst + (src - dst) * 0.1
        att.blendEnable = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        cb.blendConstants[0] = cb.blendConstants[1] = cb.blendConstants[2] = cb.blendConstants[3] = 0.1f;
    }
    if (blend == FxBlend::Multiply) {
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    }
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &ri;
    pci.stageCount = 2;
    pci.pStages = stg;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dy;
    pci.layout = fx_layout_;
    vkc::pipeline_rendering(pci, ri);
    VkPipeline pipe = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(dev_, pipeline_cache_, 1, &pci, nullptr, &pipe) != VK_SUCCESS) {
        pipe = VK_NULL_HANDLE;
        warn("effect pipeline creation failed (format " + std::to_string(fmt) + ")");
    }
    fx_pipes_[key] = pipe;
    return pipe;
}

// One full-screen pass into `out` (kept contents: blended passes add to them). Depth images are bound by their depth
// aspect with a nearest sampler.
void VkRenderer::fx_pass(VkPipeline pipe, Image& out, std::initializer_list<Image*> tex, const float push[8]) {
    if (!pipe) return;
    end_rendering();
    Image* t[3] = {};
    size_t n = 0;
    for (Image* i : tex) t[n++] = i;
    for (size_t i = n; i < 3; ++i) t[i] = t[0];
    for (Image* i : t) transition(*i, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const VkDescriptorSet set = descriptor_set(fx_set_layout_);
    VkDescriptorImageInfo ii[3];
    for (int i = 0; i < 3; ++i) {
        const bool depth = (t[i]->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        ii[i] = {depth ? fx_nearest_ : post_sampler_, view(*t[i], VK_IMAGE_VIEW_TYPE_2D, {}, depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 3;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = ii;
    vkUpdateDescriptorSets(dev_, 1, &w, 0, nullptr);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view(out, VK_IMAGE_VIEW_TYPE_2D, {});
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {out.width, out.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    VkCommandBuffer c = cmd();
    vkc::cmd_begin_rendering(c, ri, &out.format);
    const VkViewport v{0, 0, static_cast<float>(out.width), static_cast<float>(out.height), 0, 1};
    const VkRect2D sc{{0, 0}, {out.width, out.height}};
    vkCmdSetViewport(c, 0, 1, &v);
    vkCmdSetScissor(c, 0, 1, &sc);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, fx_layout_, 0, 1, &set, 0, nullptr);
    float zero[8] = {};
    vkCmdPushConstants(c, fx_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, push ? push : zero);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkc::cmd_end_rendering(c);
}

bool VkRenderer::init_post(std::string* error) {
    char* env = nullptr;
    size_t len = 0;
    if (_dupenv_s(&env, &len, "MH3U_LOOK") == 0 && env) look_ = std::string(env) == "tri" ? 1 : 0;
    std::free(env);
    if (_dupenv_s(&env, &len, "MH3U_RENDER_SCALE") == 0 && env) scale_ = std::clamp(static_cast<float>(std::atof(env)), 0.25f, 4.0f);
    std::free(env);
    if (scale_ != 1.0f) std::printf("renderer: internal resolution %.2fx\n", scale_);
    want_look_ = look_ != 0;
    want_vignette_ = env_float("MH3U_VIGNETTE", 0) != 0;
    want_ao_ = env_float("MH3U_AO", 0) != 0;
    want_dof_ = env_float("MH3U_DOF", 0) != 0;
    want_gi_ = env_float("MH3U_GI", 0) != 0;
    look_ = 0;  // set per frame from want_* (end_frame) once the passes exist
    ao_radius_ = env_float("MH3U_AO_RADIUS", ao_radius_);
    ao_strength_ = env_float("MH3U_AO_STRENGTH", ao_strength_);
    dof_radius_ = env_float("MH3U_DOF_RADIUS", dof_radius_);
    dof_start_ = env_float("MH3U_DOF_START", dof_start_);
    dof_range_ = env_float("MH3U_DOF_RANGE", dof_range_);
    dof_near_ = env_float("MH3U_DOF_NEAR", dof_near_);
    fx_debug_ = env_float("MH3U_FX_DEBUG", 0);
    gi_radius_ = env_float("MH3U_GI_RADIUS", gi_radius_);
    gi_strength_ = env_float("MH3U_GI_STRENGTH", gi_strength_);
    tri_glow_radius_ = env_float("MH3U_TRI_GLOW_RADIUS", tri_glow_radius_);
    tri_glow_ = env_float("MH3U_TRI_GLOW", tri_glow_);
    tri_haze_ = env_float("MH3U_TRI_HAZE", tri_haze_);
    tri_haze_start_ = env_float("MH3U_TRI_HAZE_START", tri_haze_start_);
    // every pass is built (they are small), so the launcher can switch them while the game runs

    VkDescriptorSetLayoutBinding b[3];
    for (uint32_t i = 0; i < 3; ++i) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 3;
    lci.pBindings = b;
    vkCreateDescriptorSetLayout(dev_, &lci, nullptr, &fx_set_layout_);
    const VkPushConstantRange pr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &fx_set_layout_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(dev_, &pli, nullptr, &fx_layout_);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(dev_, &sci, nullptr, &post_sampler_);
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    vkCreateSampler(dev_, &sci, nullptr, &fx_nearest_);

    fx_vs_ = fx_module(kPostVs, "post.vert", true, error);
    if (!fx_vs_) return false;
    bool ok = true;
    const VkShaderModule tri = fx_module(kTriFs, "tri.frag", false, error);
    post_pipe_ = tri ? fx_pipeline(tri, VK_FORMAT_R8G8B8A8_UNORM, FxBlend::None) : VK_NULL_HANDLE;
    ok &= post_pipe_ != VK_NULL_HANDLE;
    const std::string depth_glsl = kDepthGlsl;
    tri_fs_ = fx_module(depth_glsl + kTriSceneFs, "tri.frag", false, error);
    ok &= tri_fs_ != VK_NULL_HANDLE;
    if (tri_fs_ && want_look_) std::printf("renderer: look \"tri\" on\n");
    {
        gi_fs_ = fx_module(depth_glsl + kGiFs, "gi.frag", false, error);
        gi_apply_fs_ = fx_module(depth_glsl + kGiApplyFs, "gi_apply.frag", false, error);
        if (!gi_fs_ || !gi_apply_fs_) gi_fs_ = VK_NULL_HANDLE, ok = false;
        else if (want_gi_) std::printf("renderer: global illumination on (radius %g, strength %g)\n", gi_radius_, gi_strength_);
    }
    {
        ao_fs_ = fx_module(depth_glsl + kAoFs, "ao.frag", false, error);
        ao_apply_fs_ = fx_module(depth_glsl + kAoApplyFs, "ao_apply.frag", false, error);
        if (!ao_fs_ || !ao_apply_fs_) ao_fs_ = VK_NULL_HANDLE, ok = false;
        else if (want_ao_) std::printf("renderer: ambient occlusion on (radius %g, strength %g)\n", ao_radius_, ao_strength_);
    }
    {
        focus_fs_ = fx_module(depth_glsl + kFocusFs, "focus.frag", false, error);
        dof_fs_ = fx_module(depth_glsl + kDofFs, "dof.frag", false, error);
        if (!focus_fs_ || !dof_fs_) dof_fs_ = VK_NULL_HANDLE, ok = false;
        else if (want_dof_) std::printf("renderer: depth of field on (radius %g, start %g, range %g, near %g)\n", dof_radius_, dof_start_, dof_range_, dof_near_);
    }
    tri_ = want_look_ && tri_fs_;
    vignette_ = want_vignette_;
    look_ = vignette_ && post_pipe_ ? 1 : 0;
    ao_ = want_ao_ && ao_fs_;
    dof_ = want_dof_ && dof_fs_;
    gi_ = want_gi_ && gi_fs_;
    return ok;
}

// draw(): finds this frame's scene (the TV-sized colour target drawn with depth writes) and runs AO / DOF on it right
// before the first draw that samples it.
void VkRenderer::scene_hook(const DrawState& st) {
    const GuestSurface& c = st.color[0];
    if (st.color_bound[0] && st.depth_bound && st.depth_write && c.width == tv_guest_w_ && c.height == tv_guest_h_) {
        scene_color_ = c.image;
        scene_depth_ = st.depth.image;
        return;
    }
    if (!scene_color_) return;
    const StageBindings& ps = st.stage[kStagePixel];
    for (const TextureBinding& t : ps.tex) {
        if (!t.bound || t.surf.image != scene_color_) continue;
        scene_done_ = true;
        auto ci = targets_.find(scene_color_);
        auto di = targets_.find(scene_depth_);
        if (ci == targets_.end() || di == targets_.end() || !(di->second->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
        scene_effects(*ci->second, *di->second);
        return;
    }
}

void VkRenderer::scene_effects(Image& color, Image& depth) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        std::printf("renderer: scene effects on target %08X (%ux%u), depth %08X\n", color.surf.image, color.width, color.height, depth.surf.image);
    }
    // order: global illumination (bounce light from the scene as lit), ambient occlusion, the Tri grade, depth of field
    if (gi_) {
        const uint32_t w = std::max(1u, color.width / 2), h = std::max(1u, color.height / 2);  // half resolution, blurred
        if (!gi_img_ || gi_img_->width != w || gi_img_->height != h) {
            if (gi_img_) fc().garbage.push_back(std::move(gi_img_));
            gi_img_ = create_image(VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_TYPE_2D, w, h, 1, false,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        if (gi_img_) {
            Image* src = snapshot(color);
            const float pg[8] = {gi_radius_};
            fx_pass(fx_pipeline(gi_fs_, VK_FORMAT_R16G16B16A16_SFLOAT, FxBlend::None), *gi_img_, {src, &depth}, pg);
            src = snapshot(color);
            const float pa[8] = {gi_strength_};
            fx_pass(fx_pipeline(gi_apply_fs_, color.format, FxBlend::None), color, {gi_img_.get(), &depth, src}, pa);
        }
    }
    if (ao_) {
        // half resolution above 1440p: the blur in the apply pass hides it
        const uint32_t div = color.height > 1440 ? 2 : 1, w = std::max(1u, color.width / div), h = std::max(1u, color.height / div);
        if (!ao_img_ || ao_img_->width != w || ao_img_->height != h) {
            if (ao_img_) fc().garbage.push_back(std::move(ao_img_));
            ao_img_ = create_image(VK_FORMAT_R8_UNORM, VK_IMAGE_TYPE_2D, w, h, 1, false,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        }
        if (ao_img_) {
            const float p[8] = {ao_radius_, ao_strength_};
            fx_pass(fx_pipeline(ao_fs_, VK_FORMAT_R8_UNORM, FxBlend::None), *ao_img_, {&depth}, p);
            fx_pass(fx_pipeline(ao_apply_fs_, color.format, FxBlend::Multiply), color, {ao_img_.get(), &depth}, nullptr);
        }
    }
    if (tri_) {
        Image* src = snapshot(color);
        const float pt[8] = {tri_glow_radius_ * static_cast<float>(color.height) / 1080.0f, tri_haze_, tri_haze_start_, tri_haze_k_, tri_glow_};
        fx_pass(fx_pipeline(tri_fs_, color.format, FxBlend::None), color, {src, &depth}, pt);
    }
    if (dof_) {
        if (!focus_img_) {
            focus_img_ = create_image(VK_FORMAT_R16_SFLOAT, VK_IMAGE_TYPE_2D, 1, 1, 1, false,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                      VK_IMAGE_ASPECT_COLOR_BIT);
            if (focus_img_) {
                end_rendering();
                transition(*focus_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                const VkClearColorValue v{{50.0f, 50.0f, 50.0f, 50.0f}};
                const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                vkCmdClearColorImage(cmd(), focus_img_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
            }
        }
        if (focus_img_) {
            fx_pass(fx_pipeline(focus_fs_, VK_FORMAT_R16_SFLOAT, FxBlend::Ease), *focus_img_, {&depth}, nullptr);
            Image* src = snapshot(color);
            const float p[8] = {dof_radius_ * static_cast<float>(color.height) / 1080.0f, dof_start_, dof_range_, dof_near_, fx_debug_};
            fx_pass(fx_pipeline(dof_fs_, color.format, FxBlend::None), color, {src, &depth, focus_img_.get()}, p);
        }
    }
}

// src (TRANSFER_SRC) -> post_ (TRANSFER_SRC), graded; returns the image to present.
Image* VkRenderer::post_process(Image& src) {
    if (!post_pipe_) return &src;
    if (!post_ || post_->width != src.width || post_->height != src.height) {
        if (post_) fc().garbage.push_back(std::move(post_));
        post_ = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, src.width, src.height, 1, false,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!post_) return &src;
    }
    const float push[8] = {0.0f, vignette_ ? 0.55f : 0.0f};
    fx_pass(post_pipe_, *post_, {&src}, push);
    transition(*post_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    return post_.get();
}

Renderer::Frame VkRenderer::end_frame() {
    Frame out;
    if (!recording_) begin();
    end_rendering();
    // GamePad inset: the last GamePad image, scaled to 30% of the TV width, in the bottom-right corner
    if (gamepad_view_ == 1 && tv_written_ && drc_written_ && tv_ && drc_) {
        const int32_t w = static_cast<int32_t>(tv_->width) * 3 / 10;
        const int32_t h = w * static_cast<int32_t>(drc_->height) / static_cast<int32_t>(std::max(1u, drc_->width));
        const int32_t margin = static_cast<int32_t>(tv_->width) / 64;
        const int32_t x1 = static_cast<int32_t>(tv_->width) - margin, y1 = static_cast<int32_t>(tv_->height) - margin;
        transition(*tv_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {static_cast<int32_t>(drc_->width), static_cast<int32_t>(drc_->height), 1};
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[0] = {x1 - w, y1 - h, 0};
        bl.dstOffsets[1] = {x1, y1, 1};
        vkCmdBlitImage(cmd(), drc_->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tv_->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
        transition(*tv_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    // the look post pass (Tri) on what is shown on the TV
    Image* graded = look_ && tv_written_ && tv_ && gamepad_view_ != 2 ? post_process(*tv_) : nullptr;
    FrameCtx& f = fc();
    if (ts_pool_) {
        vkCmdWriteTimestamp(f.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool_, static_cast<uint32_t>(frame_ % 2) * 2 + 1);
        f.timed = true;
    }
    vkEndCommandBuffer(f.cmd);
    {
        std::lock_guard g(*vk_.queue_mutex);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &f.cmd;
        vkQueueSubmit(vk_.queue, 1, &si, f.fence);
    }
    recording_ = false;
    ++frame_;
    if (want_dump_ != dumping_) {
        dumping_ = want_dump_;
        if (dumping_)  // textures already cached are dumped at their next use (a changed hash re-uploads them)
            for (auto& [k, t] : textures_) t->hash ^= 1;
    }
    scene_color_ = scene_depth_ = 0;
    scene_done_ = false;
    tri_ = want_look_ && tri_fs_;
    vignette_ = want_vignette_;
    look_ = vignette_ && post_pipe_ ? 1 : 0;
    ao_ = want_ao_ && ao_fs_;
    dof_ = want_dof_ && dof_fs_;
    gi_ = want_gi_ && gi_fs_;
    Image* shown = graded ? graded : gamepad_view_ == 2 && drc_written_ ? drc_.get() : tv_written_ ? tv_.get() : nullptr;
    if (shown) {
        out.tv_image = shown->image;
        out.width = shown->width;
        out.height = shown->height;
    }
    return out;
}

bool VkRenderer::read_tv(std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    Image* const img = look_ && post_ ? post_.get() : tv_.get();  // what was shown (graded by the look pass)
    if (!img || !tv_written_) return false;
    begin();
    end_rendering();
    const VkDeviceSize bytes = VkDeviceSize{img->width} * img->height * 4;
    const Alloc a = alloc(bytes, 16);
    VkBufferImageCopy c{};
    c.bufferOffset = a.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {img->width, img->height, 1};
    vkCmdCopyImageToBuffer(cmd(), img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a.buffer, 1, &c);
    end_frame();
    vkWaitForFences(dev_, 1, &frames_[(frame_ - 1) % 2].fence, VK_TRUE, UINT64_MAX);
    width = img->width;
    height = img->height;
    rgba.assign(a.ptr, a.ptr + bytes);
    for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
    return true;
}

bool VkRenderer::read_target(uint32_t image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height) {
    auto it = targets_.find(image);
    if (it == targets_.end() || (it->second->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    Image& src = *it->second;
    begin();
    end_rendering();
    auto tmp = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_2D, src.width, src.height, 1, false,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    transition(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(*tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {static_cast<int32_t>(src.width), static_cast<int32_t>(src.height), 1};
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[1] = bl.srcOffsets[1];
    vkCmdBlitImage(cmd(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
    transition(*tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkDeviceSize bytes = VkDeviceSize{src.width} * src.height * 4;
    const Alloc a = alloc(bytes, 16);
    VkBufferImageCopy c{};
    c.bufferOffset = a.offset;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {src.width, src.height, 1};
    vkCmdCopyImageToBuffer(cmd(), tmp->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, a.buffer, 1, &c);
    end_frame();
    vkWaitForFences(dev_, 1, &frames_[(frame_ - 1) % 2].fence, VK_TRUE, UINT64_MAX);
    destroy_image(*tmp);
    width = src.width;
    height = src.height;
    rgba.assign(a.ptr, a.ptr + bytes);
    return true;
}

std::string VkRenderer::stats() const {
    std::string s = "renderer: " + std::to_string(n_draws_) + " draws, " + std::to_string(n_skipped_) + " skipped, " +
                    std::to_string(pipelines_.size()) + " pipelines, " + std::to_string(shaders_.size()) + " shaders, " +
                    std::to_string(targets_.size()) + " render targets, " + std::to_string(textures_.size()) + " textures, " +
                    std::to_string(n_uploads_) + " uploads, " + std::to_string(n_target_recreates_) +
                    " render targets recreated";
    // GPU time and streamed bytes per frame over the last 600 frames (the scene at exit, not boot and menus)
    const auto last = [](const std::vector<float>& v) {
        std::vector<float> w(v.end() - static_cast<ptrdiff_t>(std::min<size_t>(v.size(), 600)), v.end());
        std::sort(w.begin(), w.end());
        float sum = 0;
        for (float x : w) sum += x;
        char b[96];
        if (w.empty()) return std::string("n/a");
        std::snprintf(b, sizeof b, "avg %.2f, median %.2f, p90 %.2f, max %.2f", sum / static_cast<float>(w.size()),
                      w[w.size() / 2], w[w.size() * 9 / 10], w.back());
        return std::string(b);
    };
    s += "\n  GPU ms/frame (last 600): " + last(gpu_ms_) + "\n  streamed MB/frame (last 600): " + last(stream_mb_);
    if (const core::GuestMemory* gm = core::GuestMemory::current(); gm && gm->scans())
        s += "\n  write watch: " + std::to_string(gm->scans()) + " scans, " + std::to_string(gm->scanned_pages()) +
             " written pages";
    s += "\n  texture content hashes: " + std::to_string(n_texture_hashes_) + ", vertex conversions: " +
         std::to_string(n_vertex_converts_) + ", index conversions: " + std::to_string(n_index_converts_) + " (cache " + std::to_string(vcache_bytes_ >> 20) + " MB)";
    s += "\n  streamed in total: vertices " + std::to_string(bytes_vertex_ >> 20) + " MB, uniform blocks " +
         std::to_string(bytes_ubo_ >> 20) + " MB";
    if (packs_.size() || n_dumped_)
        s += "\n  texture packs: " + std::to_string(packs_.size()) + " files, " + std::to_string(n_replaced_) + " replaced, " +
             std::to_string(n_dumped_.load()) + " dumped";
    for (const auto& [why, n] : skip_reasons_) s += "\n  skipped " + std::to_string(n) + ": " + why;
    return s;
}

}  // namespace

std::unique_ptr<Renderer> Renderer::create(const VulkanContext& vk, uint8_t* guest_base, const std::filesystem::path& shader_dir,
                                           std::string* error) {
    if (!vk.device) {
        if (error) *error = "no Vulkan device";
        return nullptr;
    }
    auto r = std::make_unique<VkRenderer>(vk, guest_base, shader_dir);
    if (!r->init(error)) return nullptr;
    return r;
}

}  // namespace mh3u::gfx
