// Decodes every instruction word inside the function bodies of config/functions.csv (entry range + chunks),
// read from the user's decompressed ELF. Fails if any word does not decode. Exit 77 = skipped (no dump).
// Usage: decoder_coverage <repo root> [distinct-words.tsv]
//   The optional TSV lists each distinct word once: "hexword<TAB>canonical text<TAB>first address", used to
//   cross-check against Ghidra's disassembly (scripts/check_decoder.py).
#include "elf.h"
#include "espresso.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace stdfs = std::filesystem;
using namespace mh3u;

int main(int argc, char** argv) {
    stdfs::path repo = argc > 1 ? argv[1] : ".";
    stdfs::path elf_path = repo / "extracted/0005000e10118300_v32/code/MH3G_Cafe_US.elf";
    if (!stdfs::exists(elf_path)) {
        std::printf("skipped: no %s\n", elf_path.string().c_str());
        return 77;
    }
    std::string err;
    auto img = recomp::ElfImage::load(elf_path, &err);
    if (!img) {
        std::printf("FAIL loading ELF: %s\n", err.c_str());
        return 1;
    }
    std::ifstream csv(repo / "config/functions.csv");
    if (!csv) {
        std::printf("FAIL: no config/functions.csv\n");
        return 1;
    }

    // ranges: entry range [start,end) plus "AAAAAAAA-BBBBBBBB" chunks in the last column
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::string line;
    std::getline(csv, line);
    size_t functions = 0;
    while (std::getline(csv, line)) {
        if (line.empty()) continue;
        ++functions;
        uint32_t s = std::stoul(line.substr(0, 10), nullptr, 16), e = std::stoul(line.substr(11, 10), nullptr, 16);
        ranges.emplace_back(s, e);
        size_t last = line.rfind(',');
        std::istringstream chunks(line.substr(last + 1));
        std::string c;
        while (chunks >> c)
            if (c.size() == 17 && c[8] == '-')
                ranges.emplace_back(std::stoul(c.substr(0, 8), nullptr, 16), std::stoul(c.substr(9, 8), nullptr, 16));
    }

    size_t words = 0, invalid = 0;
    std::map<std::string, size_t> hist;
    std::unordered_map<uint32_t, uint32_t> distinct;  // word -> first address
    std::vector<std::pair<uint32_t, uint32_t>> bad;
    for (auto [s, e] : ranges) {
        for (uint32_t a = s; a + 4 <= e; a += 4) {
            auto w = img->read32(a);
            if (!w) {
                std::printf("FAIL: %08X outside the ELF\n", a);
                return 1;
            }
            auto in = ppc::decode(*w);
            ++words;
            if (!in.valid()) {
                ++invalid;
                if (bad.size() < 20) bad.emplace_back(a, *w);
                continue;
            }
            ++hist[ppc::mnemonic(in.op)];
            distinct.emplace(*w, a);
        }
    }
    std::printf("functions=%zu ranges=%zu words=%zu invalid=%zu distinct=%zu ops used=%zu\n", functions,
                ranges.size(), words, invalid, distinct.size(), hist.size());
    std::vector<std::pair<size_t, std::string>> top;
    for (auto& [k, v] : hist) top.emplace_back(v, k);
    std::sort(top.rbegin(), top.rend());
    std::string paired;
    size_t ps = 0;
    for (auto& [v, k] : top)
        if (k.rfind("ps", 0) == 0) ps += v, paired += k + " ";
    std::printf("paired-single instructions=%zu (%s)\n", ps, paired.c_str());
    for (size_t i = 0; i < top.size() && i < 12; ++i) std::printf("  %9zu %s\n", top[i].first, top[i].second.c_str());
    for (auto [a, w] : bad) std::printf("  invalid %08X: %08X\n", a, w);

    if (argc > 2) {
        std::ofstream out(argv[2]);
        for (auto [w, a] : distinct) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%08X\t", w);
            out << buf << ppc::format(ppc::decode(w), a);
            std::snprintf(buf, sizeof buf, "\t%08X\n", a);
            out << buf;
        }
    }
    return invalid ? 1 : 0;
}
