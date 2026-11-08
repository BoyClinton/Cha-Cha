#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mh3u::game {
// MH3U_NET_PROBE=1 logs the network backend's virtual calls and the system network imports; =online also reports a
// connected, signed-in console. Does nothing when the variable is not set. fp_imports = the nn_fp import names.
void register_net_probe(uint8_t* base, const std::vector<std::string>& fp_imports);
// Call after the GX2 imports are registered: MH3U_NET_PROBE_AREA, a debug area change at a given frame.
void register_net_probe_jump();
}  // namespace mh3u::game
