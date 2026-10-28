// The game's own frame limiter, skipped when Cha-Cha paces frames itself (frame_limit.cpp).
#pragma once

namespace mh3u::game {

// own_pacing: the host paces frames (any windowed run); the game's limiter then does nothing.
void register_frame_limit(bool own_pacing);

}  // namespace mh3u::game
