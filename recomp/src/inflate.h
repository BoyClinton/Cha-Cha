// Minimal DEFLATE / zlib decoder (RFC 1950/1951) for the zlib-compressed sections of an RPX, so the executable is
// read straight from the user's dump without an external decompression tool.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mh3u::recomp {

// Decodes a zlib stream (2-byte header, deflate data, Adler-32 trailer) into out (appended). Returns false on
// malformed input or a checksum mismatch.
bool zlib_inflate(const uint8_t* src, size_t size, std::vector<uint8_t>& out);

}  // namespace mh3u::recomp
