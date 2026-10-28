#include "latte_surface.h"

#include <algorithm>
#include <bit>

namespace mh3u::gfx::addrlib {
namespace {

// Memory configuration GX2 gives the address library on the Wii U.
constexpr uint32_t kPipes = 2, kBanks = 4;
constexpr uint32_t kGroupBytes = 256;  // pipe interleave: bytes before the pipe bit changes
constexpr uint32_t kGroupBits = 8, kPipeBits = 1, kBankBits = 2;
constexpr uint32_t kRowBytes = 2048, kSwapBytes = 256, kSplitBytes = 2048;

uint32_t pow2_ceil(uint32_t v) { return std::bit_ceil(v); }
uint32_t align_up(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

uint32_t thickness(uint32_t tm) {
    return tm == tile::k1DThick || tm == tile::k2DThick || tm == tile::k2BThick || tm == tile::k3DThick ||
                   tm == tile::k3BThick
               ? 4
               : 1;
}
bool is_thick_macro(uint32_t tm) { return tm != tile::k1DThick && thickness(tm) == 4; }
bool is_bank_swapped(uint32_t tm) { return (tm >= tile::k2BThin1 && tm <= tile::k2BThick) || tm >= tile::k3BThin1; }

// Macro tile = kBanks x kPipes micro tiles for the THIN1/THICK modes; THIN2/THIN4 trade width for height.
uint32_t macro_aspect(uint32_t tm) {
    if (tm == tile::k2DThin2 || tm == tile::k2BThin2) return 2;
    if (tm == tile::k2DThin4 || tm == tile::k2BThin4) return 4;
    return 1;
}
uint32_t macro_width(uint32_t tm) { return 8 * kBanks / macro_aspect(tm); }
uint32_t macro_height(uint32_t tm) { return 8 * kPipes * macro_aspect(tm); }

// Bank/pipe rotation applied per slice. With 2 pipes the 3D modes rotate by 1, an odd amount, so they never fall
// back to the 2D modes the way they do on 4/8-pipe parts.
uint32_t slice_rotation(uint32_t tm) {
    if (tm >= tile::k2DThin1 && tm <= tile::k2BThick) return kPipes * (kBanks / 2 - 1);
    if (tm >= tile::k3DThin1) return 1;
    return 0;
}

uint32_t without_bank_swap(uint32_t tm) {
    switch (tm) {
        case tile::k2BThin1: return tile::k2DThin1;
        case tile::k2BThin2: return tile::k2DThin2;
        case tile::k2BThin4: return tile::k2DThin4;
        case tile::k2BThick: return tile::k2DThick;
        case tile::k3BThin1: return tile::k3DThin1;
        case tile::k3BThick: return tile::k3DThick;
        default: return tm;
    }
}

// 96/48/24-bit elements are laid out as three 32/16/8-bit ones.
uint32_t layout_bpp(uint32_t bpp) { return bpp == 96 || bpp == 48 || bpp == 24 ? bpp / 3 : bpp; }

// How many 2 KiB tile splits one micro tile of all samples needs (THICK tiles count 4 samples).
uint32_t tile_splits(uint32_t tm, uint32_t bpp, uint32_t samples) {
    if (thickness(tm) > 1) samples = 4;
    const uint32_t samples_per_split = kSplitBytes / (bpp * 8);
    return samples_per_split ? std::max(samples / samples_per_split, 1u) : 1;
}

// Tile modes the hardware can't use for this sample count / depth surface, and their replacements.
uint32_t fit_tile_mode(uint32_t tm, uint32_t bpp, uint32_t samples, bool depth) {
    const bool thin = samples > 1 || depth || tile_splits(tm, bpp, samples) > 1;
    switch (tm) {
        case tile::k1DThin1: return samples > 1 ? tile::k2DThin1 : tm;  // no 1D-tiled MSAA
        case tile::k1DThick:
            if (samples == 2 || samples == 4) return tile::k2DThick;
            return samples > 1 || depth ? tile::k1DThin1 : tm;
        case tile::k2DThick: return thin ? tile::k2DThin1 : tm;
        case tile::k2BThick: return thin ? tile::k2BThin1 : tm;
        case tile::k3DThick: return thin ? tile::k3DThin1 : tm;
        case tile::k3BThick: return thin ? tile::k3BThin1 : tm;
        default: return tm;
    }
}

// Tile mode of a mip level: mips never bank-swap, and levels smaller than a macro tile (or 4 slices for THICK)
// step down to 1D / THIN1.
uint32_t level_tile_mode(uint32_t base, uint32_t bpp, uint32_t level, uint32_t width, uint32_t height, uint32_t slices,
                         uint32_t samples, bool depth) {
    uint32_t tm = fit_tile_mode(base, bpp, samples, depth);
    if (!level) return tm;
    bpp = layout_bpp(bpp);
    width = pow2_ceil(width);
    height = pow2_ceil(height);
    slices = pow2_ceil(slices);
    tm = without_bank_swap(tm);
    const uint32_t micro_tile_bytes = (samples * bpp * thickness(tm) * 64 + 7) / 8;
    const uint32_t width_factor = micro_tile_bytes >= kGroupBytes ? 1 : kGroupBytes / micro_tile_bytes;
    if (is_macro_tiled(tm) && (width < width_factor * macro_width(tm) || height < macro_height(tm)))
        tm = thickness(tm) == 4 ? tile::k1DThick : tile::k1DThin1;
    if (thickness(tm) == 4 && slices < 4) {
        if (tm == tile::k1DThick) tm = tile::k1DThin1;
        else if (tm == tile::k2DThick) tm = tile::k2DThin1;
        else if (tm == tile::k3DThick) tm = tile::k3DThin1;
    }
    return fit_tile_mode(tm, bpp, samples, depth);
}

struct Shape {
    uint32_t pitch, height, slices;
};

// Mip levels start from power-of-two dimensions; a cube keeps its 6 faces (and is then only padded in 2D).
void mip_shape(Shape& s, bool cube, uint32_t& pad_dims) {
    s.pitch = pow2_ceil(s.pitch);
    s.height = pow2_ceil(s.height);
    if (cube) pad_dims = s.slices <= 1 ? 2 : 0;
    else s.slices = pow2_ceil(s.slices);
}

// pad_dims: how many dimensions to pad (0 = all three). `thick` follows the tile mode the caller sized for.
void pad(Shape& s, uint32_t pad_dims, bool cube, bool thick, uint32_t pitch_align, uint32_t height_align,
         uint32_t slice_align) {
    if (!pad_dims) pad_dims = 3;
    s.pitch = align_up(s.pitch, pitch_align);
    if (pad_dims > 1) s.height = align_up(s.height, height_align);
    if (pad_dims > 2 || thick) {
        if (cube) s.slices = pow2_ceil(s.slices);
        if (thick) s.slices = align_up(s.slices, slice_align);
    }
}

uint64_t bytes(const Shape& s, uint32_t bpp, uint32_t samples) {
    return (uint64_t{s.height} * s.pitch * s.slices * bpp * samples + 7) / 8;
}

struct Alignment {
    uint32_t base, pitch, height;
};

Alignment macro_alignment(uint32_t tm, uint32_t bpp, uint32_t samples) {
    bpp = layout_bpp(bpp);
    const uint32_t mw = macro_width(tm), mh = macro_height(tm), thick = thickness(tm);
    Alignment a;
    a.pitch = std::max(mw, mw * (kGroupBytes / bpp / (8 * thick) / samples));
    a.height = mh;
    if (thick == 1) a.base = std::max(samples * ((bpp * mh * mw + 7) / 8), (samples * a.height * bpp * a.pitch + 7) / 8);
    else a.base = std::max(kGroupBytes, (4 * a.height * bpp * a.pitch + 7) / 8);
    const uint32_t micro_tile_bytes = (thick * samples * bpp * 64 + 7) / 8;
    if (micro_tile_bytes >= kSplitBytes) a.base /= micro_tile_bytes / kSplitBytes;
    return a;
}

// Bank-swapped modes flip bank order every `width` elements; the pitch is padded to at least that.
uint32_t bank_swap_width(uint32_t tm, uint32_t bpp, uint32_t samples, uint32_t pitch) {
    if (!is_bank_swapped(tm)) return 0;
    const uint32_t sample_bytes = 8 * bpp;  // one sample of a micro tile
    const uint32_t samples_per_split = kSplitBytes / sample_bytes;
    const uint32_t splits = samples_per_split ? std::max(samples / samples_per_split, 1u) : 1;
    if (is_thick_macro(tm)) samples = 4;
    const uint32_t split_bytes = samples * sample_bytes / splits;
    const uint32_t swap_width = std::max(kSwapBytes / 2 / bpp, 1u) * 8 * kBanks;
    const uint32_t height_bytes = samples * macro_aspect(tm) * kPipes * bpp / splits;
    const uint32_t swap_max = kPipes * kBanks * kRowBytes / height_bytes;
    const uint32_t swap_min = kGroupBytes * 8 * kBanks / split_bytes;
    uint32_t w = swap_max >= swap_width ? std::max(swap_width, swap_min) : swap_max;
    while (w >= 2 * pitch) w >>= 1;
    return w;
}

void info_linear(SurfaceInfo& out, uint32_t tm, uint32_t bpp, uint32_t samples, Shape s, uint32_t level,
                 uint32_t pad_dims, bool cube, bool scan) {
    Alignment a{1, 1, 1};
    if (tm == tile::kLinearAligned) a = {kGroupBytes, std::max(64u, 8 * kGroupBytes / bpp), 1};
    if (scan) a.pitch = align_up(a.pitch, 32);
    if (level) mip_shape(s, cube, pad_dims);
    pad(s, pad_dims, cube, false, a.pitch, a.height, 1);
    const uint64_t size = bytes({s.pitch, s.height, s.slices * samples}, bpp, samples);  // GX2 counts samples twice
    out = {size, s.pitch, s.height, s.slices, tm, a.base, a.pitch, a.height, bpp};
}

void info_micro(SurfaceInfo& out, uint32_t tm, uint32_t bpp, uint32_t samples, Shape s, uint32_t level,
                uint32_t pad_dims, bool cube, bool scan) {
    if (level) {
        mip_shape(s, cube, pad_dims);
        if (tm == tile::k1DThick && s.slices < 4) tm = tile::k1DThin1;
    }
    const uint32_t thick = thickness(tm);
    Alignment a{kGroupBytes, std::max(8u, kGroupBytes / layout_bpp(bpp) / samples / thick), 8};
    if (scan) a.pitch = align_up(a.pitch, 32);
    pad(s, pad_dims, cube, thick > 1, a.pitch, a.height, thick);
    out = {bytes(s, bpp, samples), s.pitch, s.height, s.slices, tm, a.base, a.pitch, a.height, bpp};
}

void info_macro(SurfaceInfo& out, uint32_t tm, uint32_t base_tm, uint32_t bpp, uint32_t samples, Shape s,
                uint32_t level, uint32_t pad_dims, bool cube, bool scan) {
    const Shape in = s;
    uint32_t used_tm = tm;
    if (level) {
        mip_shape(s, cube, pad_dims);
        if (tm == tile::k2DThick && s.slices < 4) used_tm = tile::k2DThin1;
    }
    // a THICK chain whose level went THIN falls back to 1D when it is narrower than the THICK macro tile
    if (level && tm != base_tm && is_thick_macro(base_tm) && !is_thick_macro(tm)) {
        const Alignment b = macro_alignment(base_tm, bpp, samples);
        if (s.pitch < b.pitch * std::max(kGroupBytes / 8 / bpp, 1u) || s.height < b.height) {
            info_micro(out, tile::k1DThin1, bpp, samples, in, level, pad_dims, cube, scan);
            return;
        }
    }
    Alignment a = macro_alignment(tm, bpp, samples);  // the scan-buffer pitch rule does not apply to macro tiling
    a.pitch = std::max(a.pitch, bank_swap_width(tm, bpp, samples, in.pitch));
    pad(s, pad_dims, cube, thickness(tm) > 1, a.pitch, a.height, thickness(used_tm));
    out = {bytes(s, bpp, samples), s.pitch, s.height, s.slices, used_tm, a.base, a.pitch, a.height, bpp};
}

// GX2's unpadded linear mode: rows of whole elements, nothing aligned.
SurfaceInfo info_linear_special(const SurfaceParams& p, uint32_t level) {
    SurfaceInfo out;
    const uint32_t block = is_block_compressed(p.format) ? 4 : 1;
    out.bpp = format_bits(p.format);
    out.base_align = out.pitch_align = out.height_align = 1;
    out.pitch = std::max(align_up(p.width >> level, block) / block, 1u);
    uint32_t height = 0;
    switch (p.dim) {
        case dim::k1D: height = 1; out.depth = 1; break;
        case dim::k2D: height = std::max(p.height >> level, 1u); out.depth = 1; break;
        case dim::k3D: height = std::max(p.height >> level, 1u); out.depth = std::max(p.depth >> level, 1u); break;
        case dim::kCube: height = std::max(p.height >> level, 1u); out.depth = std::max(p.depth, 6u); break;
        case dim::k1DArray: height = 1; out.depth = p.depth; break;
        case dim::k2DArray: height = std::max(p.height >> level, 1u); out.depth = p.depth; break;
        default: break;
    }
    out.height = std::max(align_up(height, block) / block, 1u);
    const uint32_t samples = 1u << p.aa;
    out.size = out.bpp * samples * out.depth * out.height * out.pitch / 8;  // in bits first, 32-bit as in GX2
    return out;
}

// Element index inside an 8x8 micro tile (THICK: 8x8x4). Bit order depends on the element size.
uint32_t micro_tile_index(uint32_t x, uint32_t y, uint32_t z, uint32_t bpp, bool thick) {
    const uint32_t x0 = x & 1, x1 = (x >> 1) & 1, x2 = (x >> 2) & 1, y0 = y & 1, y1 = (y >> 1) & 1, y2 = (y >> 2) & 1;
    uint32_t i;
    switch (bpp) {
        case 8: i = x0 | x1 << 1 | x2 << 2 | y1 << 3 | y0 << 4 | y2 << 5; break;
        case 16: i = x0 | x1 << 1 | x2 << 2 | y0 << 3 | y1 << 4 | y2 << 5; break;
        case 64: i = x0 | y0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5; break;
        case 128: i = y0 | x0 << 1 | x1 << 2 | x2 << 3 | y1 << 4 | y2 << 5; break;
        default: i = x0 | x1 << 1 | y0 << 2 | x2 << 3 | y1 << 4 | y2 << 5; break;  // 32 bpp and others
    }
    if (thick) i |= (z & 3) << 6;
    return i;
}

}  // namespace

bool is_macro_tiled(uint32_t hw_tile_mode) { return hw_tile_mode >= tile::k2DThin1; }

bool is_block_compressed(uint32_t format) { return (format & 0x3F) >= 0x31 && (format & 0x3F) <= 0x35; }

uint32_t format_bits(uint32_t format) {
    const uint32_t f = format & 0x3F;
    if (f >= 0x01 && f <= 0x03) return 8;     // 8, 4_4, 3_3_2
    if (f >= 0x05 && f <= 0x0C) return 16;    // 16, 16F, 8_8, 5_6_5, 6_5_5, 1_5_5_5, 4_4_4_4, 5_5_5_1
    if (f >= 0x0D && f <= 0x1B) return 32;    // 32, 32F, 16_16(F), 8_24(F), 24_8(F), 10_11_11(F), 11_11_10(F), ...
    if (f >= 0x1C && f <= 0x20) return 64;    // X24_8_32F, 32_32(F), 16_16_16_16(F)
    if (f == 0x22 || f == 0x23) return 128;   // 32_32_32_32(F)
    if (f == 0x31 || f == 0x34) return 64;    // BC1, BC4
    if (f == 0x32 || f == 0x33 || f == 0x35) return 128;  // BC2, BC3, BC5
    return 0;  // includes the vertex-only 3-component formats
}

SurfaceInfo surface_info(const SurfaceParams& p, uint32_t level) {
    if (p.tile_mode == tile::kLinearSpecial) return info_linear_special(p, level);
    SurfaceInfo out;
    uint32_t bpp = format_bits(p.format);
    if (!bpp) return out;
    const uint32_t base_tm = p.tile_mode & 0xF, samples = 1u << p.aa;
    const bool cube = p.dim == dim::kCube;
    Shape s{std::max(p.width >> level, 1u), 0, 0};
    const uint32_t h = std::max(p.height >> level, 1u);
    switch (p.dim) {
        case dim::k1D: s.height = 1; s.slices = 1; break;
        case dim::k2D: case dim::k2DMsaa: s.height = h; s.slices = 1; break;
        case dim::k3D: s.height = h; s.slices = std::max(p.depth >> level, 1u); break;
        case dim::kCube: s.height = h; s.slices = std::max(p.depth, 6u); break;
        case dim::k1DArray: s.height = 1; s.slices = p.depth; break;
        case dim::k2DArray: case dim::k2DArrayMsaa: s.height = h; s.slices = p.depth; break;
        default: break;
    }
    if (is_block_compressed(p.format)) {
        // in 4x4 blocks: the base level rounds up, mips round to a power of two first
        s.pitch = std::max((level ? pow2_ceil(s.pitch) : align_up(s.pitch, 4)) / 4, 1u);
        s.height = std::max((level ? pow2_ceil(s.height) : align_up(s.height, 4)) / 4, 1u);
    }
    const uint32_t pad_dims = cube && !level ? 2 : 0;
    const uint32_t tm = level_tile_mode(base_tm, bpp, level, s.pitch, s.height, s.slices, samples, p.depth_buffer);
    if (tm <= tile::kLinearAligned) info_linear(out, tm, bpp, samples, s, level, pad_dims, cube, p.scan_buffer);
    else if (!is_macro_tiled(tm)) info_micro(out, tm, bpp, samples, s, level, pad_dims, cube, p.scan_buffer);
    else info_macro(out, tm, base_tm, bpp, samples, s, level, pad_dims, cube, p.scan_buffer);
    return out;
}

uint32_t mip_offset(const SurfaceParams& p, uint32_t hw_tile_mode, uint32_t swizzle, uint32_t level) {
    SurfaceParams q = p;
    q.tile_mode = hw_tile_mode;
    q.aa = 0;
    q.depth_buffer = q.scan_buffer = false;
    uint32_t offset = 0, prev_size = 0, prev_tm = hw_tile_mode;
    for (uint32_t l = 1; l <= level; ++l) {
        const SurfaceInfo info = surface_info(q, l);
        uint32_t padding = 0;
        if (l > 1 && is_macro_tiled(prev_tm) && !is_macro_tiled(info.tile_mode)) padding = swizzle & 0xFFFF;
        padding += (info.base_align - offset % info.base_align) % info.base_align;
        offset += padding + prev_size;
        prev_tm = info.tile_mode;
        prev_size = static_cast<uint32_t>(info.size);
    }
    return offset;
}

ElementAddresser::ElementAddresser(const SurfaceInfo& level, uint32_t slice, uint32_t pipe_swizzle, uint32_t bank_swizzle)
    : tile_mode_(level.tile_mode), bpp_(level.bpp), pitch_(level.pitch), height_(level.height), slice_(slice) {
    if (tile_mode_ <= tile::kLinearAligned) return;
    thickness_ = thickness(tile_mode_);
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 8; ++x)
            element_index_[y * 8 + x] = static_cast<uint8_t>(micro_tile_index(x, y, slice, bpp_, thickness_ > 1));
    const auto slice_bytes = static_cast<uint32_t>((uint64_t{height_} * pitch_ * thickness_ * bpp_ + 7) / 8);
    slice_offset_ = slice_bytes * (slice / thickness_);
    if (!is_macro_tiled(tile_mode_)) {
        micro_tile_bytes_ = thickness_ * bpp_ * 8;
        return;
    }
    macro_w_ = macro_width(tile_mode_);
    macro_h_ = macro_height(tile_mode_);
    macro_tile_bytes_ = (thickness_ * bpp_ * macro_h_ * macro_w_ + 7) / 8;
    bank_swap_width_ = bank_swap_width(tile_mode_, bpp_, 1, pitch_);
    const uint32_t rotated_slice = is_thick_macro(tile_mode_) ? slice / 4 : slice;
    bank_pipe_xor_ = pipe_swizzle + kPipes * bank_swizzle + rotated_slice * slice_rotation(tile_mode_);
}

uint32_t ElementAddresser::operator()(uint32_t x, uint32_t y) const {
    if (tile_mode_ <= tile::kLinearAligned) return (x + pitch_ * y + slice_ * height_ * pitch_) * bpp_ / 8;
    const uint32_t element = element_index_[(y & 7) * 8 + (x & 7)] * bpp_ / 8;
    if (!is_macro_tiled(tile_mode_))
        return micro_tile_bytes_ * ((x >> 3) + (pitch_ >> 3) * (y >> 3)) + slice_offset_ + element;

    // Micro tiles are spread over pipes (by x3 ^ y3) and banks (by x3 ^ y5, x4 ^ y4), then swizzled.
    uint32_t pipe = ((x >> 3) ^ (y >> 3)) & 1;
    uint32_t bank = (((x >> 3) ^ (y >> 5)) & 1) | ((((x >> 4) ^ (y >> 4)) & 1) << 1);
    const uint32_t bank_pipe = ((pipe + kPipes * bank) ^ bank_pipe_xor_) % (kPipes * kBanks);
    pipe = bank_pipe % kPipes;
    bank = bank_pipe / kPipes;
    const uint32_t tile_x = x / macro_w_;
    if (bank_swap_width_) {
        static constexpr uint32_t kSwapOrder[kBanks] = {0, 1, 3, 2};
        bank ^= kSwapOrder[(macro_w_ * tile_x / bank_swap_width_) % kBanks];
    }
    const uint32_t macro_offset = (tile_x + pitch_ / macro_w_ * (y / macro_h_)) * macro_tile_bytes_;
    // byte offset within the pipe/bank channel, then the pipe and bank bits inserted above the 256-byte group
    const uint32_t channel_offset = ((macro_offset + slice_offset_) >> (kPipeBits + kBankBits)) + element;
    const uint32_t group_mask = kGroupBytes - 1;
    return ((channel_offset & ~group_mask) << (kPipeBits + kBankBits)) | (channel_offset & group_mask) |
           (pipe << kGroupBits) | (bank << (kGroupBits + kPipeBits));
}

}  // namespace mh3u::gfx::addrlib
