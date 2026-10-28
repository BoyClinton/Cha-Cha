// Disassembles guest code from the relocated image, naming import calls and symbols.
// Usage: recomp_disasm <repo root> <hex addr> [instruction count | f]   ("f" = the whole function from functions.csv)
#include "espresso.h"
#include "loader.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace stdfs = std::filesystem;
using namespace mh3u;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: recomp_disasm <repo> <hex addr> [count|f]\n");
        return 2;
    }
    stdfs::path repo = argv[1];
    std::string err;
    auto img = recomp::load_image(repo / "extracted/0005000e10118300_v32/code/MH3G_Cafe_US.elf", &err);
    if (!img) {
        std::fprintf(stderr, "load failed: %s\n", err.c_str());
        return 1;
    }
    std::map<uint32_t, std::string> names;  // symbols.txt: "0xADDR name kind # note"
    std::ifstream sym(repo / "config/symbols.txt");
    for (std::string line; std::getline(sym, line);) {
        if (line.rfind("0x", 0) != 0) continue;
        std::istringstream ss(line);
        std::string a, n;
        ss >> a >> n;
        names[std::stoul(a, nullptr, 16)] = n;
    }
    uint32_t addr = std::stoul(argv[2], nullptr, 16), end = addr + 4 * 40;
    if (argc > 3 && std::string(argv[3]) == "f") {
        std::ifstream csv(repo / "config/functions.csv");
        std::string line;
        std::getline(csv, line);
        while (std::getline(csv, line))
            if (std::stoul(line.substr(0, 10), nullptr, 16) == addr) end = std::stoul(line.substr(11, 10), nullptr, 16);
    } else if (argc > 3) {
        end = addr + 4 * std::stoul(argv[3]);
    }
    for (uint32_t a = addr; a < end; a += 4) {
        auto w = img->elf.read32(a);
        if (!w) break;
        auto in = ppc::decode(*w);
        std::string text = ppc::format(in, a), note;
        if (auto it = names.find(a); it != names.end()) std::printf("%s:\n", it->second.c_str());
        if (auto ic = img->import_calls.find(a); ic != img->import_calls.end())
            note = "-> " + img->imports[ic->second].rpl + "." + img->imports[ic->second].name;
        else if (const recomp::Import* im = img->import_at(a))
            note = "(import " + std::string(im->data ? "data " : "") + im->rpl + "." + im->name + ")";
        else if (in.op == ppc::Op::b) {
            uint32_t t = in.aa ? static_cast<uint32_t>(in.imm) : a + static_cast<uint32_t>(in.imm);
            if (auto nt = names.find(t); nt != names.end()) note = "-> " + nt->second;
        }
        std::printf("  %08X  %08X  %-36s %s\n", a, *w, text.c_str(), note.c_str());
    }
    return 0;
}
