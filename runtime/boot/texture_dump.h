// The whole game's textures written out for texture packs (texture_dump.cpp): every rTexture in every archive of the
// content tree (update over base), not only what the game draws, under the names packs use.
#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <thread>

namespace mh3u::fs {
class OverlayFs;
}

namespace mh3u::boot {

class TextureDump {
public:
    ~TextureDump();
    // Starts on a worker thread (no-op while one runs). vfs must outlive the dump.
    void start(const fs::OverlayFs& vfs, std::filesystem::path out_dir);
    bool running() const { return running_.load(); }
    // Stops a running dump soon (between textures); what was written stays.
    void cancel() { cancel_ = true; }
    bool cancelling() const { return cancel_.load(); }
    // Textures looked at / all textures in the archives (0 until counted) / files written (the rest existed already
    // or were the same texture again, or cube maps, which packs cannot replace).
    size_t done() const { return done_.load(); }
    size_t total() const { return total_.load(); }
    size_t written() const { return written_.load(); }
    const std::filesystem::path& folder() const { return out_; }
    // Runs it on this thread (headless checks); returns the files written.
    size_t run_now(const fs::OverlayFs& vfs, std::filesystem::path out_dir);

private:
    void run(const fs::OverlayFs& vfs);
    std::filesystem::path out_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<size_t> done_{0}, total_{0}, written_{0};
    std::atomic<bool> finished_{false};
    std::atomic<bool> cancel_{false};  // a dump has run to the end this session
    friend std::string texture_dump_status(const TextureDump&);
};

// For the game's help box: how a running dump goes, or what the last one wrote; "" before any. Lines split by \r\n.
std::string texture_dump_status(const TextureDump& dump);

}  // namespace mh3u::boot
