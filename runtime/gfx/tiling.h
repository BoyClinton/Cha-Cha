// Latte (R6xx addrlib) surface addressing for the THIN1 tile modes the game uses, ported from scripts/mt_tex.py
// (verified there against all 17,244 game textures): 2 pipes, 4 banks, 256-byte pipe interleave.
// Coordinates are in elements (4x4 blocks for BC formats), bpp in bits.
#pragma once

#include <cstdint>
#include <vector>

namespace mh3u::gfx {

uint32_t addr_1d_thin1(uint32_t x, uint32_t y, uint32_t bpp, uint32_t pitch, uint32_t height, uint32_t slice);
uint32_t addr_2d_thin1(uint32_t x, uint32_t y, uint32_t bpp, uint32_t pitch, uint32_t height, uint32_t slice,
                       uint32_t pipe_swizzle, uint32_t bank_swizzle);

// One mip level / slice of a GX2 surface, as needed to (de)tile it.
struct SurfaceLevel {
    uint32_t width = 0, height = 0;  // in elements
    uint32_t bpp = 32;               // bits per element
    uint32_t tile_mode = 4;          // GX2TileMode: 0/1 linear, 2 1D thin1, 4 2D thin1 (others treated as 2D thin1)
    uint32_t pitch = 0;              // in elements (GX2Surface::pitch)
    uint32_t swizzle = 0;            // GX2Surface::swizzle: pipe swizzle bit 8, bank swizzle bits 9-10 (assumed)
    uint32_t slice = 0;
};

// Byte offset of every element (row-major, width x height) inside the tiled data; cached per surface shape.
const std::vector<uint32_t>& element_offsets(const SurfaceLevel& s);

// tiled -> linear (dst: width*height*bpp/8 bytes, row-major) and back.
void detile(const uint8_t* tiled, uint8_t* linear, const SurfaceLevel& s);
void retile(const uint8_t* linear, uint8_t* tiled, const SurfaceLevel& s);

}  // namespace mh3u::gfx
