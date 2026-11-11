// The game's shaders translated to SPIR-V (latte::translate_all) into <data>/cache/shaders when the cache is not
// current for this executable: on a worker thread, its progress polled by the caller (the loading screen, or the
// console for headless runs).
#pragma once

#include "game_image.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include "translate.h"

namespace mh3u::boot {

class ShaderPrep {
public:
    // Starts the translation if the cache in dir is not current for game; needed() says whether it did.
    ShaderPrep(const fs::GameImage& game, const std::filesystem::path& dir);
    ~ShaderPrep();
    ShaderPrep(const ShaderPrep&) = delete;
    ShaderPrep& operator=(const ShaderPrep&) = delete;

    bool needed() const { return thread_.joinable() || started_; }
    bool finished() const { return !started_ || finished_; }
    // 0..1 (1 once finished or not needed)
    float progress() const;
    // after finished(): whether every shader translated (true when nothing was needed)
    bool ok() const;
    const latte::TranslateResult& result() const { return result_; }
    double seconds() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count(); }
    // Blocks until done, printing progress to the console (headless runs).
    void wait_with_console();

private:
    fs::GameImage game_;
    latte::Progress progress_;
    latte::TranslateResult result_;
    std::atomic<bool> finished_{false};
    bool started_ = false;
    std::thread thread_;
    std::chrono::steady_clock::time_point start_;
};

}  // namespace mh3u::boot
