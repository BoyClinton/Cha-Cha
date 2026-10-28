// Bloom and depth-of-field switches (filters.cpp).
#pragma once

namespace mh3u::game {

// Registers the filter draw replacements; a disabled effect is not drawn.
void register_filters(bool bloom, bool depth_of_field);
// Switches them while the game runs (any thread; from the next draw).
void set_filters(bool bloom, bool depth_of_field);

}  // namespace mh3u::game
