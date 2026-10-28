#include "elf.h"
#include "inflate.h"

#include <fstream>
#include <iterator>

namespace mh3u::recomp {
namespace {

constexpr uint32_t kShfRplZlib = 0x08000000;

uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t{d[o]} << 24) | (uint32_t{d[o + 1]} << 16) | (uint32_t{d[o + 2]} << 8) | d[o + 3];
}
uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<uint16_t>((d[o] << 8) | d[o + 1]);
}

}  // namespace

std::optional<ElfImage> ElfImage::load(const std::filesystem::path& path, std::string* error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot open file";
        return std::nullopt;
    }
    return parse(std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), error);
}

std::optional<ElfImage> ElfImage::parse(const std::vector<uint8_t>& d, std::string* error) {
    auto fail = [&](const char* why) -> std::optional<ElfImage> {
        if (error) *error = why;
        return std::nullopt;
    };
    if (d.size() < 0x34 || d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') return fail("not an ELF");
    if (d[4] != 1 || d[5] != 2) return fail("not ELF32 big-endian");
    const uint32_t shoff = be32(d, 0x20);
    const uint16_t shentsize = be16(d, 0x2E), shnum = be16(d, 0x30), shstrndx = be16(d, 0x32);
    if (shentsize < 0x28 || shoff + size_t{shnum} * shentsize > d.size() || shstrndx >= shnum)
        return fail("bad section header table");

    ElfImage img;
    std::vector<uint32_t> name_offs;
    for (uint16_t i = 0; i < shnum; ++i) {
        const size_t h = shoff + size_t{i} * shentsize;
        Section s;
        name_offs.push_back(be32(d, h));
        s.type = be32(d, h + 4);
        s.flags = be32(d, h + 8);
        s.addr = be32(d, h + 12);
        const uint32_t off = be32(d, h + 16);
        s.size = be32(d, h + 20);
        if (s.type != 8 /* NOBITS */ && s.type != 0) {
            if (size_t{off} + s.size > d.size()) return fail("section data out of range");
            s.data.assign(d.begin() + off, d.begin() + off + s.size);
            // RPX: SHF_RPL_ZLIB sections hold the inflated size (u32) followed by a zlib stream
            if (s.flags & kShfRplZlib) {
                if (s.data.size() < 4) return fail("compressed section too small");
                const uint32_t inflated = be32(s.data, 0);
                std::vector<uint8_t> out;
                out.reserve(inflated);
                if (!zlib_inflate(s.data.data() + 4, s.data.size() - 4, out) || out.size() != inflated)
                    return fail("cannot decompress an RPX section");
                s.data = std::move(out);
                s.size = inflated;
                s.flags &= ~kShfRplZlib;
            }
        }
        img.sections.push_back(std::move(s));
    }
    const Section& strtab = img.sections[shstrndx];
    for (size_t i = 0; i < img.sections.size(); ++i) {
        size_t o = name_offs[i];
        while (o < strtab.data.size() && strtab.data[o]) img.sections[i].name += static_cast<char>(strtab.data[o++]);
    }
    return img;
}

const Section* ElfImage::section(const std::string& name) const {
    for (const auto& s : sections)
        if (s.name == name) return &s;
    return nullptr;
}

std::optional<uint32_t> ElfImage::read32(uint32_t addr) const {
    for (const auto& s : sections) {
        if (s.addr == 0 || s.data.empty() || addr < s.addr || addr - s.addr + 4 > s.data.size()) continue;
        return be32(s.data, addr - s.addr);
    }
    return std::nullopt;
}

}  // namespace mh3u::recomp
