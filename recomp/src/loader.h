// Loads the decompressed RPX (ELF32 BE) for recompilation: applies the RPL relocations (.rela.text, .rela.rodata,
// .rela.data) and records every reference to an imported function or data symbol, so the emitter can call the
// HLE implementation directly instead of following the (out-of-range) branch into the import stub area.
#pragma once

#include "elf.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace mh3u::recomp {

struct Import {
    std::string rpl;   // e.g. "coreinit", "gx2", "snd_core.rpl" (section name without .fimport_/.dimport_)
    std::string name;  // e.g. "OSCreateThread"
    bool data = false; // .dimport_* (data import) vs .fimport_* (function)
    uint32_t stub = 0; // symbol value in the import area (0xC000xxxx)
};

struct ImportRef {
    uint32_t site = 0;     // address of the relocated field
    uint8_t type = 0;      // R_PPC_* relocation type
    uint32_t import = 0;   // index into Image::imports
};

struct RelocStats {
    size_t applied = 0;           // relocations processed
    size_t changed_internal = 0;  // non-import relocations whose result differed from the file (expected 0)
    size_t changed_import = 0;    // import relocations whose result differed from the file
    std::map<int, size_t> by_type;
};

struct Image {
    ElfImage elf;                         // sections with relocations applied
    std::vector<Import> imports;
    std::vector<ImportRef> import_refs;   // sorted by site
    std::map<uint32_t, uint32_t> import_calls;  // REL24 site (the bl/b) -> import index
    std::vector<uint32_t> undefined_refs;  // sites relocated against $UNDEF (undefined weak symbols -> 0), sorted
    RelocStats stats;

    // Import referenced by the instruction at `insn_addr` (a call, or a lis/addi half of a data address).
    const Import* import_at(uint32_t insn_addr) const;
};

// R_PPC relocation types used by this executable.
enum : uint8_t { R_PPC_ADDR32 = 1, R_PPC_ADDR16_LO = 4, R_PPC_ADDR16_HI = 5, R_PPC_ADDR16_HA = 6, R_PPC_REL24 = 10 };

// From a file (ELF or RPX), from the file's bytes, or from an already parsed image.
std::optional<Image> load_image(const std::filesystem::path& elf_path, std::string* error = nullptr);
std::optional<Image> load_image(const std::vector<uint8_t>& file, std::string* error = nullptr);
std::optional<Image> link_image(ElfImage elf, std::string* error = nullptr);

}  // namespace mh3u::recomp
