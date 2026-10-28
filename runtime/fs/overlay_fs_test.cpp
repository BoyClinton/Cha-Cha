// Tests OverlayFs and the game sources against the user's dump. Exit 77 = skipped.
//   extracted/ (base v0 + update v32): layering, path normalisation, merged listings, the save mount.
//   game/*.wua, if present: open_game finds base + update and verifies the RPX; every file of both titles reads
//   back byte for byte equal to extracted/ (the archive reader against zarchive.exe's extraction).
// Expectations come from the content inventory: the update replaces e.g. arc/ID/ID_com_eng.arc, adds
// arc/ID/ID_cock_tu2_eng.arc, and leaves arc/ainpc/ainpc.arc to the base title.
#include "game_image.h"
#include "overlay_fs.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>

namespace stdfs = std::filesystem;
using namespace mh3u::fs;

static int failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

// Every file below dir in `fs` equals the host file below host; returns the bytes compared.
static uint64_t compare_tree(const OverlayFs& fs, const std::string& dir, const stdfs::path& host, int& files) {
    uint64_t bytes = 0;
    std::vector<uint8_t> a, b;
    for (const DirEntry& e : fs.list(dir)) {
        const std::string p = dir + "/" + e.name;
        if (e.is_dir) {
            bytes += compare_tree(fs, p, host / e.name, files);
            continue;
        }
        auto f = fs.open(p);
        std::ifstream h(host / e.name, std::ios::binary);
        CHECK(f && h);
        if (!f || !h) continue;
        a.resize(static_cast<size_t>(f->size()));
        CHECK(f->read(0, a.data(), a.size()) == a.size());
        b.assign(std::istreambuf_iterator<char>(h), std::istreambuf_iterator<char>());
        if (a != b) {
            std::printf("FAIL %s differs from %s\n", p.c_str(), (host / e.name).string().c_str());
            ++failures;
        }
        // an unaligned read across a block boundary
        if (a.size() > 70000) {
            uint8_t mid[5000];
            CHECK(f->read(65000, mid, sizeof mid) == sizeof mid && std::memcmp(mid, a.data() + 65000, sizeof mid) == 0);
        }
        ++files;
        bytes += a.size();
    }
    return bytes;
}

int main(int argc, char** argv) {
    stdfs::path repo = argc > 1 ? argv[1] : ".";
    stdfs::path base = repo / "extracted/0005000010118300_v0";
    stdfs::path update = repo / "extracted/0005000e10118300_v32";
    if (!stdfs::exists(base / "content") || !stdfs::exists(update / "content")) {
        std::printf("skipped: no extracted dump under %s\n", repo.string().c_str());
        return 77;
    }
    stdfs::path save = stdfs::temp_directory_path() / "mh3u_fs_test_save";
    stdfs::remove_all(save);
    OverlayFs fs = make_title_fs(host_tree(base), host_tree(update), save);

    // Update shadows base for replaced files; base serves untouched files; update-only files appear.
    CHECK(fs.layer_of("/vol/content/nativeCafe/arc/ID/ID_com_eng.arc") == 0u);
    CHECK(stdfs::exists(base / "content/nativeCafe/arc/ID/ID_com_eng.arc"));
    CHECK(fs.layer_of("/vol/content/nativeCafe/arc/ainpc/ainpc.arc") == 1u);
    CHECK(fs.layer_of("/vol/content/nativeCafe/arc/ID/ID_cock_tu2_eng.arc") == 0u);
    CHECK(!stdfs::exists(base / "content/nativeCafe/arc/ID/ID_cock_tu2_eng.arc"));
    CHECK(fs.layer_of("/vol/code/MH3G_Cafe_US.rpx") == 0u);
    auto rpx = fs.open("/vol/code/MH3G_Cafe_US.rpx");
    CHECK(rpx && rpx->size() == stdfs::file_size(update / "code/MH3G_Cafe_US.rpx"));

    // Path forms: backslashes, doubled separators, "." and ".." are normalised.
    CHECK(fs.layer_of("/vol/content/nativeCafe\\arc//ID/./ID_com_eng.arc") == 0u);
    CHECK(fs.layer_of("/vol/content/nativeCafe/arc/../arc/ainpc/ainpc.arc") == 1u);

    // Misses: absent file, outside every mount, climbing above "/".
    CHECK(!fs.stat("/vol/content/nativeCafe/does_not_exist.arc"));
    CHECK(!fs.stat("/vol/other/x"));
    CHECK(!fs.stat("/../../Windows/win.ini"));
    CHECK(!fs.stat("/vol/content/../../../x"));
    CHECK(fs.stat("/vol/content/nativeCafe") && fs.stat("/vol/content/nativeCafe")->is_dir);
    CHECK(!fs.open("/vol/content/nativeCafe"));

    // Merged listing of arc/ID = base names plus update-only names, replaced files taken from the update.
    auto id = fs.list("/vol/content/nativeCafe/arc/ID");
    std::set<std::string> names;
    for (auto& e : stdfs::directory_iterator(base / "content/nativeCafe/arc/ID")) names.insert(e.path().filename().string());
    std::size_t base_count = names.size();
    for (auto& e : stdfs::directory_iterator(update / "content/nativeCafe/arc/ID")) names.insert(e.path().filename().string());
    CHECK(id.size() == names.size());
    CHECK(id.size() > base_count);
    bool replaced_from_update = false, untouched_from_base = false;
    for (auto& e : id) {
        if (e.name == "ID_com_eng.arc") replaced_from_update = e.layer == 0;
        if (e.name == "ID_cock_tu2_eng.arc") CHECK(e.layer == 0);
        if (!e.is_dir && e.layer == 1) untouched_from_base = true;
    }
    CHECK(replaced_from_update);
    CHECK(untouched_from_base);
    std::printf("arc/ID: %zu entries (base %zu)\n", id.size(), base_count);

    // Save mount: writable, creates parents, reads back; read-only mounts refuse writes.
    auto w = fs.resolve_for_write("/vol/save/common/slot0.bin");
    CHECK(w && w->parent_path() == save / "common");
    if (w) std::ofstream(*w) << "x";
    CHECK(fs.stat("/vol/save/common/slot0.bin") && fs.stat("/vol/save/common/slot0.bin")->size == 1);
    CHECK(!fs.resolve_for_write("/vol/content/nativeCafe/new.bin"));
    stdfs::remove_all(save);

    // Sources: the extracted folders as one source of title folders, and the .rpx inside a title folder.
    std::string err;
    auto g = open_game({repo / "extracted"}, &err);
    CHECK(g && g->base.version == 0 && g->update.version == 32);
    if (!g) std::printf("open_game(extracted): %s\n", err.c_str());
    CHECK(!open_game({base}, &err));  // base alone: the update is missing
    std::printf("base alone: %s\n", err.c_str());
    CHECK(open_game({base / "code/MH3G_Cafe_US.rpx", update}, &err));

    // The .wua: same titles, every file equal to the extraction.
    std::error_code ec;
    for (const auto& e : stdfs::directory_iterator(repo / "game", ec)) {
        if (e.path().extension() != ".wua") continue;
        const auto t0 = std::chrono::steady_clock::now();
        auto w2 = open_game({e.path()}, &err);
        CHECK(w2);
        if (!w2) {
            std::printf("open_game(%s): %s\n", e.path().string().c_str(), err.c_str());
            continue;
        }
        std::printf("%s: base %s, update %s (v%u), rpx %s\n", e.path().filename().string().c_str(), w2->base.where.c_str(),
                    w2->update.where.c_str(), w2->update.version, w2->rpx_sha1.c_str());
        int files = 0;
        uint64_t bytes = 0;
        const OverlayFs b = make_title_fs(w2->base.tree, nullptr, {}), u = make_title_fs(w2->update.tree, nullptr, {});
        // the whole titles (~6 GB from a spinning disk, minutes) only with MH3U_TEST_FULL_WUA=1
        char* env = nullptr;
        size_t env_len = 0;
        const bool full = _dupenv_s(&env, &env_len, "MH3U_TEST_FULL_WUA") == 0 && env;
        std::free(env);
        const std::vector<std::string> dirs = full ? std::vector<std::string>{"code", "content"}
                                                   : std::vector<std::string>{"code", "content/nativeCafe/arc/ID", "content/nativeCafe/es"};
        for (const std::string& dir : dirs) {
            const std::string g_dir = "/vol/" + dir;
            if (b.stat(g_dir)) bytes += compare_tree(b, g_dir, base / dir, files);
            if (u.stat(g_dir)) bytes += compare_tree(u, g_dir, update / dir, files);
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("  %d files, %.1f MB equal to extracted/ (%.1f s, %.0f MB/s)\n", files, bytes / 1e6, secs, bytes / 1e6 / secs);
    }

    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
