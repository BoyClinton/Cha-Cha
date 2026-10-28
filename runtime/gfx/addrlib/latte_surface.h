// Latte (R7xx) surface layout for the Wii U's memory configuration (2 pipes, 4 banks, 256-byte pipe interleave):
// per-level sizes and alignment as GX2's address library computes them, mip-chain offsets, and element addresses
// for every tile mode. Written for this project; checked against the earlier vendored addrlib by addrlib_test.
#pragma once

#include <cstdint>

namespace mh3u::gfx::addrlib {

// Hardware tile modes (GX2TileMode 0-15). GX2 adds two software values on top.
namespace tile {
constexpr uint32_t kLinearGeneral = 0, kLinearAligned = 1, k1DThin1 = 2, k1DThick = 3;
constexpr uint32_t k2DThin1 = 4, k2DThin2 = 5, k2DThin4 = 6, k2DThick = 7;
constexpr uint32_t k2BThin1 = 8, k2BThin2 = 9, k2BThin4 = 10, k2BThick = 11;
constexpr uint32_t k3DThin1 = 12, k3DThick = 13, k3BThin1 = 14, k3BThick = 15;
constexpr uint32_t kLinearSpecial = 16, k32Special = 32;  // GX2-only: unpadded linear / "let GX2 pick"
}  // namespace tile

// GX2SurfaceDim
namespace dim {
constexpr uint32_t k1D = 0, k2D = 1, k3D = 2, kCube = 3, k1DArray = 4, k2DArray = 5, k2DMsaa = 6, k2DArrayMsaa = 7;
}  // namespace dim

constexpr uint32_t kFormatNV12 = 0x81;  // GX2_SURFACE_FORMAT_NV12_UNORM (two planes)

bool is_macro_tiled(uint32_t hw_tile_mode);       // 2D/2B/3D/3B modes (>= 4)
bool is_block_compressed(uint32_t format);        // BC1-BC5 (GX2 format, low 6 bits)
uint32_t format_bits(uint32_t format);            // bits per element (per 4x4 block for BC); 0 = no memory layout

// What GX2 needs to size one level of a surface.
struct SurfaceParams {
    uint32_t format = 0, width = 0, height = 0, depth = 1, dim = dim::k2D;
    uint32_t tile_mode = tile::k2DThin1;  // GX2TileMode
    uint32_t aa = 0;                      // log2 of the sample count
    bool depth_buffer = false;            // GX2_SURFACE_USE_DEPTH_BUFFER
    bool scan_buffer = false;             // GX2_SURFACE_USE_SCAN_BUFFER
};

// Layout of one level, in elements (4x4 blocks for BC formats). All zero if the format has no element size.
struct SurfaceInfo {
    uint64_t size = 0;  // bytes, all slices and samples
    uint32_t pitch = 0, height = 0, depth = 0;
    uint32_t tile_mode = 0;  // hardware tile mode this level really uses (small levels drop to 1D)
    uint32_t base_align = 0, pitch_align = 0, height_align = 0;
    uint32_t bpp = 0;  // bits per element
};
SurfaceInfo surface_info(const SurfaceParams& p, uint32_t level);

// Byte offset of mip level `level` (>= 1) from the mip-chain pointer, which points at level 1. `swizzle` is
// GX2Surface::swizzle: its low 16 bits pad the chain where it leaves macro tiling. Uses p.aa = 0.
uint32_t mip_offset(const SurfaceParams& p, uint32_t hw_tile_mode, uint32_t swizzle, uint32_t level);

// Byte address of element (x, y) of one slice of a level, relative to the level's base, single-sampled colour.
class ElementAddresser {
public:
    ElementAddresser(const SurfaceInfo& level, uint32_t slice, uint32_t pipe_swizzle, uint32_t bank_swizzle);
    uint32_t operator()(uint32_t x, uint32_t y) const;

private:
    uint32_t tile_mode_, bpp_, pitch_, height_, slice_;
    uint32_t thickness_ = 1;
    uint32_t slice_offset_ = 0;      // bytes before this slice (micro/macro tiled)
    uint32_t micro_tile_bytes_ = 0;  // 1D tiled
    uint32_t macro_w_ = 0, macro_h_ = 0, macro_tile_bytes_ = 0, bank_swap_width_ = 0;
    uint32_t bank_pipe_xor_ = 0;     // swizzle and per-slice rotation, as (bank << 1) | pipe
    uint8_t element_index_[64] = {};  // element index inside a micro tile, by (y & 7) * 8 + (x & 7)
};

}  // namespace mh3u::gfx::addrlib
