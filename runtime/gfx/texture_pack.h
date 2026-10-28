#pragma once
// Texture replacement packs: DDS files in <data>/texture_replacements (any subfolder), each named after the game
// texture it replaces - tex_<width>x<height>_<CRC32>[anything].dds, the CRC32 taken over the texture's level 0 as it
// sits in guest memory (GX2-tiled). MH3U_DUMP_TEXTURES writes the game's textures under exactly these names, so a
// pack is made by editing dumped files. See docs/texture-packs.md.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace mh3u::gfx::texpack {

uint32_t crc32(const uint8_t* data, size_t n);
std::string file_name(uint32_t width, uint32_t height, uint32_t crc);  // tex_256x256_1A2B3C4D.dds

// A DDS image: DXGI format, levels back to back (block-compressed or 32-bit texels).
struct Dds {
    uint32_t dxgi = 0, width = 0, height = 0, levels = 0;
    std::vector<uint8_t> data;
    std::vector<size_t> offsets;  // per level into data
};
// Bytes of one level, or 0 for a format we don't handle. Block formats count 4x4 blocks.
size_t level_bytes(uint32_t dxgi, uint32_t width, uint32_t height);
bool read_dds(const std::filesystem::path& file, Dds& out, std::string* error);
bool write_dds(const std::filesystem::path& file, uint32_t dxgi, uint32_t width, uint32_t height, const uint8_t* data, size_t n);

class Library {
public:
    // Indexes every tex_*.dds below dir (later files with the same key win: subfolders sort like file names).
    size_t scan(const std::filesystem::path& dir);
    const std::filesystem::path* find(uint32_t width, uint32_t height, uint32_t crc) const;
    size_t size() const { return files_.size(); }

private:
    std::unordered_map<uint64_t, std::filesystem::path> files_;
};

}  // namespace mh3u::gfx::texpack
