// Charm table probe: a research aid for the Charm Table Fix mod (mods/charm-table-fix, docs/research-log.md
// 2026-11-11 "Charm tables"). Off unless MH3U_CHARM_PROBE=<frame> is set.
//
// At that frame (counted in GX2SwapScanBuffers calls, on the game's thread) it rolls one talisman with the game's
// generator 0x0217F63C (makeTalisman?) from every state of RNG slot 1 (sGame+0x42) for every charm 0x25-0x28, through
// the hook point, so a loaded Charm Table Fix takes part. It reports per table (the cycle of
// x -> (x || 1) * 176 mod 65363 the start state lies on) how many different talismans came out, how many draws a roll
// takes, and whether slot 1 ended on the start state's cycle. Slot 1 is put back afterwards. Status: wip (research).
#define _CRT_SECURE_NO_WARNINGS  // getenv, on the debug variable only
#include "charm_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

#include "functions.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kGamePtr = 0x101FDB9C, kSlot1 = 0x42, kModulus = 65363;
ppc::GuestFunction g_swap = nullptr;
uint32_t g_frame = 0, g_at = 0;

uint32_t step(uint32_t x) { return ((x ? x : 1) * 176) % kModulus; }

void run(ppc::Context& ctx, uint8_t* base) {
    const uint32_t game = ppc::ld32(base, kGamePtr);
    if (!game) {
        std::printf("charm probe: no sGame yet at frame %u\n", g_frame);
        return;
    }
    const uint32_t slot = game + kSlot1;
    const uint16_t saved = ppc::ld16(base, slot);
    // cycle id = the smallest state on its cycle
    std::vector<uint16_t> cycle(kModulus, 0);
    std::map<uint16_t, uint32_t> cycle_len;
    for (uint32_t s = 1; s < kModulus; ++s) {
        if (cycle[s]) continue;
        uint32_t x = s, n = 0;
        do cycle[x] = static_cast<uint16_t>(s), x = step(x), ++n;
        while (x != s);
        cycle_len[static_cast<uint16_t>(s)] = n;
    }
    const ppc::Context saved_ctx = ctx;
    ctx.r[1] -= 0x800;
    const uint32_t out = static_cast<uint32_t>(ctx.r[1]) + 0x400;  // in our own frame, above the callee's
    std::map<uint16_t, std::set<uint64_t>> per_cycle;               // cycle -> talismans (charm << 56 | bytes 1..7)
    std::set<uint64_t> all;
    std::map<int, uint32_t> draws;
    uint32_t off_cycle = 0, unknown = 0;
    for (uint32_t charm = 0x25; charm <= 0x28; ++charm)
        for (uint32_t s = 1; s < kModulus; ++s) {
            for (uint32_t i = 0; i < 8; ++i) ppc::st8(base, out + i, 0);
            ppc::st16(base, slot, static_cast<uint16_t>(s));
            ctx.r[3] = 0;
            ctx.r[4] = out;
            ctx.r[5] = charm;
            sub_0217F63C(ctx, base);
            uint64_t key = static_cast<uint64_t>(charm) << 56;
            for (uint32_t i = 1; i < 8; ++i) key |= static_cast<uint64_t>(ppc::ld8(base, out + i)) << (8 * (7 - i));
            per_cycle[cycle[s]].insert(key);
            all.insert(key);
            const uint16_t now = ppc::ld16(base, slot);
            if (cycle[now ? now : 1] != cycle[s]) ++off_cycle;
            int k = -1;
            uint32_t x = s;
            for (int n = 0; n <= 16; ++n, x = step(x))
                if (x == now) {
                    k = n;
                    break;
                }
            if (k < 0) ++unknown;
            else ++draws[k];
        }
    ppc::st16(base, slot, saved);
    const uint64_t r1 = ctx.r[1] + 0x800;
    ctx = saved_ctx;
    ctx.r[1] = r1;
    std::printf("charm probe: %zu different talismans over all states and charms\n", all.size());
    for (auto& [c, set] : per_cycle)
        std::printf("charm probe: cycle %5u (%4u states): %6zu talismans\n", c, cycle_len[c], set.size());
    std::printf("charm probe: slot 1 left its start cycle in %u rolls; start->end not a short walk in %u rolls; draws:",
                off_cycle, unknown);
    for (auto& [k, n] : draws) std::printf(" %d:%u", k, n);
    std::printf("\n");
    // MH3U_CHARM_PROBE_DUMP=<file>: every talisman per cycle, "cycle charm slots rarity id1 p1 id2 p2"
    if (const char* dump = std::getenv("MH3U_CHARM_PROBE_DUMP"); dump && *dump)
        if (FILE* f = std::fopen(dump, "w")) {
            for (auto& [c, set] : per_cycle)
                for (uint64_t k : set)
                    std::fprintf(f, "%u %u %u %u %u %d %u %d\n", c, static_cast<unsigned>(k >> 56),
                                 static_cast<unsigned>((k >> 48) & 0xFF), static_cast<unsigned>((k >> 32) & 0xFFFF),
                                 static_cast<unsigned>((k >> 24) & 0xFF), static_cast<int8_t>((k >> 16) & 0xFF),
                                 static_cast<unsigned>((k >> 8) & 0xFF), static_cast<int8_t>(k & 0xFF));
            std::fclose(f);
            std::printf("charm probe: wrote %s\n", dump);
        }
    std::fflush(stdout);
}

PPC_FUNC(swap_with_probe) {
    if (++g_frame == g_at) run(ctx, base);
    g_swap(ctx, base);
}

}  // namespace

void register_charm_probe() {
    const char* v = std::getenv("MH3U_CHARM_PROBE");
    if (!v || !*v) return;
    g_at = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    g_swap = ppc::resolve_import("gx2.GX2SwapScanBuffers");
    if (g_swap && g_at) ppc::register_import("gx2.GX2SwapScanBuffers", &swap_with_probe);
}

}  // namespace mh3u::game
