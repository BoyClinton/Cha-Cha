// Whole-game shader translation: every GX2 shader in the user's dump -> GLSL -> SPIR-V, in-process (shaderc from
// the Vulkan SDK, linked statically), into a cache folder the runtime loads from (runtime/gfx/renderer.cpp).
// Used by the runtime's first-start setup and by the shaderc-latte tool. The output is game-derived: it lives in the
// user's data folder (or build/ for the tool), never in git or a release.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "gfd.h"
#include "latte.h"

namespace mh3u::latte {

// Bump when translated output changes (translator, shader_abi.h or compile options): caches made by another
// version are rebuilt on the next start.
inline constexpr int kTranslatorVersion = 2;  // 2: SPIR-V 1.5 (Vulkan 1.2) for older GPUs

// Game content access, relative to /vol/content ("nativeCafe/shaderPack.spkg").
struct ContentReader {
    std::function<std::optional<std::vector<uint8_t>>(const std::string& path)> read;
    std::function<std::vector<std::string>(const std::string& dir)> list_files;  // file names, sorted
};

struct Found {
    Shader shader;
    std::string source;  // first file it was found in
    Program program;
};

// Every GFD in the files that hold shaders (es/, system/ltshader/, shaderPack.spkg, LtUserShaderPackage.lfx),
// de-duplicated by runtime lookup key. gfds/failures count GFDs seen and GFDs that did not parse.
std::map<uint64_t, Found> find_shaders(const ContentReader& content, int* gfds = nullptr, std::vector<std::string>* errors = nullptr);

struct Progress {
    std::atomic<int> done{0}, total{0};
};

struct TranslateResult {
    int shaders = 0, translated = 0, compiled = 0;
    std::vector<std::string> errors;  // per-shader problems (decode, GLSL, compile)
    bool ok() const { return shaders > 0 && compiled == shaders; }
};

// Decodes, emits GLSL and compiles every shader to <out>/<key>.<stage>.spv + .meta, plus varyings.txt, then writes
// <out>/version.txt (stamp) last, so an interrupted run is redone. keep_glsl leaves the .vert/.frag/.geom sources.
TranslateResult translate_all(const ContentReader& content, const std::filesystem::path& out, const std::string& stamp,
                              Progress* progress = nullptr, bool keep_glsl = false, bool optimize = true);

// The stamp a cache made for this game build must carry (translator version + the game's RPX hash).
std::string cache_stamp(const std::string& rpx_sha1);
// True if <out>/version.txt equals stamp.
bool cache_is_current(const std::filesystem::path& out, const std::string& stamp);

// GLSL -> SPIR-V words with shaderc; empty on failure (message in *error).
std::vector<uint32_t> compile_glsl(const std::string& source, Stage stage, const std::string& name, bool optimize,
                                   std::string* error);

}  // namespace mh3u::latte
