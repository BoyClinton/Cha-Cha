// Checks the recompiler loader on the user's executable. Exit 77 = skipped (no dump).
// Usage: loader_check <repo root> [ghidra blocks dir]
//   Always: relocations apply; non-import relocations change nothing (the file is linked at its final addresses);
//   every import call site is a branch; the imports match config/imports.csv (static imports).
//   With a directory from ghidra/scripts/ExportBlocks.java: .text/.rodata/.data must equal Ghidra's relocated
//   memory byte for byte.
#include "espresso.h"
#include "loader.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

namespace stdfs = std::filesystem;
using namespace mh3u;

static int failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            std::printf("FAIL %s: ", #cond);              \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
            ++failures;                                   \
        }                                                 \
    } while (0)

int main(int argc, char** argv) {
    stdfs::path repo = argc > 1 ? argv[1] : ".";
    stdfs::path elf_path = repo / "extracted/0005000e10118300_v32/code/MH3G_Cafe_US.elf";
    if (!stdfs::exists(elf_path)) {
        std::printf("skipped: no %s\n", elf_path.string().c_str());
        return 77;
    }
    std::string err;
    auto img = recomp::load_image(elf_path, &err);
    if (!img) {
        std::printf("FAIL load: %s\n", err.c_str());
        return 1;
    }
    // The RPX as stored in the dump, inflated by our own decoder, equals wiiurpxtool's ELF section by section.
    {
        std::ifstream rf(elf_path.parent_path() / "MH3G_Cafe_US.rpx", std::ios::binary);
        const std::vector<uint8_t> rpx((std::istreambuf_iterator<char>(rf)), std::istreambuf_iterator<char>());
        auto from_rpx = recomp::ElfImage::parse(rpx, &err);
        auto from_elf = recomp::ElfImage::load(elf_path, &err);
        CHECK(from_rpx && from_elf, "parse: %s", err.c_str());
        if (from_rpx && from_elf) {
            size_t same = 0, compared = 0;
            CHECK(from_rpx->sections.size() == from_elf->sections.size(), "%zu vs %zu sections", from_rpx->sections.size(),
                  from_elf->sections.size());
            for (size_t i = 0; i < std::min(from_rpx->sections.size(), from_elf->sections.size()); ++i) {
                const auto &a = from_rpx->sections[i], &b = from_elf->sections[i];
                if (a.type == 0 || a.type == 8) continue;
                ++compared;
                const bool eq = a.name == b.name && a.addr == b.addr && a.data == b.data;
                same += eq;
                CHECK(eq, "section %zu (%s): rpx %u bytes vs elf %u bytes", i, a.name.c_str(), a.size, b.size);
            }
            std::printf("RPX inflated: %zu/%zu sections equal to the ELF (%zu bytes compressed)\n", same, compared, rpx.size());
        }
    }
    const auto& st = img->stats;
    std::printf("relocations=%zu (", st.applied);
    for (auto [t, n] : st.by_type) std::printf(" type%d=%zu", t, n);
    std::printf(" ) changed: internal=%zu import=%zu; imports=%zu, import refs=%zu, import calls=%zu, $UNDEF refs=%zu\n",
                st.changed_internal, st.changed_import, img->imports.size(), img->import_refs.size(),
                img->import_calls.size(), img->undefined_refs.size());
    CHECK(st.changed_internal == 0, "%zu internal relocations disagree with the file", st.changed_internal);

    // every import call site is a b/bl
    size_t non_branch = 0;
    for (auto [site, idx] : img->import_calls) {
        auto in = ppc::decode(*img->elf.read32(site));
        if (in.op != ppc::Op::b) ++non_branch;
    }
    CHECK(non_branch == 0, "%zu import call sites are not branches", non_branch);

    // imports vs config/imports.csv (kind func/data = the static import tables)
    std::set<std::string> ours, theirs;
    for (const auto& im : img->imports) {
        std::string rpl = im.rpl;
        if (rpl.size() > 4 && rpl.compare(rpl.size() - 4, 4, ".rpl") == 0) rpl.resize(rpl.size() - 4);
        ours.insert(rpl + ":" + im.name + (im.data ? ":data" : ":func"));
    }
    std::ifstream csv(repo / "config/imports.csv");
    std::string line;
    std::getline(csv, line);
    while (std::getline(csv, line)) {
        size_t c1 = line.find(','), c2 = line.find(',', c1 + 1), c3 = line.find(',', c2 + 1);
        std::string rpl = line.substr(0, c1), fn = line.substr(c1 + 1, c2 - c1 - 1), kind = line.substr(c2 + 1, c3 - c2 - 1);
        if (rpl.size() > 4 && rpl.compare(rpl.size() - 4, 4, ".rpl") == 0) rpl.resize(rpl.size() - 4);
        if (kind == "func" || kind == "data") theirs.insert(rpl + ":" + fn + ":" + kind);
    }
    size_t missing = 0, extra = 0;
    for (auto& s : theirs)
        if (!ours.count(s)) { if (missing++ < 5) std::printf("  imports.csv only: %s\n", s.c_str()); }
    for (auto& s : ours)
        if (!theirs.count(s)) { if (extra++ < 5) std::printf("  loader only: %s\n", s.c_str()); }
    std::printf("imports: loader=%zu imports.csv=%zu missing=%zu extra=%zu\n", ours.size(), theirs.size(), missing, extra);
    CHECK(missing == 0 && extra == 0, "import sets differ");

    if (argc > 2) {
        for (const char* name : {"text", "rodata", "data"}) {
            const recomp::Section* s = img->elf.section(std::string(".") + name);
            std::ifstream f(stdfs::path(argv[2]) / (std::string(name) + ".bin"), std::ios::binary);
            std::vector<uint8_t> g((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            // Ghidra resolves $UNDEF to a placeholder address; the Cafe loader (and we) use 0. Those sites are the only
            // allowed differences.
            std::set<uint32_t> undef_words;
            for (uint32_t p : img->undefined_refs) undef_words.insert(p & ~3u);
            size_t diff = 0, shown = 0, undef_diff = 0;
            size_t n = std::min(g.size(), s->data.size());
            for (size_t i = 0; i + 4 <= n; i += 4) {
                if (std::equal(g.begin() + i, g.begin() + i + 4, s->data.begin() + i)) continue;
                if (undef_words.count(s->addr + static_cast<uint32_t>(i))) {
                    ++undef_diff;
                    continue;
                }
                ++diff;
                if (shown++ < 8) {
                    uint32_t a = s->addr + static_cast<uint32_t>(i);
                    auto w = [&](const std::vector<uint8_t>& d) {
                        return (uint32_t{d[i]} << 24) | (uint32_t{d[i + 1]} << 16) | (uint32_t{d[i + 2]} << 8) | d[i + 3];
                    };
                    std::printf("  %s %08X ours=%08X ghidra=%08X\n", name, a, w(s->data), w(g));
                }
            }
            std::printf("%s: %zu bytes (ghidra %zu), differing words=%zu (+%zu at $UNDEF sites)\n", name,
                        s->data.size(), g.size(), diff, undef_diff);
            CHECK(g.size() == s->data.size() && diff == 0, "%s differs from Ghidra", name);
        }
    }
    std::printf(failures ? "%d failure(s)\n" : "all checks passed\n", failures);
    return failures ? 1 : 0;
}
