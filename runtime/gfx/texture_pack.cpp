#include "texture_pack.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace mh3u::gfx::texpack {
namespace {

namespace stdfs = std::filesystem;

uint64_t key(uint32_t w, uint32_t h, uint32_t crc) { return (uint64_t{w & 0xFFFF} << 48) | (uint64_t{h & 0xFFFF} << 32) | crc; }

uint32_t rd32(const uint8_t* p) { return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24; }
void wr32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

// "tex_<w>x<h>_<8 hex>" at the start of a file name
bool parse_name(const std::string& n, uint32_t& w, uint32_t& h, uint32_t& crc) {
    if (n.size() < 4 || (n.compare(0, 4, "tex_") != 0 && n.compare(0, 4, "TEX_") != 0)) return false;
    size_t i = 4;
    const auto num = [&](uint32_t& v) {
        const size_t s = i;
        v = 0;
        while (i < n.size() && std::isdigit(static_cast<unsigned char>(n[i])) && i - s < 5) v = v * 10 + uint32_t(n[i++] - '0');
        return i > s;
    };
    if (!num(w) || i >= n.size() || (n[i] != 'x' && n[i] != 'X')) return false;
    ++i;
    if (!num(h) || i >= n.size() || n[i] != '_') return false;
    ++i;
    if (i + 8 > n.size()) return false;
    crc = 0;
    for (size_t k = 0; k < 8; ++k) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(n[i + k])));
        if (c >= '0' && c <= '9') crc = crc << 4 | uint32_t(c - '0');
        else if (c >= 'a' && c <= 'f') crc = crc << 4 | uint32_t(c - 'a' + 10);
        else return false;
    }
    return w && h;
}

}  // namespace

uint32_t crc32(const uint8_t* data, size_t n) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string file_name(uint32_t width, uint32_t height, uint32_t crc) {
    char b[64];
    std::snprintf(b, sizeof b, "tex_%ux%u_%08X.dds", width, height, crc);
    return b;
}

size_t level_bytes(uint32_t dxgi, uint32_t w, uint32_t h) {
    const size_t bw = (std::max(1u, w) + 3) / 4, bh = (std::max(1u, h) + 3) / 4;
    switch (dxgi) {
        case 71: case 72: case 80: case 81: return bw * bh * 8;                       // BC1, BC4
        case 74: case 75: case 77: case 78: case 83: case 84: case 98: case 99:       // BC2, BC3, BC5, BC7
            return bw * bh * 16;
        case 28: case 29: case 87: case 91: return size_t{std::max(1u, w)} * std::max(1u, h) * 4;  // RGBA8, BGRA8
        default: return 0;
    }
}

bool read_dds(const stdfs::path& file, Dds& out, std::string* error) {
    const auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    std::ifstream f(file, std::ios::binary);
    if (!f) return fail("can't open");
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 128 || rd32(b.data()) != fourcc('D', 'D', 'S', ' ')) return fail("not a DDS file");
    const uint8_t* hd = b.data() + 4;
    out.height = rd32(hd + 8);
    out.width = rd32(hd + 12);
    out.levels = std::max(1u, rd32(hd + 24));
    const uint8_t* pf = hd + 72;  // DDS_PIXELFORMAT
    const uint32_t pf_flags = rd32(pf + 4), cc = rd32(pf + 8);
    size_t at = 128;
    if (pf_flags & 4) {  // fourcc
        if (cc == fourcc('D', 'X', '1', '0')) {
            if (b.size() < 148) return fail("truncated DX10 header");
            out.dxgi = rd32(b.data() + 128);
            if (rd32(b.data() + 128 + 4) != 3 || rd32(b.data() + 128 + 12) > 1) return fail("only single 2D textures are supported");
            at = 148;
        } else if (cc == fourcc('D', 'X', 'T', '1')) out.dxgi = 71;
        else if (cc == fourcc('D', 'X', 'T', '2') || cc == fourcc('D', 'X', 'T', '3')) out.dxgi = 74;
        else if (cc == fourcc('D', 'X', 'T', '4') || cc == fourcc('D', 'X', 'T', '5')) out.dxgi = 77;
        else if (cc == fourcc('A', 'T', 'I', '1') || cc == fourcc('B', 'C', '4', 'U')) out.dxgi = 80;
        else if (cc == fourcc('A', 'T', 'I', '2') || cc == fourcc('B', 'C', '5', 'U')) out.dxgi = 83;
        else return fail("unsupported DDS format");
    } else if ((pf_flags & 0x40) && rd32(pf + 12) == 32) {  // uncompressed 32-bit RGB(A)
        const uint32_t rmask = rd32(pf + 16);
        out.dxgi = rmask == 0x000000FFu ? 28 : rmask == 0x00FF0000u ? 87 : 0;
        if (!out.dxgi) return fail("unsupported DDS channel layout");
    } else {
        return fail("unsupported DDS format");
    }
    if (!out.width || !out.height || out.width > 16384 || out.height > 16384 || !level_bytes(out.dxgi, 4, 4))
        return fail("unsupported DDS size or format");
    out.offsets.clear();
    size_t total = 0;
    uint32_t levels = 0;
    for (; levels < out.levels; ++levels) {
        const size_t n = level_bytes(out.dxgi, out.width >> levels, out.height >> levels);
        if (at + total + n > b.size()) break;  // keep the levels that are present
        out.offsets.push_back(total);
        total += n;
    }
    if (!levels) return fail("truncated DDS data");
    out.levels = levels;
    out.data.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + total));
    return true;
}

bool write_dds(const stdfs::path& file, uint32_t dxgi, uint32_t width, uint32_t height, const uint8_t* data, size_t n) {
    std::array<uint8_t, 148> h{};
    wr32(&h[0], fourcc('D', 'D', 'S', ' '));
    uint8_t* hd = &h[4];
    wr32(hd + 0, 124);
    const bool block = level_bytes(dxgi, 4, 4) < 64;
    wr32(hd + 4, 0x1 | 0x2 | 0x4 | 0x1000 | (block ? 0x80000 : 0x8));  // caps, height, width, pixelformat, linear/pitch
    wr32(hd + 8, height);
    wr32(hd + 12, width);
    wr32(hd + 16, static_cast<uint32_t>(block ? level_bytes(dxgi, width, height) : size_t{width} * 4));
    wr32(hd + 24, 1);
    wr32(hd + 72, 32);
    wr32(hd + 76, 4);
    wr32(hd + 80, fourcc('D', 'X', '1', '0'));
    wr32(hd + 104, 0x1000);  // DDSCAPS_TEXTURE
    wr32(&h[128], dxgi);
    wr32(&h[132], 3);  // TEXTURE2D
    wr32(&h[140], 1);  // array size
    std::error_code ec;
    stdfs::create_directories(file.parent_path(), ec);
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(h.data()), static_cast<std::streamsize>(h.size()));
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

size_t Library::scan(const stdfs::path& dir) {
    files_.clear();
    std::error_code ec;
    std::vector<stdfs::path> found;
    for (auto it = stdfs::recursive_directory_iterator(dir, stdfs::directory_options::skip_permission_denied, ec);
         !ec && it != stdfs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (ext == ".dds") found.push_back(it->path());
    }
    std::sort(found.begin(), found.end());
    for (const auto& p : found) {
        uint32_t w, h, crc;
        if (parse_name(p.filename().string(), w, h, crc)) files_[key(w, h, crc)] = p;
    }
    return files_.size();
}

const stdfs::path* Library::find(uint32_t width, uint32_t height, uint32_t crc) const {
    auto it = files_.find(key(width, height, crc));
    return it != files_.end() ? &it->second : nullptr;
}

}  // namespace mh3u::gfx::texpack
