// Shader translation on a worker thread (shader_prep.h).
#include "shader_prep.h"

#include "overlay_fs.h"

#include <cstdio>
#include <optional>
#include <vector>

namespace mh3u::boot {
namespace {

// Unoptimised SPIR-V: the driver optimises when it builds pipelines anyway, and spirv-opt over the translated
// shaders takes minutes (360 s vs 4 s for all 668 on a 12-thread CPU).
constexpr bool kOptimizeSpirv = false;

latte::ContentReader content_reader(const fs::OverlayFs& vfs) {
    latte::ContentReader c;
    c.read = [&vfs](const std::string& p) -> std::optional<std::vector<uint8_t>> {
        auto f = vfs.open("/vol/content/" + p);
        if (!f) return std::nullopt;
        std::vector<uint8_t> d(static_cast<size_t>(f->size()));
        if (f->read(0, d.data(), d.size()) != d.size()) return std::nullopt;
        return d;
    };
    c.list_files = [&vfs](const std::string& dir) {
        std::vector<std::string> names;
        for (const auto& e : vfs.list("/vol/content/" + dir))
            if (!e.is_dir) names.push_back(e.name);
        return names;
    };
    return c;
}

}  // namespace

ShaderPrep::ShaderPrep(const fs::GameImage& game, const std::filesystem::path& dir) : start_(std::chrono::steady_clock::now()) {
    const std::string stamp = latte::cache_stamp(game.rpx_sha1);
    if (latte::cache_is_current(dir, stamp)) return;
    started_ = true;
    game_ = game;
    std::printf("translating the game's shaders into %s\n", dir.string().c_str());
    thread_ = std::thread([this, dir, stamp] {
        const fs::OverlayFs vfs = game_.make_fs({});
        result_ = latte::translate_all(content_reader(vfs), dir, stamp, &progress_, false, kOptimizeSpirv);
        for (const auto& e : result_.errors) std::fprintf(stderr, "  %s\n", e.c_str());
        std::printf("shaders: %d/%d ready in %.1f s\n", result_.compiled, result_.shaders, seconds());
        finished_ = true;
    });
}

ShaderPrep::~ShaderPrep() {
    if (thread_.joinable()) thread_.join();
}

float ShaderPrep::progress() const {
    if (finished()) return 1.0f;
    const int total = progress_.total;
    return total ? static_cast<float>(progress_.done) / static_cast<float>(total) : 0.0f;
}

bool ShaderPrep::ok() const { return !started_ || result_.compiled > 0; }

void ShaderPrep::wait_with_console() {
    for (int last = -1; !finished();) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const int done = progress_.done, total = progress_.total;
        if (total && done / 100 != last) {
            last = done / 100;
            std::printf("  shaders %d/%d\n", done, total);
        }
    }
    if (thread_.joinable()) thread_.join();
}

}  // namespace mh3u::boot
