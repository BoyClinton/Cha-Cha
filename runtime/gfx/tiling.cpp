#include "tiling.h"

#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

namespace mh3u::gfx {
namespace {

constexpr uint32_t kPipes = 2, kBanks = 4, kPipeInterleaveBits = 8, kPipeBits = 1, kBankBits = 2;

uint32_t pixel_index_in_micro_tile(uint32_t x, uint32_t y, uint32_t bpp) {
    uint32_t b[6];
    switch (bpp) {
        case 8: b[0] = x & 1; b[1] = (x >> 1) & 1; b[2] = (x >> 2) & 1; b[3] = (y >> 1) & 1; b[4] = y & 1; b[5] = (y >> 2) & 1; break;
        case 16: b[0] = x & 1; b[1] = (x >> 1) & 1; b[2] = (x >> 2) & 1; b[3] = y & 1; b[4] = (y >> 1) & 1; b[5] = (y >> 2) & 1; break;
        case 64: b[0] = x & 1; b[1] = y & 1; b[2] = (x >> 1) & 1; b[3] = (x >> 2) & 1; b[4] = (y >> 1) & 1; b[5] = (y >> 2) & 1; break;
        case 128: b[0] = y & 1; b[1] = x & 1; b[2] = (x >> 1) & 1; b[3] = (x >> 2) & 1; b[4] = (y >> 1) & 1; b[5] = (y >> 2) & 1; break;
        default: b[0] = x & 1; b[1] = (x >> 1) & 1; b[2] = y & 1; b[3] = (x >> 2) & 1; b[4] = (y >> 1) & 1; b[5] = (y >> 2) & 1; break;
    }
    uint32_t v = 0;
    for (int i = 0; i < 6; ++i) v |= b[i] << i;
    return v;
}

}  // namespace

uint32_t addr_1d_thin1(uint32_t x, uint32_t y, uint32_t bpp, uint32_t pitch, uint32_t height, uint32_t slice) {
    const uint32_t micro_tile_bytes = 64 * bpp / 8;
    const uint32_t micro_offset = micro_tile_bytes * ((x >> 3) + (y >> 3) * (pitch >> 3));
    const uint32_t slice_offset = slice * (pitch * height * bpp / 8);
    return slice_offset + micro_offset + ((bpp * pixel_index_in_micro_tile(x, y, bpp)) >> 3);
}

uint32_t addr_2d_thin1(uint32_t x, uint32_t y, uint32_t bpp, uint32_t pitch, uint32_t height, uint32_t slice,
                       uint32_t pipe_swizzle, uint32_t bank_swizzle) {
    const uint32_t elem_offset = (bpp * pixel_index_in_micro_tile(x, y, bpp)) >> 3;
    uint32_t pipe = ((y >> 3) ^ (x >> 3)) & 1;
    uint32_t bank = (((y / (16 * kPipes)) ^ (x >> 3)) & 1) | ((((y / (8 * kPipes)) ^ (x >> 4)) & 1) << 1);
    uint32_t bank_pipe = pipe + kPipes * bank;
    const uint32_t rotation = kPipes * ((kBanks >> 1) - 1);
    bank_pipe ^= pipe_swizzle + kPipes * bank_swizzle + slice * rotation;
    bank_pipe %= kPipes * kBanks;
    pipe = bank_pipe % kPipes;
    bank = bank_pipe / kPipes;
    const uint32_t macro_w = 8 * kBanks, macro_h = 8 * kPipes;
    const uint32_t macro_tile_bytes = bpp * macro_w * macro_h / 8;
    const uint32_t macro_offset = ((x / macro_w) + (pitch / macro_w) * (y / macro_h)) * macro_tile_bytes;
    const uint32_t slice_offset = slice * (pitch * height * bpp / 8);
    const uint32_t swizzle_bits = kPipeBits + kBankBits;
    const uint32_t total = elem_offset + ((macro_offset + slice_offset) >> swizzle_bits);
    const uint32_t group_mask = (1u << kPipeInterleaveBits) - 1;
    return (bank << (kPipeBits + kPipeInterleaveBits)) | (pipe << kPipeInterleaveBits) | (total & group_mask) |
           ((total & ~group_mask) << swizzle_bits);
}

const std::vector<uint32_t>& element_offsets(const SurfaceLevel& s) {
    using Key = std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>;
    static std::mutex m;
    static std::map<Key, std::vector<uint32_t>> cache;
    const Key key{s.width, s.height, s.bpp, s.tile_mode, s.pitch, s.swizzle, s.slice};
    std::lock_guard g(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::vector<uint32_t> off(size_t{s.width} * s.height);
    const uint32_t pitch = s.pitch ? s.pitch : s.width;
    const uint32_t bytes = s.bpp / 8;
    const uint32_t pipe_swz = (s.swizzle >> 8) & 1, bank_swz = (s.swizzle >> 9) & 3;
    for (uint32_t y = 0; y < s.height; ++y) {
        for (uint32_t x = 0; x < s.width; ++x) {
            uint32_t a;
            if (s.tile_mode <= 1) {
                a = (y * pitch + x) * bytes;
            } else if (s.tile_mode == 2 || s.tile_mode == 3) {
                const uint32_t h = (s.height + 7) & ~7u;
                a = addr_1d_thin1(x, y, s.bpp, pitch, h, s.slice);
            } else {
                const uint32_t h = (s.height + 15) & ~15u;
                a = addr_2d_thin1(x, y, s.bpp, pitch, h, s.slice, pipe_swz, bank_swz);
            }
            off[size_t{y} * s.width + x] = a;
        }
    }
    return cache.emplace(key, std::move(off)).first->second;
}

void detile(const uint8_t* tiled, uint8_t* linear, const SurfaceLevel& s) {
    const std::vector<uint32_t>& off = element_offsets(s);
    const uint32_t* o = off.data();
    const size_t n = off.size(), bytes = s.bpp / 8;
    if (bytes == 4) {  // element offsets of 32-bit surfaces are 4-aligned
        uint32_t* dst = reinterpret_cast<uint32_t*>(linear);
        for (size_t i = 0; i < n; ++i) dst[i] = *reinterpret_cast<const uint32_t*>(tiled + o[i]);
    } else {
        for (size_t i = 0; i < n; ++i) std::memcpy(linear + i * bytes, tiled + o[i], bytes);
    }
}

void retile(const uint8_t* linear, uint8_t* tiled, const SurfaceLevel& s) {
    const std::vector<uint32_t>& off = element_offsets(s);
    const uint32_t* o = off.data();
    const size_t n = off.size(), bytes = s.bpp / 8;
    for (size_t i = 0; i < n; ++i) std::memcpy(tiled + o[i], linear + i * bytes, bytes);
}

}  // namespace mh3u::gfx
