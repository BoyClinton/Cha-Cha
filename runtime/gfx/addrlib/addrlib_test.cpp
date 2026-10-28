// Surface layout regression test:
// - the addrlib_sweep results (597,700 cases: per-level sizing, GX2CalcSurfaceSizeAndAlignment, and the element
//   offsets of every level of 6,000 surfaces) must hash to the value the previously vendored Cemu addrlib produced
//   for the same sweep (2026-10-24, before it was replaced by latte_surface.cpp);
// - 1D/2D THIN1 addresses must match runtime/gfx/tiling.cpp, which was checked against all game textures.
#include <cstdio>

#include "addrlib_sweep.h"
#include "gx2_surface.h"
#include "latte_surface.h"
#include "tiling.h"

using namespace mh3u::gfx;
namespace sw = mh3u::gfx::addrlib::sweep;

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

constexpr uint64_t kSweepHash = 0x0F359C48B73370F2ull;

void copy(const sw::Desc& a, addrlib::SurfaceDesc& b) {
    b = {a.dim, a.width, a.height, a.depth, a.levels, a.format, a.aa, a.use, a.image_size, a.mip_size, a.tile_mode,
         a.swizzle, a.alignment, a.pitch};
    for (int i = 0; i < 13; ++i) b.mip_offset[i] = a.mip_offset[i];
}

struct Ours {
    static sw::Info info(uint32_t format, uint32_t w, uint32_t h, uint32_t d, uint32_t dim, uint32_t tm, uint32_t aa,
                         uint32_t level, bool depth_buffer, bool scan_buffer) {
        const addrlib::SurfaceInfo o =
            addrlib::surface_info({format, w, h, d, dim, tm, aa, depth_buffer, scan_buffer}, level);
        return {o.size, o.pitch, o.height, o.depth, o.tile_mode, o.base_align, o.pitch_align, o.height_align};
    }
    static void calc(sw::Desc& d) {
        addrlib::SurfaceDesc s;
        copy(d, s);
        addrlib::calc_size_and_alignment(s);
        d = {s.dim, s.width, s.height, s.depth, s.levels, s.format, s.aa, s.use, s.image_size, s.mip_size, s.tile_mode,
             s.swizzle, s.alignment, s.pitch, {}};
        for (int i = 0; i < 13; ++i) d.mip_offset[i] = s.mip_offset[i];
    }
    static void layout(const sw::Desc& d, uint32_t image, uint32_t mipmaps, uint32_t level, uint32_t slice,
                       sw::LevelOut& l) {
        addrlib::SurfaceDesc s;
        copy(d, s);
        addrlib::Level o;
        l.ok = addrlib::level_layout(s, image, mipmaps, level, slice, o);
        l = {l.ok, o.width, o.height, o.bytes, o.pitch, o.base, o.span, o.offsets};
    }
};

struct Hasher {
    uint64_t hash = 0xCBF29CE484222325ull;
    size_t values = 0;
    void begin(const sw::Case&) {}
    void add(uint64_t v) {
        hash = (hash ^ v) * 0x100000001B3ull;
        ++values;
    }
};

}  // namespace

int main() {
    Hasher h;
    sw::run<Ours>(h);
    std::printf("sweep: %zu values, hash 0x%016llX\n", h.values, static_cast<unsigned long long>(h.hash));
    CHECK(h.hash == kSweepHash);

    sw::Rng rng;
    for (int i = 0; i < 20000; ++i) {
        const uint32_t bpps[] = {8, 16, 32, 64, 128};
        addrlib::SurfaceInfo level;
        level.bpp = rng.pick(bpps);
        level.pitch = 32 * (1 + rng.below(40));
        level.height = 16 * (1 + rng.below(40));
        const uint32_t x = rng.below(level.pitch), y = rng.below(level.height), slice = rng.below(4);
        const uint32_t pipe_swizzle = rng.below(2), bank_swizzle = rng.below(4);
        level.tile_mode = addrlib::tile::k2DThin1;
        CHECK(addrlib::ElementAddresser(level, slice, pipe_swizzle, bank_swizzle)(x, y) ==
              addr_2d_thin1(x, y, level.bpp, level.pitch, level.height, slice, pipe_swizzle, bank_swizzle));
        level.tile_mode = addrlib::tile::k1DThin1;
        CHECK(addrlib::ElementAddresser(level, slice, 0, 0)(x, y) ==
              addr_1d_thin1(x, y, level.bpp, level.pitch, level.height, slice));
        if (failures > 10) break;
    }

    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
