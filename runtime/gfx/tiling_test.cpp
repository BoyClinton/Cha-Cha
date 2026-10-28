// Tiling addresses against scripts/mt_tex.py (whose addressing decodes all 17,244 game textures), plus a
// detile/retile round trip.
#include "tiling.h"

#include <cstdio>
#include <vector>

using namespace mh3u::gfx;

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

int main() {
    // x, y, bpp, pitch, height, slice, pipe swizzle, bank swizzle, expected (from mt_tex.addr_2d_thin1)
    const uint32_t c2d[][9] = {
        {0, 0, 32, 64, 16, 0, 0, 0, 0x0},          {5, 3, 32, 64, 32, 0, 0, 0, 0x74},
        {37, 21, 32, 128, 64, 0, 0, 0, 0x2CB4},     {100, 77, 32, 1280, 736, 0, 0, 0, 0x519B0},
        {1279, 719, 32, 1280, 720, 0, 0, 0, 0x383EFC}, {13, 9, 64, 64, 32, 0, 0, 0, 0x258},
        {40, 30, 16, 64, 32, 0, 1, 2, 0xBE0},       {7, 7, 8, 32, 16, 0, 0, 0, 0x3F},
        {33, 17, 128, 64, 32, 1, 0, 0, 0xE630},
    };
    for (auto& c : c2d) {
        uint32_t a = addr_2d_thin1(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
        if (a != c[8]) std::printf("  2d (%u,%u) bpp %u: got 0x%X want 0x%X\n", c[0], c[1], c[2], a, c[8]);
        CHECK(a == c[8]);
    }
    const uint32_t c1d[][6] = {{0, 0, 32, 8, 8, 0x0}, {9, 3, 32, 16, 8, 0x154}, {15, 15, 64, 16, 16, 0x7F8}, {3, 5, 8, 8, 8, 0x33}};
    for (auto& c : c1d) CHECK(addr_1d_thin1(c[0], c[1], c[2], c[3], c[4], 0) == c[5]);

    // round trip on a 2D-tiled 100x50 RGBA8 surface (pitch padded to 128, height to 64)
    SurfaceLevel s;
    s.width = 100;
    s.height = 50;
    s.bpp = 32;
    s.tile_mode = 4;
    s.pitch = 128;
    std::vector<uint8_t> lin(100 * 50 * 4), tiled(128 * 64 * 4), back(lin.size());
    for (size_t i = 0; i < lin.size(); ++i) lin[i] = static_cast<uint8_t>(i * 7 + 3);
    retile(lin.data(), tiled.data(), s);
    detile(tiled.data(), back.data(), s);
    CHECK(back == lin);
    // offsets are distinct (a permutation into the padded surface)
    std::vector<uint8_t> seen(tiled.size() / 4);
    bool unique = true;
    for (uint32_t o : element_offsets(s)) unique &= !seen[o / 4]++;
    CHECK(unique);

    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
