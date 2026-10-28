// shaderc-latte: offline translator for the game's closed set of GX2 shaders (docs/formats/shaders.md).
//
//   shaderc-latte <dump> [--out DIR] [--disasm] [--stats] [--spv [--glsl] [--opt]]
//
// <dump> is the user's .wua or game folders (runtime/fs/game_image.h). Finds every GFD, de-duplicates by runtime
// lookup key (Shader::key) and decodes the microcode.
//   --disasm  DIR/<key>.<stage>.txt listings
//   --stats   opcode statistics
//   --spv     translate and compile everything, as the runtime's first start does (translate.h) -> DIR
//   --glsl    with --spv: keep the GLSL sources next to the SPIR-V
//   --opt     with --spv: run spirv-opt as well (minutes instead of seconds; no measurable GPU gain)
// The runtime does --spv by itself on first start; this tool is for investigating the translator.
// Output is game-derived: it goes to build/, never git.
// Exit code: 0 if every shader succeeded at every requested step, 1 otherwise, 77 if the dump is missing.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "game_image.h"
#include "translate.h"

namespace stdfs = std::filesystem;
using namespace mh3u::latte;

namespace {

const char* stage_ext(Stage s) { return s == Stage::Vertex ? "vert" : s == Stage::Pixel ? "frag" : "geom"; }

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: shaderc-latte <dump> [--out DIR] [--disasm] [--stats] [--glsl] [--spv]\n");
        return 2;
    }
    stdfs::path out_dir = "build/shaders";
    bool want_disasm = false, want_stats = false, want_glsl = false, want_spv = false, optimize = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--disasm")) want_disasm = true;
        else if (!std::strcmp(argv[i], "--stats")) want_stats = true;
        else if (!std::strcmp(argv[i], "--glsl")) want_glsl = true;
        else if (!std::strcmp(argv[i], "--spv")) want_spv = true;
        else if (!std::strcmp(argv[i], "--opt")) optimize = true;
    }
    std::error_code ec;
    if (!stdfs::exists(argv[1], ec)) {
        std::printf("dump not found at %s; skipping\n", argv[1]);
        return 77;
    }
    std::string err;
    auto game = mh3u::fs::open_game({argv[1]}, &err);
    if (!game) {
        std::printf("%s\n", err.c_str());
        return 1;
    }
    const mh3u::fs::OverlayFs vfs = game->make_fs({});
    ContentReader content;
    content.read = [&](const std::string& p) -> std::optional<std::vector<uint8_t>> {
        auto f = vfs.open("/vol/content/" + p);
        if (!f) return std::nullopt;
        std::vector<uint8_t> d(static_cast<size_t>(f->size()));
        if (f->read(0, d.data(), d.size()) != d.size()) return std::nullopt;
        return d;
    };
    content.list_files = [&](const std::string& dir) {
        std::vector<std::string> names;
        for (const auto& e : vfs.list("/vol/content/" + dir))
            if (!e.is_dir) names.push_back(e.name);
        return names;  // OverlayFs lists sorted by name
    };

    if (want_spv) {
        Progress progress;
        const auto t0 = std::chrono::steady_clock::now();
        const TranslateResult r = translate_all(content, out_dir, cache_stamp(game->rpx_sha1), &progress, want_glsl, optimize);
        for (const auto& e : r.errors) std::printf("%s\n", e.c_str());
        std::printf("%d unique shaders: %d translated, %d compiled to SPIR-V in %.1f s -> %s\n", r.shaders, r.translated,
                    r.compiled, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(),
                    out_dir.string().c_str());
        return r.ok() ? 0 : 1;
    }

    int gfds = 0, failures = 0;
    std::vector<std::string> errors;
    std::map<uint64_t, Found> shaders = find_shaders(content, &gfds, &errors);
    for (const auto& e : errors) std::printf("%s\n", e.c_str());
    failures += static_cast<int>(errors.size());
    std::map<std::string, int> cf_ops, alu_ops, tex_ops, misc;
    int by_stage[3] = {}, decoded = 0;
    if (want_disasm) stdfs::create_directories(out_dir);
    for (auto& [key, e] : shaders) {
        ++by_stage[static_cast<int>(e.shader.stage)];
        e.program = decode(e.shader.program.data(), e.shader.program.size());
        if (e.program.ok()) ++decoded;
        else {
            ++failures;
            std::printf("%016llx (%s): %s\n", static_cast<unsigned long long>(key), e.source.c_str(), e.program.errors[0].c_str());
        }
        if (want_disasm) {
            char name[64];
            std::snprintf(name, sizeof name, "%016llx.%s.txt", static_cast<unsigned long long>(key), stage_ext(e.shader.stage));
            std::ofstream f(out_dir / name);
            f << "; " << e.source << "  mode " << e.shader.mode << "\n";
            f << disassemble(e.program);
            if (!e.shader.copy_program.empty()) f << "; copy shader\n" << disassemble(decode(e.shader.copy_program.data(), e.shader.copy_program.size()));
        }
        if (want_stats) {
            const char* st = stage_ext(e.shader.stage);
            misc[std::string("mode ") + std::to_string(e.shader.mode) + " " + st]++;
            for (const Cf& cf : e.program.cf) {
                std::string n = cf.name() ? cf.name() : "?";
                if (cf.kind == CfKind::Export) n += std::string(" type") + std::to_string(cf.export_type) + " base" + std::to_string(cf.array_base);
                if (cf.pop_count) n += " pop" + std::to_string(cf.pop_count);
                cf_ops[n]++;
                for (const AluGroup& g : cf.alu)
                    for (const AluInst& a : g.insts) {
                        alu_ops[a.name() ? a.name() : "?"]++;
                        for (int i = 0; i < a.num_src(); ++i) {
                            if (a.src[i].rel) misc["src rel"]++;
                            if (a.src[i].sel >= SEL_CFILE) misc["src cfile"]++;
                        }
                        if (a.dst_rel) misc["dst rel"]++;
                        if (a.pred_sel) misc["pred_sel"]++;
                        if (a.update_exec_mask) misc["update_exec_mask"]++;
                        if (a.update_pred) misc["update_pred"]++;
                        if (a.omod) misc["omod"]++;
                    }
                for (const TexFetch& t : cf.tex) {
                    tex_ops[t.name()]++;
                    if (t.offset[0] || t.offset[1] || t.offset[2]) misc["tex offset"]++;
                    if (t.lod_bias) misc["tex lod_bias"]++;
                    if (t.resource_id != t.sampler_id) misc["tex resource!=sampler"]++;
                    if (!t.coord_normalized[0]) misc["tex unnormalized"]++;
                }
                for (const VtxFetch& v : cf.vtx) misc["vtx buffer " + std::to_string(v.buffer_id) + " " + st]++;
            }
            for (const SamplerVar& s : e.shader.samplers) misc["sampler type " + std::to_string(s.type) + " " + s.name]++;
        }
    }
    if (want_stats) {
        const auto dump = [](const char* title, const std::map<std::string, int>& m) {
            std::printf("-- %s\n", title);
            for (const auto& [k, v] : m) std::printf("  %-40s %d\n", k.c_str(), v);
        };
        dump("CF", cf_ops);
        dump("ALU", alu_ops);
        dump("TEX", tex_ops);
        dump("misc", misc);
    }
    std::printf("%d GFDs, %zu unique shaders (%d vs, %d ps, %d gs); decoded %d/%zu\n", gfds, shaders.size(), by_stage[0],
                by_stage[1], by_stage[2], decoded, shaders.size());
    return failures ? 1 : 0;
}
