#pragma once
#include <cstdint>

namespace mh3u::game {
// MH3U_NET_BACKEND=1: Cha-Cha's own answers in place of the Snow network backend (docs/online.md). Off when not set.
// Call before register_net_probe, so the probe logs the replacements.
void register_net_backend(uint8_t* base);
}  // namespace mh3u::game
