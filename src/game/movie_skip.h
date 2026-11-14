// Skippable cutscenes: + skips the pre-rendered cutscene movie playing (movie_skip.cpp).
#pragma once

namespace mh3u::game {

// take_skip: true once per new press of the skip button (the host's input)
void register_movie_skip(bool (*take_skip)());

}  // namespace mh3u::game
