// The loading screen shown while the game's shaders are translated (first start, or after an update of Cha-Cha) and
// while the pipelines earlier runs used are built again, before the game boots: "Compiling shaders...", a progress
// bar and the percentage in a brown box, on black. Drawn on the CPU and presented as an image (no UI library).
#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace mh3u::gfx {
class Presenter;
}

namespace mh3u::boot {

class LoadingScreen {
public:
    explicit LoadingScreen(gfx::Presenter& presenter);
    // Host main thread, every loop iteration: presents a frame at most ~60 times a second. progress 0..1, or < 0
    // when unknown (an empty bar, no percentage).
    void frame(float progress);

private:
    gfx::Presenter& presenter_;
    std::vector<uint32_t> pixels_;
    std::chrono::steady_clock::time_point start_, last_{};
};

}  // namespace mh3u::boot
