#include "gfd.h"

#include <cstring>

namespace mh3u::latte {
namespace {

uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }

// GFD block types (wut gfd.h)
enum : uint32_t {
    kEof = 1,
    kVsHeader = 3,
    kVsProgram = 5,
    kPsHeader = 6,
    kPsProgram = 7,
    kGsHeader = 8,
    kGsProgram = 9,
    kGsCopyProgram = 10,
};

struct Block {
    uint32_t type;
    const uint8_t* data;
    uint32_t size;
};

class HeaderReader {
public:
    HeaderReader(const Block& b) : b_(b) {}
    bool ok() const { return ok_; }
    uint32_t u32(uint32_t off) {
        if (off + 4 > b_.size) return ok_ = false, 0;
        return be32(b_.data + off);
    }
    // A relocated pointer: low 20 bits are an offset into this block.
    uint32_t ptr(uint32_t off) { return u32(off) & 0xFFFFF; }
    std::string str(uint32_t off) {
        const uint32_t p = ptr(off);
        if (p >= b_.size) return ok_ = false, std::string();
        const char* s = reinterpret_cast<const char*>(b_.data + p);
        return std::string(s, strnlen(s, b_.size - p));
    }
    std::vector<uint32_t> words(uint32_t off, uint32_t bytes) {
        std::vector<uint32_t> w(bytes / 4);
        for (uint32_t i = 0; i < w.size(); ++i) w[i] = u32(off + i * 4);
        return w;
    }

private:
    const Block& b_;
    bool ok_ = true;
};

// Reads the variable tables that follow `mode` in every header (uniform blocks, vars, initial values, loop vars,
// samplers; attribs for the VS).
void read_tables(HeaderReader& h, uint32_t mode_off, bool attribs, Shader& s) {
    s.mode = h.u32(mode_off);
    uint32_t o = mode_off + 4;
    const auto table = [&](uint32_t& count, uint32_t& at) {
        count = h.u32(o);
        at = count ? h.ptr(o + 4) : 0;
        o += 8;
    };
    uint32_t n, at;
    table(n, at);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = at + i * 12;
        s.uniform_blocks.push_back({h.str(e), h.u32(e + 4), h.u32(e + 8)});
    }
    table(n, at);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = at + i * 20;
        s.uniform_vars.push_back({h.str(e), h.u32(e + 4), h.u32(e + 8), h.u32(e + 12), static_cast<int32_t>(h.u32(e + 16))});
    }
    table(n, at);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = at + i * 20;
        InitialValue v;
        for (int c = 0; c < 4; ++c) {
            const uint32_t bits = h.u32(e + c * 4);
            std::memcpy(&v.value[c], &bits, 4);
        }
        v.offset = h.u32(e + 16);
        s.initial_values.push_back(v);
    }
    table(n, at);
    for (uint32_t i = 0; i < n; ++i) s.loop_vars.push_back({h.u32(at + i * 8), h.u32(at + i * 8 + 4)});
    table(n, at);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = at + i * 12;
        s.samplers.push_back({h.str(e), h.u32(e + 4), h.u32(e + 8)});
    }
    if (attribs) {
        table(n, at);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t e = at + i * 16;
            s.attribs.push_back({h.str(e), h.u32(e + 4), h.u32(e + 8), h.u32(e + 12)});
        }
        s.ring_item_size = h.u32(o);
    }
}

}  // namespace

uint64_t shader_key(const uint8_t* regs_be, size_t regs_size, const uint8_t* program, size_t program_size) {
    uint64_t h = 0xCBF29CE484222325ull;
    const auto mix = [&](const uint8_t* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    };
    mix(regs_be, regs_size);
    mix(program, program_size);
    return h;
}

uint64_t Shader::key() const {
    std::vector<uint8_t> be(regs.size() * 4);
    for (size_t i = 0; i < regs.size(); ++i) {
        be[i * 4 + 0] = static_cast<uint8_t>(regs[i] >> 24);
        be[i * 4 + 1] = static_cast<uint8_t>(regs[i] >> 16);
        be[i * 4 + 2] = static_cast<uint8_t>(regs[i] >> 8);
        be[i * 4 + 3] = static_cast<uint8_t>(regs[i]);
    }
    return shader_key(be.data(), be.size(), program.data(), program.size());
}

bool parse_gfd(const uint8_t* data, size_t size, std::vector<Shader>& out, std::string* error) {
    const auto fail = [&](const char* msg) {
        if (error) *error = msg;
        return false;
    };
    if (size < 0x20 || std::memcmp(data, "Gfx2", 4) != 0) return fail("not a GFD");
    std::vector<Block> blocks;
    size_t o = be32(data + 4);
    while (o + 0x20 <= size && std::memcmp(data + o, "BLK{", 4) == 0) {
        const uint32_t hs = be32(data + o + 4), type = be32(data + o + 16), bsize = be32(data + o + 20);
        if (o + hs + bsize > size) return fail("block overruns the file");
        blocks.push_back({type, data + o + hs, bsize});
        o += hs + bsize;
        if (type == kEof) break;
    }
    // Headers and programs come in order; pair each header with the program block(s) of its stage.
    const auto program_of = [&](uint32_t type, size_t nth) -> const Block* {
        size_t k = 0;
        for (const Block& b : blocks)
            if (b.type == type && k++ == nth) return &b;
        return nullptr;
    };
    size_t nvs = 0, nps = 0, ngs = 0;
    for (const Block& b : blocks) {
        Shader s;
        HeaderReader h(b);
        const Block* prog = nullptr;
        if (b.type == kVsHeader) {
            s.stage = Stage::Vertex;
            s.regs = h.words(0, kVsRegsBytes);
            read_tables(h, 0xD8, true, s);
            prog = program_of(kVsProgram, nvs++);
        } else if (b.type == kPsHeader) {
            s.stage = Stage::Pixel;
            s.regs = h.words(0, kPsRegsBytes);
            read_tables(h, 0xAC, false, s);
            prog = program_of(kPsProgram, nps++);
        } else if (b.type == kGsHeader) {
            s.stage = Stage::Geometry;
            s.regs = h.words(0, kGsRegsBytes);
            read_tables(h, 0x5C, false, s);
            s.ring_item_size = h.u32(0x88);
            prog = program_of(kGsProgram, ngs);
            if (const Block* c = program_of(kGsCopyProgram, ngs)) s.copy_program.assign(c->data, c->data + c->size);
            ++ngs;
        } else {
            continue;
        }
        if (!h.ok()) return fail("header field out of range");
        if (!prog) return fail("shader header without a program block");
        s.program.assign(prog->data, prog->data + prog->size);
        out.push_back(std::move(s));
    }
    return true;
}

}  // namespace mh3u::latte
