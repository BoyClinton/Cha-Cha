// Decoders for the game's GUI sheets (game_skin.cpp): zlib, MT .arc entries, MT .tex level -> RGBA8.
#include "skin_decode.h"

#include "tiling.h"

#include <algorithm>
#include <cstring>

namespace mh3u::boot {

// ---- zlib (RFC 1950 / 1951): canonical Huffman decoding, one bit at a time; small and only used at start -----------
namespace skin_detail {
namespace {

struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool err = false;
    uint32_t get(int need) {
        uint32_t v = buf;
        while (cnt < need) {
            if (pos >= n) {
                err = true;
                return 0;
            }
            v |= static_cast<uint32_t>(p[pos++]) << cnt;
            cnt += 8;
        }
        buf = need < 32 ? v >> need : 0;
        cnt -= need;
        return v & ((1u << need) - 1);
    }
};

struct Huffman {
    uint16_t count[16] = {};  // codes of each length
    uint16_t symbol[320] = {};  // symbols ordered by code
};

// < 0: over-subscribed (invalid); 0: complete; > 0: incomplete (allowed for single-code trees)
int build(Huffman& h, const uint8_t* lengths, int n) {
    std::memset(h.count, 0, sizeof h.count);
    for (int i = 0; i < n; ++i) ++h.count[lengths[i]];
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    uint16_t offs[16] = {};
    for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    return left;
}

int decode(Bits& b, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(b.get(1));
        if (b.err) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& b, std::vector<uint8_t>& out, const Huffman& lens, const Huffman& dists) {
    for (;;) {
        int sym = decode(b, lens);
        if (sym < 0) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) return false;
        const size_t len = kLenBase[sym] + b.get(kLenExtra[sym]);
        const int ds = decode(b, dists);
        if (ds < 0 || ds >= 30) return false;
        const size_t dist = kDistBase[ds] + b.get(kDistExtra[ds]);
        if (b.err || dist > out.size()) return false;
        const size_t from = out.size() - dist;
        for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);  // may overlap what it writes
    }
}

}  // namespace

bool inflate_zlib(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected_size) {
    if (n < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return false;
    out.clear();
    out.reserve(expected_size);
    Bits b{src + 2, n - 2};
    Huffman lens, dists;
    for (bool last = false; !last;) {
        last = b.get(1) != 0;
        const uint32_t type = b.get(2);
        if (b.err) return false;
        if (type == 0) {  // stored: from the next byte boundary
            b.buf = 0;
            b.cnt = 0;
            if (b.pos + 4 > b.n) return false;
            const uint32_t len = b.p[b.pos] | (b.p[b.pos + 1] << 8), nlen = b.p[b.pos + 2] | (b.p[b.pos + 3] << 8);
            b.pos += 4;
            if (len != (~nlen & 0xFFFF) || b.pos + len > b.n) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
        } else if (type == 1) {  // fixed codes
            uint8_t l[288 + 30];
            std::fill(l, l + 144, uint8_t{8});
            std::fill(l + 144, l + 256, uint8_t{9});
            std::fill(l + 256, l + 280, uint8_t{7});
            std::fill(l + 280, l + 288, uint8_t{8});
            std::fill(l + 288, l + 318, uint8_t{5});
            build(lens, l, 288);
            build(dists, l + 288, 30);
            if (!codes(b, out, lens, dists)) return false;
        } else if (type == 2) {  // dynamic codes
            const int nlen = static_cast<int>(b.get(5)) + 257, ndist = static_cast<int>(b.get(5)) + 1, ncode = static_cast<int>(b.get(4)) + 4;
            if (b.err || nlen > 286 || ndist > 30) return false;
            static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t l[320] = {};
            for (int i = 0; i < ncode; ++i) l[kOrder[i]] = static_cast<uint8_t>(b.get(3));
            Huffman cl;
            if (build(cl, l, 19) != 0) return false;
            for (int i = 0; i < nlen + ndist;) {
                const int sym = decode(b, cl);
                if (sym < 0) return false;
                if (sym < 16) {
                    l[i++] = static_cast<uint8_t>(sym);
                    continue;
                }
                uint8_t v = 0;
                int rep;
                if (sym == 16) {
                    if (i == 0) return false;
                    v = l[i - 1];
                    rep = 3 + static_cast<int>(b.get(2));
                } else if (sym == 17) {
                    rep = 3 + static_cast<int>(b.get(3));
                } else {
                    rep = 11 + static_cast<int>(b.get(7));
                }
                if (b.err || i + rep > nlen + ndist) return false;
                while (rep--) l[i++] = v;
            }
            if (l[256] == 0) return false;  // no end-of-block code
            const int el = build(lens, l, nlen), ed = build(dists, l + nlen, ndist);
            if (el < 0 || (el > 0 && nlen - lens.count[0] != 1) || ed < 0 || (ed > 0 && ndist - dists.count[0] != 1)) return false;
            if (!codes(b, out, lens, dists)) return false;
        } else {
            return false;
        }
    }
    return true;
}

namespace {
uint32_t be32(const uint8_t* p) { return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3]; }
}  // namespace

bool arc_entry(const std::vector<uint8_t>& arc, const std::string& path, uint32_t type_hash, std::vector<uint8_t>& out) {
    if (arc.size() < 12 || be32(arc.data()) != 0x00435241u) return false;
    const size_t count = (size_t{arc[6]} << 8) | arc[7];
    if (12 + count * 0x50 > arc.size()) return false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* e = arc.data() + 12 + i * 0x50;
        const size_t name_len = strnlen(reinterpret_cast<const char*>(e), 64);
        if (name_len != path.size() || std::memcmp(e, path.data(), name_len) != 0 || be32(e + 0x40) != type_hash) continue;
        const uint32_t comp = be32(e + 0x44), size = be32(e + 0x48) >> 3, off = be32(e + 0x4C);
        if (size_t{off} + comp > arc.size()) return false;
        if (!inflate_zlib(arc.data() + off, comp, out, size)) {
            if (comp != size) return false;  // never seen: an entry stored as it is
            out.assign(arc.begin() + off, arc.begin() + off + comp);
        }
        return out.size() == size;
    }
    return false;
}

namespace {

void rgb565(uint16_t c, uint8_t* o) {
    const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    o[0] = static_cast<uint8_t>(r << 3 | r >> 2);
    o[1] = static_cast<uint8_t>(g << 2 | g >> 4);
    o[2] = static_cast<uint8_t>(b << 3 | b >> 2);
    o[3] = 255;
}

// one BC1 colour block -> 16 RGBA texels (punchthrough: the 3-colour + transparent mode of plain BC1)
void bc1_block(const uint8_t* blk, bool punchthrough, uint8_t (*texels)[4]) {
    const uint16_t c0 = static_cast<uint16_t>(blk[0] | blk[1] << 8), c1 = static_cast<uint16_t>(blk[2] | blk[3] << 8);
    const uint32_t idx = blk[4] | blk[5] << 8 | blk[6] << 16 | static_cast<uint32_t>(blk[7]) << 24;
    uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !punchthrough) {
        for (int k = 0; k < 3; ++k) {
            pal[2][k] = static_cast<uint8_t>((2 * pal[0][k] + pal[1][k]) / 3);
            pal[3][k] = static_cast<uint8_t>((pal[0][k] + 2 * pal[1][k]) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) pal[2][k] = static_cast<uint8_t>((pal[0][k] + pal[1][k]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    for (int t = 0; t < 16; ++t) std::memcpy(texels[t], pal[(idx >> (2 * t)) & 3], 4);
}

void bc3_alpha(const uint8_t* blk, uint8_t (*texels)[4]) {
    const uint32_t a0 = blk[0], a1 = blk[1];
    uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(blk[2 + i]) << (8 * i);
    uint8_t pal[8] = {static_cast<uint8_t>(a0), static_cast<uint8_t>(a1)};
    if (a0 > a1) {
        for (uint32_t i = 1; i < 7; ++i) pal[i + 1] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    } else {
        for (uint32_t i = 1; i < 5; ++i) pal[i + 1] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
        pal[6] = 0;
        pal[7] = 255;
    }
    for (int t = 0; t < 16; ++t) texels[t][3] = pal[(bits >> (3 * t)) & 7];
}

}  // namespace

bool tex_rgba(const std::vector<uint8_t>& tex, int level, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h) {
    if (tex.size() < 16 || be32(tex.data()) != 0x00584554u) return false;
    const uint32_t b = be32(&tex[8]), c = be32(&tex[12]);
    const uint32_t mips = b >> 26, tw = (b >> 13) & 0x1FFF, th = b & 0x1FFF, faces = c >> 24, fmt = (c >> 16) & 0xFF;
    uint32_t bd, bpe;  // block size in texels, bytes per element
    if (fmt == 0x0B) bd = 4, bpe = 8;          // BC1
    else if (fmt == 0x0C) bd = 4, bpe = 16;    // BC3
    else if (fmt == 0x03) bd = 1, bpe = 4;     // RGBA8 (bytes R, G, B, A)
    else return false;
    if (faces != 1 || level < 0 || static_cast<uint32_t>(level) >= mips || (tw < 32 && th < 16)) return false;
    const auto align = [](uint32_t x, uint32_t a) { return (x + a - 1) / a * a; };
    // level offsets: 2D-tiled levels are 32x16 elements aligned (docs/formats/tex.md); only those are read here
    size_t off = 16;
    uint32_t ew = 0, eh = 0;
    for (int l = 0; l <= level; ++l) {
        const uint32_t lw = std::max(tw >> l, 1u), lh = std::max(th >> l, 1u);
        ew = (lw + bd - 1) / bd;
        eh = (lh + bd - 1) / bd;
        if (l > 0 && (ew < 32 || eh < 16)) return false;  // 1D-tiled from here on
        if (l < level) off += size_t{align(ew, 32)} * align(eh, 16) * bpe;
        w = lw;
        h = lh;
    }
    const uint32_t pitch = align(ew, 32), height = align(eh, 16);
    const size_t level_size = size_t{pitch} * height * bpe;
    if (off + level_size > tex.size()) return false;
    const uint8_t* src = tex.data() + off;
    rgba.assign(size_t{w} * h * 4, 0);
    uint8_t texels[16][4];
    for (uint32_t y = 0; y < eh; ++y) {
        for (uint32_t x = 0; x < ew; ++x) {
            const uint32_t a = gfx::addr_2d_thin1(x, y, bpe * 8, pitch, height, 0, 0, 0);
            if (a + bpe > level_size) return false;
            const uint8_t* e = src + a;
            if (bd == 1) {
                std::memcpy(&rgba[(size_t{y} * w + x) * 4], e, 4);
                continue;
            }
            if (bpe == 8) {
                bc1_block(e, true, texels);
            } else {
                bc1_block(e + 8, false, texels);
                bc3_alpha(e, texels);
            }
            for (uint32_t t = 0; t < 16; ++t) {
                const uint32_t px = x * 4 + (t & 3), py = y * 4 + (t >> 2);
                if (px < w && py < h) std::memcpy(&rgba[(size_t{py} * w + px) * 4], texels[t], 4);
            }
        }
    }
    return true;
}

}  // namespace skin_detail

}  // namespace mh3u::boot
