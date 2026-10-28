// Minimal big-endian ELF32 reader for the executable: a plain ELF or an RPX straight from the dump (its
// zlib-compressed sections are inflated on load).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mh3u::recomp {

struct Section {
    std::string name;
    uint32_t type = 0, flags = 0, addr = 0, size = 0;
    std::vector<uint8_t> data;  // empty for NOBITS
};

class ElfImage {
public:
    static std::optional<ElfImage> load(const std::filesystem::path& path, std::string* error = nullptr);
    static std::optional<ElfImage> parse(const std::vector<uint8_t>& file, std::string* error = nullptr);

    const Section* section(const std::string& name) const;
    // Big-endian u32 at a guest address inside any loaded section.
    std::optional<uint32_t> read32(uint32_t addr) const;

    std::vector<Section> sections;
};

}  // namespace mh3u::recomp
