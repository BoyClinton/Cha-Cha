// Deterministic sweep over surface parameters for addrlib_test: sizes every level of many surfaces, runs
// GX2CalcSurfaceSizeAndAlignment and lays out levels, feeding every result into a sink. `Impl` adapts an
// implementation (ours, or an older one when checking a rewrite) to the calls below.
#pragma once

#include <cstdint>
#include <vector>

namespace mh3u::gfx::addrlib::sweep {

struct Info {  // the SurfaceInfo fields GX2 code reads
    uint64_t size;
    uint32_t pitch, height, depth, tile_mode, base_align, pitch_align, height_align;
};

struct Desc {  // SurfaceDesc
    uint32_t dim, width, height, depth, levels, format, aa, use, image_size, mip_size, tile_mode, swizzle, alignment,
        pitch;
    uint32_t mip_offset[13];
};

struct LevelOut {
    bool ok;
    uint32_t width, height, bytes, pitch, base, span;
    const std::vector<uint32_t>* offsets;
};

// What the sink is told before each group of results, so a mismatch can be traced to its inputs.
struct Case {
    const char* kind;
    uint32_t format, width, height, depth, dim, tile_mode, aa, level, flags, swizzle, slice;
};

class Rng {  // xorshift32
public:
    uint32_t next() {
        s_ ^= s_ << 13;
        s_ ^= s_ >> 17;
        s_ ^= s_ << 5;
        return s_;
    }
    uint32_t below(uint32_t n) { return next() % n; }
    template <size_t N>
    uint32_t pick(const uint32_t (&v)[N]) { return v[below(N)]; }

private:
    uint32_t s_ = 0x4D483355;  // "MH3U"
};

// Formats with an element size, a few with GX2's extra bits, invalid, and NV12.
inline constexpr uint32_t kFormats[] = {0x00, 0x01, 0x02, 0x05, 0x07, 0x08, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x10, 0x11,
                                        0x16, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x22, 0x23, 0x31, 0x32,
                                        0x33, 0x34, 0x35, 0x41A, 0x431, 0x816, 0x81E, 0x811, 0x235, 0x81};
inline constexpr uint32_t kSizes[] = {0,  1,   2,   3,   4,   5,   6,   7,   8,   9,   12,  15,  16,   17,   24,  31,
                                      32, 33,  48,  63,  64,  65,  100, 127, 128, 129, 200, 255, 256,  257,  320, 480,
                                      512, 640, 720, 854, 1024, 1080, 1280, 1920, 2048, 4096};
inline constexpr uint32_t kDepths[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 33};
inline constexpr uint32_t kTileModes[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 32};
inline constexpr uint32_t kUses[] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x8, 0xA, 0x0};

template <class Impl, class Sink>
void run(Sink& sink) {
    Rng rng;
    auto size = [&] { return rng.below(4) ? rng.pick(kSizes) : 1 + rng.below(2048); };

    for (int i = 0; i < 400000; ++i) {
        Case c{"surface_info", rng.pick(kFormats), size(), size(), rng.pick(kDepths), rng.below(8),
               rng.pick(kTileModes), rng.below(4), rng.below(4) ? rng.below(4) : rng.below(13), rng.below(4), 0, 0};
        if (c.format == 0x81) c.format = 0x1A;  // NV12 is only special to GX2's size calculation
        sink.begin(c);
        const Info r = Impl::info(c.format, c.width, c.height, c.depth, c.dim, c.tile_mode, c.aa, c.level,
                                  (c.flags & 1) != 0, (c.flags & 2) != 0);
        for (uint64_t v : {r.size, uint64_t{r.pitch}, uint64_t{r.height}, uint64_t{r.depth}, uint64_t{r.tile_mode},
                           uint64_t{r.base_align}, uint64_t{r.pitch_align}, uint64_t{r.height_align}})
            sink.add(v);
    }

    for (int i = 0; i < 100000; ++i) {
        Desc d{};
        d.dim = rng.below(8);
        d.width = size();
        d.height = size();
        d.depth = rng.pick(kDepths);
        d.levels = rng.below(15);
        d.format = rng.pick(kFormats);
        d.aa = rng.below(5) ? 0 : rng.below(4);
        d.use = rng.pick(kUses);
        d.tile_mode = rng.pick(kTileModes);
        d.swizzle = rng.next();
        for (uint32_t& m : d.mip_offset) m = rng.next();
        if (!(d.format & 0x3F)) d.levels = 1;  // invalid format: no alignment to chain mips with
        sink.begin({"calc", d.format, d.width, d.height, d.depth, d.dim, d.tile_mode, d.aa, d.levels, d.use, d.swizzle, 0});
        Impl::calc(d);
        for (uint32_t v : {d.levels, d.image_size, d.mip_size, d.tile_mode, d.swizzle, d.alignment, d.pitch}) sink.add(v);
        for (uint32_t m : d.mip_offset) sink.add(m);
    }

    // Layouts of small surfaces (every level, a few slices), as the texture loader asks for them.
    for (int i = 0; i < 6000; ++i) {
        Desc d{};
        d.dim = rng.below(3) ? 1 : rng.below(8);
        d.width = 1 + rng.below(rng.below(4) ? 160 : 520);
        d.height = 1 + rng.below(rng.below(4) ? 160 : 300);
        d.depth = 1 + rng.below(9);
        d.levels = 1 + rng.below(10);
        d.format = rng.pick(kFormats);
        d.use = rng.pick(kUses);
        d.tile_mode = rng.pick(kTileModes);
        d.swizzle = rng.next() & 0x700;
        if (!(d.format & 0x3F)) d.levels = 1;
        sink.begin({"layout calc", d.format, d.width, d.height, d.depth, d.dim, d.tile_mode, 0, d.levels, d.use, d.swizzle, 0});
        Impl::calc(d);
        const uint32_t mipmaps = 0x20000000u | (rng.next() & 0x700);
        for (uint32_t level = 0; level < d.levels; ++level)
            for (uint32_t slice : {0u, 1u, 2u, 5u}) {
                if (slice && slice >= d.depth) break;
                sink.begin({"layout", d.format, d.width, d.height, d.depth, d.dim, d.tile_mode, 0, level, d.use,
                            d.swizzle, slice});
                LevelOut l{};
                Impl::layout(d, 0x10000000u, mipmaps, level, slice, l);
                sink.add(l.ok);
                if (!l.ok) continue;
                for (uint32_t v : {l.width, l.height, l.bytes, l.pitch, l.base, l.span}) sink.add(v);
                uint64_t h = 0xCBF29CE484222325ull;
                for (uint32_t o : *l.offsets) h = (h ^ o) * 0x100000001B3ull;
                sink.add(h);
            }
    }
}

}  // namespace mh3u::gfx::addrlib::sweep
