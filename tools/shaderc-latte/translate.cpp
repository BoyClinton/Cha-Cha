#include "translate.h"

#include "glsl.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

namespace mh3u::latte {
namespace {

namespace stdfs = std::filesystem;

const char* stage_ext(Stage s) { return s == Stage::Vertex ? "vert" : s == Stage::Pixel ? "frag" : "geom"; }

// Every GFD inside a file (loose .es files hold one, the pack and the .lfx many).
void scan(const std::vector<uint8_t>& buf, const std::string& label, std::map<uint64_t, Found>& out, int& gfds,
          std::vector<std::string>* errors) {
    for (size_t s = 0; s + 0x20 <= buf.size(); ++s) {
        if (std::memcmp(&buf[s], "Gfx2", 4) != 0 || buf[s + 4] || buf[s + 5] || buf[s + 6] || buf[s + 7] != 0x20) continue;
        ++gfds;
        std::vector<Shader> shaders;
        std::string error;
        if (!parse_gfd(&buf[s], buf.size() - s, shaders, &error)) {
            if (errors) errors->push_back(label + "+0x" + std::to_string(s) + ": " + error);
            continue;
        }
        for (Shader& sh : shaders) {
            const uint64_t key = sh.key();
            if (out.count(key)) continue;
            Found f;
            f.source = label;
            f.shader = std::move(sh);
            out.emplace(key, std::move(f));
        }
    }
}

bool write_file(const stdfs::path& p, const void* data, size_t size) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

}  // namespace

std::map<uint64_t, Found> find_shaders(const ContentReader& content, int* gfds, std::vector<std::string>* errors) {
    std::map<uint64_t, Found> out;
    int n = 0;
    for (const char* sub : {"nativeCafe/es", "nativeCafe/system/ltshader"})
        for (const std::string& name : content.list_files(sub))
            if (auto buf = content.read(std::string(sub) + "/" + name)) scan(*buf, std::string(sub + 11) + "/" + name, out, n, errors);
    for (const char* file : {"nativeCafe/shaderPack.spkg", "nativeCafe/LtUserShader/LtUserShaderPackage.lfx"})
        if (auto buf = content.read(file)) scan(*buf, file + 11, out, n, errors);
    if (gfds) *gfds = n;
    return out;
}

std::vector<uint32_t> compile_glsl(const std::string& source, Stage stage, const std::string& name, bool optimize,
                                   std::string* error) {
    thread_local struct Compiler {
        shaderc_compiler_t c = shaderc_compiler_initialize();
        shaderc_compile_options_t opt = make(shaderc_optimization_level_performance), plain = make(shaderc_optimization_level_zero);
        static shaderc_compile_options_t make(shaderc_optimization_level level) {
            shaderc_compile_options_t opts = shaderc_compile_options_initialize();
            // Vulkan 1.2 (SPIR-V 1.5): older GPUs' drivers stop at 1.2; demote comes from SPV_EXT_demote_to_helper_invocation
            shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
            shaderc_compile_options_set_optimization_level(opts, level);
            return opts;
        }
        ~Compiler() {
            shaderc_compile_options_release(opt);
            shaderc_compile_options_release(plain);
            shaderc_compiler_release(c);
        }
    } comp;
    const shaderc_shader_kind kind = stage == Stage::Vertex  ? shaderc_vertex_shader
                                     : stage == Stage::Pixel ? shaderc_fragment_shader
                                                             : shaderc_geometry_shader;
    shaderc_compilation_result_t r = shaderc_compile_into_spv(comp.c, source.data(), source.size(), kind, name.c_str(), "main", optimize ? comp.opt : comp.plain);
    std::vector<uint32_t> words;
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
        const size_t bytes = shaderc_result_get_length(r);
        words.resize(bytes / 4);
        std::memcpy(words.data(), shaderc_result_get_bytes(r), words.size() * 4);
    } else if (error) {
        *error = shaderc_result_get_error_message(r);
    }
    shaderc_result_release(r);
    return words;
}

TranslateResult translate_all(const ContentReader& content, const stdfs::path& out, const std::string& stamp,
                              Progress* progress, bool keep_glsl, bool optimize) {
    TranslateResult res;
    std::error_code ec;
    stdfs::create_directories(out, ec);
    stdfs::remove(out / "version.txt", ec);
    int gfds = 0;
    std::map<uint64_t, Found> shaders = find_shaders(content, &gfds, &res.errors);
    res.shaders = static_cast<int>(shaders.size());
    if (progress) progress->total = res.shaders;

    // One varying location per semantic id, over the whole game, so any VS links with any PS.
    std::set<uint32_t> semantics;
    for (const auto& [key, e] : shaders) {
        for (uint32_t s : vs_param_semantics(e.shader)) semantics.insert(s);
        for (uint32_t s : ps_input_semantics(e.shader)) semantics.insert(s);
    }
    semantics.erase(0xFF);
    VaryingTable varyings;
    std::string table = "# semantic location\n";
    for (uint32_t s : semantics) {
        const uint32_t loc = static_cast<uint32_t>(varyings.size());
        varyings[s] = loc;
        table += std::to_string(s) + ' ' + std::to_string(loc) + '\n';
    }
    write_file(out / "varyings.txt", table.data(), table.size());

    std::vector<std::pair<const uint64_t, Found>*> work;
    for (auto& kv : shaders) work.push_back(&kv);
    std::atomic<size_t> next{0};
    std::atomic<int> translated{0}, compiled{0};
    std::mutex m;
    const auto job = [&] {
        for (size_t i; (i = next++) < work.size();) {
            auto& [key, e] = *work[i];
            char name[64];
            std::snprintf(name, sizeof name, "%016llx.%s", static_cast<unsigned long long>(key), stage_ext(e.shader.stage));
            std::string why;
            e.program = decode(e.shader.program.data(), e.shader.program.size());
            if (!e.program.ok()) {
                why = "decode: " + e.program.errors[0];
            } else if (const GlslResult g = emit_glsl(e.shader, e.program, varyings); !g.errors.empty()) {
                why = "glsl: " + g.errors[0];
            } else {
                ++translated;
                if (keep_glsl) write_file(out / name, g.source.data(), g.source.size());
                std::string err;
                const std::vector<uint32_t> spv = compile_glsl(g.source, e.shader.stage, name, optimize, &err);
                if (spv.empty()) {
                    why = "compile: " + err;
                } else if (!write_file(out / (std::string(name) + ".spv"), spv.data(), spv.size() * 4) ||
                           !write_file(out / (std::string(name) + ".meta"), g.meta.data(), g.meta.size())) {
                    why = "cannot write to " + out.string();
                } else {
                    ++compiled;
                }
            }
            if (!why.empty()) {
                std::lock_guard g(m);
                res.errors.push_back(std::string(name) + " (" + e.source + "): " + why);
            }
            if (progress) ++progress->done;
        }
    };
    std::vector<std::thread> pool;
    const unsigned n = std::max(1u, std::thread::hardware_concurrency());
    for (unsigned t = 0; t < n; ++t) pool.emplace_back(job);
    for (std::thread& t : pool) t.join();
    res.translated = translated;
    res.compiled = compiled;
    // the stamp goes last: only a complete cache counts as current
    if (res.ok()) write_file(out / "version.txt", stamp.data(), stamp.size());
    return res;
}

std::string cache_stamp(const std::string& rpx_sha1) {
    return "translator " + std::to_string(kTranslatorVersion) + " game " + rpx_sha1 + "\n";
}

bool cache_is_current(const stdfs::path& out, const std::string& stamp) {
    std::ifstream f(out / "version.txt", std::ios::binary);
    const std::string have((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return have == stamp;
}

}  // namespace mh3u::latte
