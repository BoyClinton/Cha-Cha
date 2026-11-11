#pragma once

namespace mh3u::game {
// MH3U_CHARM_PROBE=<frame>: rolls a talisman from every state of RNG slot 1 for every charm at that frame and reports
// the talismans per charm table (docs/research-log.md 2026-11-11). Call after the GX2 imports are registered.
void register_charm_probe();
}  // namespace mh3u::game
