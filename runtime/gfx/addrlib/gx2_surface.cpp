#include "gx2_surface.h"

#include <algorithm>
#include <bit>
#include <map>
#include <mutex>
#include <tuple>

#include "latte_surface.h"

namespace mh3u::gfx::addrlib {
namespace {

constexpr uint32_t kUseColorBuffer = 0x2, kUseDepthBuffer = 0x4, kUseScanBuffer = 0x8;

SurfaceParams params(const SurfaceDesc& s, uint32_t tile_mode, uint32_t aa, bool use_flags) {
    SurfaceParams p;
    p.format = s.format;
    p.width = s.width;
    p.height = s.height;
    p.depth = s.depth;
    p.dim = s.dim;
    p.tile_mode = tile_mode;
    p.aa = aa;
    p.depth_buffer = use_flags && (s.use & kUseDepthBuffer);
    p.scan_buffer = use_flags && (s.use & kUseScanBuffer);
    return p;
}

// Levels in a full chain down to 1x1(x1).
uint32_t full_chain(const SurfaceDesc& s) {
    uint32_t largest = std::max(s.width, s.height);
    if (s.dim == dim::k3D) largest = std::max(largest, s.depth);
    return static_cast<uint32_t>(std::bit_width(largest));
}

constexpr uint32_t kSwizzleLevelMask = 0x00FF0000;  // GX2Surface::swizzle bits 16-23: first non-macro-tiled level

// GX2TileMode is macro tiled (the GX2-only "linear special" value is not)
bool gx2_macro_tiled(uint32_t tm) { return is_macro_tiled(tm) && tm != tile::kLinearSpecial; }

}  // namespace

void calc_size_and_alignment(SurfaceDesc& s) {
    // "linear general" and the special value 32 ask GX2 to choose: macro tiling unless this is a plain 1D texture
    const bool auto_tile = s.tile_mode == tile::kLinearGeneral || s.tile_mode == tile::k32Special;
    const bool shrink_small = auto_tile && (s.dim != dim::k1D || (s.use & kUseDepthBuffer) || s.aa);
    // with mode 32 the small-surface test below compares BC sizes in blocks against pixel alignments
    const uint32_t small_shift = s.tile_mode == tile::k32Special && is_block_compressed(s.format) ? 2 : 0;
    if (auto_tile)
        s.tile_mode = !shrink_small ? tile::kLinearAligned
                      : s.dim != dim::k3D || (s.use & kUseColorBuffer) ? tile::k2DThin1
                                                                        : tile::k2DThick;
    s.levels = s.levels <= 1 ? 1 : std::min(s.levels, full_chain(s));
    uint32_t chain_tm = s.tile_mode;  // tile mode of the previous level
    s.swizzle = (s.swizzle & ~kSwizzleLevelMask) | (gx2_macro_tiled(s.tile_mode) ? 0xD0000 : 0);

    // mip_offset[0] is the offset of level 1 from the image, the others are relative to level 1 (entries past the
    // chain keep what the caller had there)
    s.mip_offset[0] = 0;
    uint32_t prev_size = 0, level1 = 0;
    if (s.levels) {  // a 0x0 surface has no levels and keeps the caller's size fields
        SurfaceInfo info = surface_info(params(s, s.tile_mode, s.aa, true), 0);
        if (shrink_small) {
            if (s.tile_mode != info.tile_mode) {
                s.tile_mode = info.tile_mode;
                info = surface_info(params(s, s.tile_mode, s.aa, true), 0);
                if (!is_macro_tiled(s.tile_mode)) s.swizzle &= ~kSwizzleLevelMask;
                chain_tm = s.tile_mode;
            }
            // smaller than one tile in both directions: 1D tiling wastes less
            if (s.width < (info.pitch_align << small_shift) && s.height < (info.height_align << small_shift)) {
                s.tile_mode = s.tile_mode == tile::k2DThick ? tile::k1DThick : tile::k1DThin1;
                info = surface_info(params(s, s.tile_mode, s.aa, true), 0);
                s.swizzle &= ~kSwizzleLevelMask;
                chain_tm = s.tile_mode;
            }
        }
        s.image_size = static_cast<uint32_t>(info.size);
        s.alignment = info.base_align;
        s.pitch = info.pitch;
        prev_size = static_cast<uint32_t>(info.size);
    }
    for (uint32_t level = 1; level < s.levels; ++level) {
        const SurfaceInfo info = surface_info(params(s, s.tile_mode, s.aa, true), level);
        uint32_t padding = 0;
        if (gx2_macro_tiled(chain_tm) && !is_macro_tiled(info.tile_mode)) {
            s.swizzle = (s.swizzle & ~kSwizzleLevelMask) | (level << 16);
            chain_tm = info.tile_mode;
            if (level > 1) padding = s.swizzle & 0xFFFF;
        }
        if (info.base_align) padding += (info.base_align - prev_size % info.base_align) % info.base_align;
        if (level == 1) level1 = padding + prev_size;
        else s.mip_offset[level - 1] = padding + prev_size + s.mip_offset[level - 2];
        prev_size = static_cast<uint32_t>(info.size);
    }
    s.mip_size = s.levels > 1 ? prev_size + s.mip_offset[s.levels - 2] : 0;
    s.mip_offset[0] = level1;
    if (s.format == kFormatNV12 && s.alignment) {  // chroma plane follows the luma plane, half its size
        s.mip_offset[0] = s.image_size + (s.alignment - s.image_size % s.alignment) % s.alignment;
        s.image_size = s.mip_offset[0] + s.image_size / 2;
    }
}

bool level_layout(const SurfaceDesc& s, uint32_t image, uint32_t mipmaps, uint32_t level, uint32_t slice, Level& out) {
    const uint32_t tm = s.tile_mode & 0xF;
    const SurfaceParams p = params(s, tm, 0, false);
    const SurfaceInfo info = surface_info(p, level);
    if (!info.bpp) return false;
    uint32_t pipe_swizzle = (s.swizzle >> 8) & 1, bank_swizzle = (s.swizzle >> 9) & 3;
    // a macro-tiled mip chain carries its own swizzle in the mipmaps pointer
    if (level > 0 && is_macro_tiled(tm) && is_macro_tiled(surface_info(p, 1).tile_mode)) {
        pipe_swizzle = (mipmaps >> 8) & 1;
        bank_swizzle = (mipmaps >> 9) & 3;
        mipmaps &= ~0x700u;
    }
    out.width = std::max(1u, s.width >> level);
    out.height = std::max(1u, s.height >> level);
    if (is_block_compressed(s.format)) {
        out.width = (out.width + 3) / 4;
        out.height = (out.height + 3) / 4;
    }
    out.bytes = info.bpp / 8;
    out.pitch = info.pitch;
    out.base = level ? mipmaps + mip_offset(p, tm, s.swizzle, level) : image;

    using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static std::mutex m;
    static std::map<Key, std::pair<std::vector<uint32_t>, uint32_t>> cache;  // offsets, span
    const Key key{s.format & 0x3F, s.width, s.height, s.depth, s.dim, s.tile_mode, level, slice, pipe_swizzle, bank_swizzle};
    std::lock_guard g(m);
    auto it = cache.find(key);
    if (it == cache.end()) {
        std::vector<uint32_t> off(size_t{out.width} * out.height);
        const ElementAddresser addr(info, slice, pipe_swizzle, bank_swizzle);
        uint32_t span = 0;
        for (uint32_t y = 0; y < out.height; ++y)
            for (uint32_t x = 0; x < out.width; ++x) {
                const uint32_t a = addr(x, y);
                off[size_t{y} * out.width + x] = a;
                span = std::max(span, a + out.bytes);
            }
        it = cache.emplace(key, std::make_pair(std::move(off), span)).first;
    }
    out.offsets = &it->second.first;
    out.span = it->second.second;
    return true;
}

}  // namespace mh3u::gfx::addrlib
