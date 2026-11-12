// The whole game's textures written out for texture packs: every rTexture in every .arc under /vol/content (the
// update shadowing the base, as the game sees it), as tex_<w>x<h>_<CRC32>.dds - the CRC over level 0 as the file
// holds it, which is how it sits in guest memory (GX2-tiled), so the names are the ones the renderer looks up and
// its own dump writes (runtime/gfx/renderer.cpp replace_texture). Level 0 detiled, in the texture's format (BC1, BC3,
// RGBA8). Cube maps are left out, as packs cannot replace them.
//
// ARC (docs/formats/arc.md): u32 magic 0x00435241, u16 version, u16 count, then 0x50-byte entries (path[64], type
// hash, compressed size, size << 3 | flags, offset); entries are zlib streams. TEX (docs/formats/tex.md,
// scripts/mt_tex.py): a 16-byte header (mips, width, height, faces, format), level 0 right after it; 2D-tiled
// (macro tiles of 32 x 16 elements) unless both texel dimensions are below that, else 1D (8 x 8).
#include "texture_dump.h"

#include "inflate.h"
#include "overlay_fs.h"
#include "texture_pack.h"
#include "tiling.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace mh3u::boot {
namespace {

constexpr uint32_t kArcMagic = 0x00435241, kTexMagic = 0x00584554, kTexture = 0x241F5DEB;
constexpr size_t kEntry = 0x50;

uint32_t be32(const uint8_t* p) { return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>(p[0] << 8 | p[1]); }
uint32_t align(uint32_t x, uint32_t a) { return (x + a - 1) / a * a; }

void find_arcs(const fs::OverlayFs& vfs, const std::string& dir, std::vector<std::string>& out) {
    for (const fs::DirEntry& e : vfs.list(dir)) {
        const std::string p = dir + "/" + e.name;
        if (e.is_dir) find_arcs(vfs, p, out);
        else if (p.size() > 4 && (p.ends_with(".arc") || p.ends_with(".ARC"))) out.push_back(p);
    }
}

std::vector<uint8_t> read_all(const fs::OverlayFs& vfs, const std::string& path, size_t limit = SIZE_MAX) {
    std::vector<uint8_t> d;
    if (auto f = vfs.open(path)) {
        d.resize(static_cast<size_t>(std::min<uint64_t>(f->size(), limit)));
        d.resize(f->read(0, d.data(), d.size()));
    }
    return d;
}

// texture entries of an arc whose data is at hand (header only for counting)
size_t count_textures(const std::vector<uint8_t>& d) {
    if (d.size() < 12 || be32(d.data()) != kArcMagic) return 0;
    const size_t n = be16(d.data() + 6);
    size_t k = 0;
    for (size_t i = 0; i < n && 12 + (i + 1) * kEntry <= d.size(); ++i)
        if (be32(d.data() + 12 + i * kEntry + 64) == kTexture) ++k;
    return k;
}

}  // namespace

TextureDump::~TextureDump() {
    if (worker_.joinable()) worker_.join();
}

void TextureDump::start(const fs::OverlayFs& vfs, std::filesystem::path out_dir) {
    if (running_.exchange(true)) return;
    if (worker_.joinable()) worker_.join();
    out_ = std::move(out_dir);
    done_ = total_ = written_ = 0;
    finished_ = false;
    cancel_ = false;
    worker_ = std::thread([this, &vfs] { run(vfs); });
}

size_t TextureDump::run_now(const fs::OverlayFs& vfs, std::filesystem::path out_dir) {
    if (running_.exchange(true)) return 0;
    out_ = std::move(out_dir);
    done_ = total_ = written_ = 0;
    finished_ = false;
    run(vfs);
    return written_.load();
}

void TextureDump::run(const fs::OverlayFs& vfs) {
    std::error_code ec;
    std::filesystem::create_directories(out_, ec);
    std::vector<std::string> arcs;
    find_arcs(vfs, "/vol/content", arcs);
    size_t total = 0;
    for (const std::string& a : arcs) {
        std::vector<uint8_t> head = read_all(vfs, a, 12);
        if (head.size() == 12 && be32(head.data()) == kArcMagic) total += count_textures(read_all(vfs, a, 12 + be16(head.data() + 6) * kEntry));
    }
    total_ = total;
    std::printf("texture dump: %zu textures in %zu archives -> %s\n", total, arcs.size(), reinterpret_cast<const char*>(out_.u8string().c_str()));
    std::set<uint64_t> seen;
    std::vector<uint8_t> tex, linear;
    for (const std::string& a : arcs) {
        if (cancel_) break;
        const std::vector<uint8_t> d = read_all(vfs, a);
        if (d.size() < 12 || be32(d.data()) != kArcMagic) continue;
        const size_t n = be16(d.data() + 6);
        for (size_t i = 0; i < n && 12 + (i + 1) * kEntry <= d.size(); ++i) {
            const uint8_t* e = d.data() + 12 + i * kEntry;
            if (be32(e + 64) != kTexture) continue;
            ++done_;
            const uint32_t csize = be32(e + 68), size = be32(e + 72) >> 3, off = be32(e + 76);
            if (size_t{off} + csize > d.size()) continue;
            tex.clear();
            if (!recomp::zlib_inflate(d.data() + off, csize, tex)) tex.assign(d.data() + off, d.data() + off + csize);
            if (tex.size() < 16 || be32(tex.data()) != kTexMagic || (size && tex.size() != size)) continue;
            const uint32_t b = be32(tex.data() + 8), c = be32(tex.data() + 12);
            const uint32_t w = (b >> 13) & 0x1FFF, h = b & 0x1FFF, faces = c >> 24, format = (c >> 16) & 0xFF;
            uint32_t block = 0, bpe = 0, dxgi = 0;
            switch (format) {
                case 0x03: block = 1, bpe = 4, dxgi = 28; break;  // RGBA8
                case 0x0B: block = 4, bpe = 8, dxgi = 71; break;  // BC1
                case 0x0C: block = 4, bpe = 16, dxgi = 77; break;  // BC3
                default: continue;
            }
            if (faces != 1 || !w || !h) continue;
            const uint32_t ew = (w + block - 1) / block, eh = (h + block - 1) / block;
            const bool tiled2d = !(w < 32 && h < 16);
            const uint32_t pitch = tiled2d ? align(ew, 32) : align(ew, 8), rows = tiled2d ? align(eh, 16) : align(eh, 8);
            const size_t level0 = size_t{pitch} * rows * bpe;
            if (16 + level0 > tex.size()) continue;
            const uint32_t crc = gfx::texpack::crc32(tex.data() + 16, level0);
            if (!seen.insert(uint64_t{w} << 48 | uint64_t{h} << 32 | crc).second) continue;
            const std::filesystem::path file = out_ / gfx::texpack::file_name(w, h, crc);
            if (std::filesystem::exists(file, ec)) continue;
            gfx::SurfaceLevel s;
            s.width = ew, s.height = eh, s.bpp = bpe * 8, s.tile_mode = tiled2d ? 4 : 2, s.pitch = pitch;
            linear.assign(size_t{ew} * eh * bpe, 0);
            gfx::detile(tex.data() + 16, linear.data(), s);
            if (gfx::texpack::write_dds(file, dxgi, w, h, linear.data(), linear.size())) ++written_;
        }
    }
    std::printf("texture dump: %s, %zu files written (%zu textures)\n", cancel_ ? "cancelled" : "done", written_.load(), done_.load());
    finished_ = !cancel_;
    running_ = false;
}

std::string texture_dump_status(const TextureDump& d) {
    char b[200];
    if (d.running()) {
        if (!d.total()) return "Saving textures...\r\nLooking through the game's archives.";
        std::snprintf(b, sizeof b, "Saving textures...  %zu / %zu  (%zu%%)\r\nto the texture_dump folder.", d.done(), d.total(),
                      d.done() * 100 / d.total());
        return b;
    }
    if (!d.finished_.load()) return {};
    std::snprintf(b, sizeof b, "Done: %zu new textures saved in the\r\ntexture_dump folder.", d.written());
    return b;
}

}  // namespace mh3u::boot
