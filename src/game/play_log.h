// MH3U_PLAY_LOG=<file>: a per-frame log of the hunter and the above-head marker, for finding frame-rate bugs (play_log.cpp).
#pragma once

#include <cstdint>
#include <functional>

namespace mh3u::game {

// watch (optional): arms a write watch on a guest word; called every frame with the hunter's x position address.
void register_play_log(std::function<void(uint32_t)> watch);

}  // namespace mh3u::game
