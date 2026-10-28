// GX2 surface sizing (GX2CalcSurfaceSizeAndAlignment) and per-level element addressing, on top of latte_surface.h.
#pragma once

#include <cstdint>
#include <vector>

namespace mh3u::gfx::addrlib {

// Host copy of the GX2Surface fields that sizing reads and writes (guest offsets in runtime/gfx/gx2.cpp).
struct SurfaceDesc {
    uint32_t dim = 1, width = 0, height = 0, depth = 1, levels = 1, format = 0, aa = 0, use = 0;
    uint32_t image_size = 0, mip_size = 0, tile_mode = 0, swizzle = 0, alignment = 0, pitch = 0;
    uint32_t mip_offset[13] = {};
};

// GX2CalcSurfaceSizeAndAlignment: fills image/mip sizes, alignment, pitch, mip offsets, final tile mode, swizzle.
void calc_size_and_alignment(SurfaceDesc& s);

// One mip level / slice of a surface, in elements (4x4 blocks for BC formats).
struct Level {
    uint32_t width = 0, height = 0, bytes = 0;  // dimensions and bytes per element
    uint32_t pitch = 0;                         // hardware pitch of the level, in elements
    uint32_t base = 0;                          // guest address of the level (image, or mipmaps + level offset)
    uint32_t span = 0;                          // bytes from base the level's elements may touch (for bounds checks)
    const std::vector<uint32_t>* offsets = nullptr;  // byte offset of each element (row-major) from base
};
// Layout of level `level`, slice `slice` of a surface whose image / mipmaps are at the given guest addresses
// (the hardware swizzle of a macro-tiled mip chain lives in bits 8-10 of the mipmaps pointer). False if the
// surface's format has no element size.
bool level_layout(const SurfaceDesc& s, uint32_t image, uint32_t mipmaps, uint32_t level, uint32_t slice, Level& out);

}  // namespace mh3u::gfx::addrlib
