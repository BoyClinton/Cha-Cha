#include "loader.h"

#include <algorithm>

namespace mh3u::recomp {
namespace {

constexpr uint32_t SHT_SYMTAB = 2, SHT_RELA = 4;

uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t{d[o]} << 24) | (uint32_t{d[o + 1]} << 16) | (uint32_t{d[o + 2]} << 8) | d[o + 3];
}
uint16_t be16(const std::vector<uint8_t>& d, size_t o) {
    return static_cast<uint16_t>((d[o] << 8) | d[o + 1]);
}
void put32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = static_cast<uint8_t>(v >> 24);
    d[o + 1] = static_cast<uint8_t>(v >> 16);
    d[o + 2] = static_cast<uint8_t>(v >> 8);
    d[o + 3] = static_cast<uint8_t>(v);
}
void put16(std::vector<uint8_t>& d, size_t o, uint16_t v) {
    d[o] = static_cast<uint8_t>(v >> 8);
    d[o + 1] = static_cast<uint8_t>(v);
}

std::string cstr(const std::vector<uint8_t>& d, size_t o) {
    std::string s;
    while (o < d.size() && d[o]) s += static_cast<char>(d[o++]);
    return s;
}

}  // namespace

const Import* Image::import_at(uint32_t insn_addr) const {
    // relocated fields are the whole word (REL24, ADDR32) or its low halfword (ADDR16_*: site = insn + 2)
    auto it = std::lower_bound(import_refs.begin(), import_refs.end(), insn_addr,
                               [](const ImportRef& r, uint32_t a) { return r.site < a; });
    if (it != import_refs.end() && it->site >= insn_addr && it->site < insn_addr + 4) return &imports[it->import];
    return nullptr;
}

std::optional<Image> load_image(const std::filesystem::path& elf_path, std::string* error) {
    auto elf = ElfImage::load(elf_path, error);
    if (!elf) return std::nullopt;
    return link_image(std::move(*elf), error);
}

std::optional<Image> load_image(const std::vector<uint8_t>& file, std::string* error) {
    auto elf = ElfImage::parse(file, error);
    if (!elf) return std::nullopt;
    return link_image(std::move(*elf), error);
}

std::optional<Image> link_image(ElfImage elf, std::string* error) {
    auto fail = [&](const std::string& why) -> std::optional<Image> {
        if (error) *error = why;
        return std::nullopt;
    };
    Image img{std::move(elf), {}, {}, {}, {}};
    auto& secs = img.elf.sections;

    // Symbols: import symbols live in .fimport_* / .dimport_* sections.
    struct Sym {
        uint32_t value;
        uint16_t shndx;
        int import = -1;
        bool undefined = false;  // $UNDEF: undefined weak references (crt hooks); the Cafe loader resolves them to 0
    };
    std::vector<Sym> syms;
    size_t symtab_index = secs.size();
    for (size_t i = 0; i < secs.size(); ++i)
        if (secs[i].type == SHT_SYMTAB) symtab_index = i;
    if (symtab_index == secs.size()) return fail("no .symtab");
    const Section& symtab = secs[symtab_index];
    // .symtab's sh_link (string table) is not kept by ElfImage; RPX puts .strtab right after .symtab.
    const Section* strtab = img.elf.section(".strtab");
    if (!strtab) return fail("no .strtab");
    for (size_t o = 0; o + 16 <= symtab.data.size(); o += 16) {
        Sym s{be32(symtab.data, o + 4), be16(symtab.data, o + 14)};
        const uint8_t sym_type = symtab.data[o + 12] & 0xF;  // STT_SECTION (3) symbols name the section itself
        s.undefined = s.shndx == 0 && cstr(strtab->data, be32(symtab.data, o)) == "$UNDEF";
        if (s.shndx < secs.size() && sym_type != 3) {
            const std::string& sec = secs[s.shndx].name;
            bool f = sec.rfind(".fimport_", 0) == 0, dt = sec.rfind(".dimport_", 0) == 0;
            if (f || dt) {
                s.import = static_cast<int>(img.imports.size());
                img.imports.push_back({sec.substr(9), cstr(strtab->data, be32(symtab.data, o)), dt, s.value});
            }
        }
        syms.push_back(s);
    }

    // Relocations. Each .rela.X applies to the section named X; r_offset is an absolute address.
    for (const Section& rs : secs) {
        if (rs.type != SHT_RELA) continue;
        Section* target = nullptr;
        for (auto& s : secs)
            if (s.name == rs.name.substr(5)) target = &s;  // ".rela.text" -> ".text"
        if (!target) return fail("no target section for " + rs.name);
        for (size_t o = 0; o + 12 <= rs.data.size(); o += 12) {
            const uint32_t P = be32(rs.data, o), info = be32(rs.data, o + 4);
            const int32_t A = static_cast<int32_t>(be32(rs.data, o + 8));
            const uint8_t type = info & 0xFF;
            const uint32_t sym = info >> 8;
            if (sym >= syms.size()) return fail("relocation symbol out of range");
            if (P < target->addr || P - target->addr + 4 > target->data.size() + (type == R_PPC_ADDR32 ? 0 : 2))
                return fail("relocation outside " + target->name);
            const uint32_t S = syms[sym].value, V = S + static_cast<uint32_t>(A), off = P - target->addr;
            auto& d = target->data;
            bool changed = false;
            switch (type) {
                case R_PPC_ADDR32:
                    changed = be32(d, off) != V;
                    put32(d, off, V);
                    break;
                case R_PPC_ADDR16_LO:
                    changed = be16(d, off) != (V & 0xFFFF);
                    put16(d, off, static_cast<uint16_t>(V));
                    break;
                case R_PPC_ADDR16_HI:
                    changed = be16(d, off) != (V >> 16);
                    put16(d, off, static_cast<uint16_t>(V >> 16));
                    break;
                case R_PPC_ADDR16_HA: {
                    uint16_t ha = static_cast<uint16_t>((V + 0x8000) >> 16);
                    changed = be16(d, off) != ha;
                    put16(d, off, ha);
                    break;
                }
                case R_PPC_REL24: {
                    // Import stubs (0xC000xxxx) are out of branch range; like Ghidra we keep the low 26 bits of the
                    // displacement. The real target is recorded in import_calls.
                    uint32_t insn = be32(d, off);
                    uint32_t patched = (insn & ~0x03FFFFFCu) | ((V - P) & 0x03FFFFFCu);
                    changed = insn != patched;
                    put32(d, off, patched);
                    break;
                }
                default:
                    return fail("unsupported relocation type " + std::to_string(type) + " in " + rs.name);
            }
            ++img.stats.applied;
            ++img.stats.by_type[type];
            if (syms[sym].import >= 0) {
                img.stats.changed_import += changed;
                img.import_refs.push_back({P, type, static_cast<uint32_t>(syms[sym].import)});
                if (type == R_PPC_REL24) img.import_calls[P] = static_cast<uint32_t>(syms[sym].import);
            } else if (syms[sym].undefined) {
                img.undefined_refs.push_back(P);
            } else {
                img.stats.changed_internal += changed;
            }
        }
    }
    std::sort(img.undefined_refs.begin(), img.undefined_refs.end());
    std::sort(img.import_refs.begin(), img.import_refs.end(),
              [](const ImportRef& a, const ImportRef& b) { return a.site < b.site; });
    return img;
}

}  // namespace mh3u::recomp
