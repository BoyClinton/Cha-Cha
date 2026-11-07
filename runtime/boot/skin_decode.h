#pragma once
// Decoding helpers for the game's GUI sheets (game_skin.h); no graphics dependencies, unit-tested in skin_decode_test.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mh3u::boot::skin_detail {

// zlib stream (RFC 1950 / 1951) -> bytes; expected_size reserves the output. False on a corrupt stream.
bool inflate_zlib(const uint8_t* src, size_t n, std::vector<uint8_t>& out, size_t expected_size = 0);
// An MT .arc (big-endian, version 16): the decompressed bytes of the entry with this path and type hash.
bool arc_entry(const std::vector<uint8_t>& arc, const std::string& path, uint32_t type_hash, std::vector<uint8_t>& out);
// An MT .tex (version 0xA5): one 2D-tiled mip level as RGBA8 rows (BC1, BC3 or RGBA8 textures).
bool tex_rgba(const std::vector<uint8_t>& tex, int level, std::vector<uint8_t>& rgba, uint32_t& w, uint32_t& h);

}  // namespace mh3u::boot::skin_detail
