// Hold to gather (carving.cpp): always on.
#pragma once

#include <functional>

namespace mh3u::game {

// on_obtain: called (guest thread) each time the player obtains an item by carving, gathering or mining.
void register_carving(std::function<void()> on_obtain);

}  // namespace mh3u::game
