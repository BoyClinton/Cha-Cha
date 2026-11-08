#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mh3u::game {
// Cha-Cha's own answers in place of the Snow network backend (docs/online.md). Registered always; it does nothing until the
// game signs in or the player asks for a session, and opens no socket before that. fp_imports = the nn_fp import names.
// Call before register_net_probe, so the probe logs the replacements.
void register_net_backend(uint8_t* base, const std::vector<std::string>& fp_imports);
// Call after the GX2 imports are registered: runs the Host / Join sequence once a frame.
void register_net_backend_tick();

enum class NetRequest { Host, Join, Leave };
void net_request(NetRequest request);  // from any thread; takes effect on the next frame
std::string net_status();              // one line for the settings page
bool net_can_request(NetRequest request);
}  // namespace mh3u::game
