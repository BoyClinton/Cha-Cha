#include "game_image.h"

#include "zarchive.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#endif

namespace mh3u::fs {
namespace {

namespace stdfs = std::filesystem;

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

std::string hex16(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

// Text between <tag ...> and </tag> in meta.xml.
std::optional<std::string> xml_value(const std::string& xml, const char* tag) {
    const std::string open = std::string("<") + tag;
    std::size_t p = xml.find(open);
    while (p != std::string::npos && xml[p + open.size()] != ' ' && xml[p + open.size()] != '>') p = xml.find(open, p + 1);
    if (p == std::string::npos) return std::nullopt;
    const std::size_t gt = xml.find('>', p), end = gt == std::string::npos ? gt : xml.find('<', gt);
    if (end == std::string::npos) return std::nullopt;
    return xml.substr(gt + 1, end - gt - 1);
}

// The title in a tree whose root holds code/ content/ meta/. code/app.xml has the title's own id and version (hex);
// an update's meta/meta.xml carries the *base* title id (0005000010118300 in MH3U's v32), so it is only a fallback.
std::optional<Title> identify(std::shared_ptr<Tree> tree, std::string where) {
    if (!tree->stat("code")) return std::nullopt;
    std::optional<std::string> id, version;
    int version_base = 16;
    if (auto app = tree->read_all("code/app.xml")) {
        const std::string text(app->begin(), app->end());
        id = xml_value(text, "title_id");
        version = xml_value(text, "title_version");
    }
    if (!id) {
        auto meta = tree->read_all("meta/meta.xml");
        if (!meta) return std::nullopt;
        const std::string text(meta->begin(), meta->end());
        id = xml_value(text, "title_id");
        version = xml_value(text, "title_version");
        version_base = 10;
        if (!id) return std::nullopt;
    }
    Title t;
    t.id = std::strtoull(id->c_str(), nullptr, 16);
    t.version = version ? static_cast<std::uint32_t>(std::strtoul(version->c_str(), nullptr, version_base)) : 0;
    t.where = std::move(where);
    t.tree = std::move(tree);
    return t;
}

bool is_title_dir(const stdfs::path& p) {
    std::error_code ec;
    return stdfs::is_directory(p / "code", ec) && (stdfs::is_regular_file(p / "code" / "app.xml", ec) || stdfs::is_regular_file(p / "meta" / "meta.xml", ec));
}

// Title folders at most `depth` levels below dir (Dumpling: Games/<name>, Cemu: mlc01/usr/title/<hi>/<lo>).
// Does not descend into a title folder, and gives up after a few thousand folders (someone picked a whole drive).
void find_titles(const stdfs::path& dir, int depth, std::vector<Title>& out, int& budget) {
    if (is_title_dir(dir)) {
        if (auto t = identify(host_tree(dir), dir.string())) out.push_back(std::move(*t));
        return;
    }
    if (depth == 0) return;
    std::error_code ec;
    std::vector<stdfs::path> subs;
    for (const auto& e : stdfs::directory_iterator(dir, stdfs::directory_options::skip_permission_denied, ec)) {
        if (--budget < 0) return;
        if (e.is_directory(ec)) subs.push_back(e.path());
    }
    std::sort(subs.begin(), subs.end());
    for (const auto& s : subs) find_titles(s, depth - 1, out, budget);
}

const char* encrypted_hint =
    "It is an encrypted dump, which this port cannot read yet. Convert it in Cemu (Tools > Title Manager > "
    "\"Convert to compressed Wii U archive (.wua)\") or choose the decrypted folders (code, content, meta).";

}  // namespace

#ifndef _WIN32
namespace {
// SHA-1 (FIPS 180-4), for checking the executable; BCrypt does it on Windows
void sha1(const std::uint8_t* data, std::size_t size, std::uint8_t digest[20]) {
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    const auto block = [&](const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = std::uint32_t{p[4 * i]} << 24 | std::uint32_t{p[4 * i + 1]} << 16 | std::uint32_t{p[4 * i + 2]} << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            const std::uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            const std::uint32_t k = i < 20 ? 0x5A827999 : i < 40 ? 0x6ED9EBA1 : i < 60 ? 0x8F1BBCDC : 0xCA62C1D6;
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    };
    std::size_t i = 0;
    for (; i + 64 <= size; i += 64) block(data + i);
    std::uint8_t tail[128] = {};
    const std::size_t rest = size - i;
    std::memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const std::size_t len = rest + 9 <= 64 ? 64 : 128;
    const std::uint64_t bits = std::uint64_t{size} * 8;
    for (int j = 0; j < 8; ++j) tail[len - 1 - j] = static_cast<std::uint8_t>(bits >> (8 * j));
    block(tail);
    if (len == 128) block(tail + 64);
    for (int j = 0; j < 20; ++j) digest[j] = static_cast<std::uint8_t>(h[j / 4] >> (24 - 8 * (j % 4)));
}
}  // namespace
#endif

std::string sha1_hex(const std::uint8_t* data, std::size_t size) {
    std::uint8_t digest[20] = {};
#ifdef _WIN32
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data), static_cast<ULONG>(size), digest, sizeof digest);
#else
    sha1(data, size, digest);
#endif
    std::string out;
    char b[3];
    for (std::uint8_t v : digest) {
        std::snprintf(b, sizeof b, "%02x", v);
        out += b;
    }
    return out;
}

std::optional<std::vector<Title>> scan_source(const stdfs::path& path, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<std::vector<Title>> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    std::error_code ec;
    const std::string name = path.filename().string();
    if (!stdfs::exists(path, ec)) return fail("\"" + path.string() + "\" does not exist.");
    std::vector<Title> out;
    if (stdfs::is_regular_file(path, ec)) {
        const std::string ext = lower(path.extension().string());
        if (ext == ".wud" || ext == ".wux" || ext == ".app" || ext == ".tmd" || ext == ".tik" || lower(name) == "title.tmd")
            return fail("\"" + name + "\": " + encrypted_hint);
        if (ext == ".rpx" || ext == ".elf") {  // code/<name>.rpx inside a title folder
            const stdfs::path title = path.parent_path().parent_path();
            if (!is_title_dir(title)) return fail("\"" + name + "\" is not inside a title folder (code, content, meta).");
            return scan_source(title, error);
        }
        std::string err;
        auto archive = ZArchive::open(path, &err);
        if (!archive) return fail("\"" + name + "\" is not a .wua file Cemu made, or not a game dump (" + err + ").");
        const auto& root = archive->node(ZArchive::kRoot);
        for (std::uint32_t c = root.first; c < root.first + root.count; ++c) {
            if (archive->node(c).is_file) continue;
            const std::string folder = archive->node(c).name;
            if (auto t = identify(archive->subtree(folder), name + ": " + folder)) out.push_back(std::move(*t));
        }
        return out;
    }
    if (stdfs::exists(path / "title.tmd", ec)) return fail("\"" + path.string() + "\": " + encrypted_hint);
    int budget = 5000;
    find_titles(path, 4, out, budget);
    return out;
}

std::optional<GameImage> open_game(const std::vector<stdfs::path>& sources, std::string* error) {
    auto fail = [&](std::string why) -> std::optional<GameImage> {
        if (error) *error = std::move(why);
        return std::nullopt;
    };
    GameImage g;
    g.sources = sources;
    std::vector<Title> titles;
    for (const auto& s : sources) {
        auto found = scan_source(s, error);
        if (!found) return std::nullopt;
        for (auto& t : *found) titles.push_back(std::move(t));
    }
    const auto newest = [&](std::uint64_t id) -> const Title* {
        const Title* best = nullptr;
        for (const auto& t : titles)
            if (t.id == id && (!best || t.version > best->version)) best = &t;
        return best;
    };
    const Title* base = newest(kBaseTitleId);
    const Title* update = newest(kUpdateTitleId);
    if (!base) {
        if (titles.empty()) return fail("No Wii U game was found there. Choose a .wua file or a game folder (code, content, meta).");
        std::set<std::string> ids;
        for (const auto& t : titles) ids.insert(hex16(t.id));
        std::string list;
        for (const auto& id : ids) list += (list.empty() ? "" : ", ") + id;
        if (update) return fail("Found the update but not the game itself (" + hex16(kBaseTitleId) + "). Add the base game too.");
        return fail("This is not Monster Hunter 3 Ultimate (US, " + hex16(kBaseTitleId) + "); found " + list +
                    ". Other regions are not supported: the port is built from the US executable.");
    }
    g.base = *base;
    if (update) g.update = *update;
    OverlayFs fs = make_title_fs(g.base.tree, g.update.tree, {});
    auto rpx = fs.open(std::string("/vol/code/") + kRpxName);
    if (!rpx) return fail(std::string("The game's executable (code/") + kRpxName + ") is missing from the dump.");
    g.rpx.resize(static_cast<std::size_t>(rpx->size()));
    if (rpx->read(0, g.rpx.data(), g.rpx.size()) != g.rpx.size()) return fail("Cannot read the game's executable.");
    g.rpx_sha1 = sha1_hex(g.rpx.data(), g.rpx.size());
    if (g.rpx_sha1 != kTargetRpxSha1) {
        if (!update) return fail("The update (v" + std::to_string(kTargetUpdateVersion) + ", title " + hex16(kUpdateTitleId) +
                                 ") is missing. Add the update folder, or a .wua that includes it.");
        if (update->version != kTargetUpdateVersion)
            return fail("Found update v" + std::to_string(update->version) + "; this port needs update v" +
                        std::to_string(kTargetUpdateVersion) + ", the last one.");
        return fail("The game's executable does not match the US v32 release (SHA-1 " + g.rpx_sha1 +
                    "). The dump may be modified or damaged: dump it again.");
    }
    return g;
}

}  // namespace mh3u::fs
