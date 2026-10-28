#include "inflate.h"

namespace mh3u::recomp {
namespace {

// LSB-first bit reader over the deflate stream.
struct Bits {
    const uint8_t* p;
    size_t n, pos = 0;
    uint32_t buf = 0;
    int cnt = 0;
    bool overrun = false;

    uint32_t get(int need) {
        while (cnt < need) {
            uint32_t byte = 0;
            if (pos < n) byte = p[pos++];
            else overrun = true;
            buf |= byte << cnt;
            cnt += 8;
        }
        const uint32_t v = buf & ((1u << need) - 1);
        buf >>= need;
        cnt -= need;
        return v;
    }
    void align() {
        buf = 0;
        cnt = 0;
    }
};

// Canonical Huffman table: counts per code length and symbols ordered by code.
struct Huffman {
    uint16_t count[16] = {};
    uint16_t symbol[320] = {};

    bool build(const uint8_t* lengths, int n) {
        for (int i = 0; i < 16; ++i) count[i] = 0;
        for (int i = 0; i < n; ++i) ++count[lengths[i]];
        count[0] = 0;
        int left = 1;  // over-subscribed sets are invalid; incomplete ones are allowed (single-code distance trees)
        for (int len = 1; len < 16; ++len) {
            left = (left << 1) - count[len];
            if (left < 0) return false;
        }
        uint16_t offs[16] = {};
        for (int len = 1; len < 15; ++len) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
        for (int i = 0; i < n; ++i)
            if (lengths[i]) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        return true;
    }

    int decode(Bits& b) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= static_cast<int>(b.get(1));
            const int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first = (first + c) << 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& b, std::vector<uint8_t>& out, size_t start, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = lit.decode(b);
        if (sym < 0 || b.overrun) return false;
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29) return false;
            const size_t len = kLenBase[sym] + b.get(kLenExtra[sym]);
            const int ds = dist.decode(b);
            if (ds < 0 || ds >= 30) return false;
            const size_t d = kDistBase[ds] + b.get(kDistExtra[ds]);
            if (d > out.size() - start) return false;
            const size_t from = out.size() - d;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);  // may overlap: byte by byte
        }
    }
}

bool deflate(Bits& b, std::vector<uint8_t>& out) {
    const size_t start = out.size();
    for (bool last = false; !last;) {
        last = b.get(1) != 0;
        const uint32_t type = b.get(2);
        if (type == 0) {  // stored
            b.align();
            if (b.pos + 4 > b.n) return false;
            const uint32_t len = b.p[b.pos] | (b.p[b.pos + 1] << 8), nlen = b.p[b.pos + 2] | (b.p[b.pos + 3] << 8);
            b.pos += 4;
            if ((len ^ 0xFFFF) != nlen || b.pos + len > b.n) return false;
            out.insert(out.end(), b.p + b.pos, b.p + b.pos + len);
            b.pos += len;
        } else if (type == 1) {  // fixed codes
            static const auto fixed = [] {
                std::pair<Huffman, Huffman> t;
                uint8_t l[288];
                for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
                t.first.build(l, 288);
                for (int i = 0; i < 30; ++i) l[i] = 5;
                t.second.build(l, 30);
                return t;
            }();
            if (!codes(b, out, start, fixed.first, fixed.second)) return false;
        } else if (type == 2) {  // dynamic codes
            const int nlen = static_cast<int>(b.get(5)) + 257, ndist = static_cast<int>(b.get(5)) + 1,
                      ncode = static_cast<int>(b.get(4)) + 4;
            if (nlen > 286 || ndist > 30) return false;
            static constexpr uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; ++i) lengths[kOrder[i]] = static_cast<uint8_t>(b.get(3));
            Huffman lencode;
            if (!lencode.build(lengths, 19)) return false;
            for (int i = 0; i < nlen + ndist;) {
                const int sym = lencode.decode(b);
                if (sym < 0 || b.overrun) return false;
                if (sym < 16) {
                    lengths[i++] = static_cast<uint8_t>(sym);
                    continue;
                }
                uint8_t v = 0;
                int rep = 0;
                if (sym == 16) {
                    if (i == 0) return false;
                    v = lengths[i - 1];
                    rep = 3 + static_cast<int>(b.get(2));
                } else if (sym == 17) {
                    rep = 3 + static_cast<int>(b.get(3));
                } else {
                    rep = 11 + static_cast<int>(b.get(7));
                }
                if (i + rep > nlen + ndist) return false;
                while (rep--) lengths[i++] = v;
            }
            if (lengths[256] == 0) return false;
            Huffman lit, dist;
            if (!lit.build(lengths, nlen) || !dist.build(lengths + nlen, ndist)) return false;
            if (!codes(b, out, start, lit, dist)) return false;
        } else {
            return false;
        }
        if (b.overrun) return false;
    }
    return true;
}

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, s = 0;
    while (n) {
        const size_t chunk = n < 5552 ? n : 5552;  // largest run without overflowing 32 bits
        for (size_t i = 0; i < chunk; ++i) {
            a += p[i];
            s += a;
        }
        a %= 65521;
        s %= 65521;
        p += chunk;
        n -= chunk;
    }
    return (s << 16) | a;
}

}  // namespace

bool zlib_inflate(const uint8_t* src, size_t size, std::vector<uint8_t>& out) {
    if (size < 6 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31 != 0 || (src[1] & 0x20)) return false;
    const size_t start = out.size();
    Bits b{src + 2, size - 2};
    if (!deflate(b, out)) return false;
    b.align();  // the Adler-32 trailer starts at the next byte
    if (b.pos + 4 > b.n) return false;
    const uint8_t* t = b.p + b.pos;
    const uint32_t want = (uint32_t{t[0]} << 24) | (uint32_t{t[1]} << 16) | (uint32_t{t[2]} << 8) | t[3];
    return adler32(out.data() + start, out.size() - start) == want;
}

}  // namespace mh3u::recomp
