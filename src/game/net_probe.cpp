// Network probe (docs/online.md, plan step 1). Research aid, off unless MH3U_NET_PROBE is set.
//
// Backend calls: entries 11+ and the destructor of the Uds / Nex / Snow / Nnac vtables are pointed at one host function
// that logs and calls the original (no game code is regenerated). Entries 37+ of Session and 36+ of Context are the
// shared per-frame base code and stay as they are. Addresses: config/vtables.csv, build v32.
//
// MH3U_NET_PROBE=online also answers nn_ac / nn_act / nn_fp as a connected, signed-in console. Values are invented; only
// what the game accepts matters here. Status: wip.
#define _CRT_SECURE_NO_WARNINGS  // sscanf, on the debug variables only
#include "net_probe.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string_view>

#include "functions.h"
#include "net_backend.h"

namespace mh3u::game {
namespace {

std::string env(const char* name) {  // empty when not set
    char* v = nullptr;
    size_t n = 0;
    std::string out;
    if (_dupenv_s(&v, &n, name) == 0 && v) out = v;
    std::free(v);
    return out;
}

struct Traced {
    uint32_t original;
    const char* cls;
    int slot;
    int logged = 0;
};

constexpr uint32_t kFakeBase = 0x7FF00000;  // not mapped; only ever used as a call_indirect target
constexpr int kMaxLogPerSlot = 6;
constexpr uint32_t kPrincipalId = 0x10000001;

std::map<uint32_t, Traced> g_traced;
std::mutex g_log;

PPC_FUNC(traced_slot) {
    const uint32_t target = ppc::current_indirect_target();
    Traced& t = g_traced.at(target);
    const uint32_t r3 = static_cast<uint32_t>(ctx.r[3]), r4 = static_cast<uint32_t>(ctx.r[4]),
                   r5 = static_cast<uint32_t>(ctx.r[5]), r6 = static_cast<uint32_t>(ctx.r[6]);
    bool log;
    {
        std::lock_guard g(g_log);
        log = t.logged < kMaxLogPerSlot;
        if (log) ++t.logged;
    }
    ppc::call_indirect(ctx, base, t.original);
    if (log)
        std::printf("net: %s[%d] this=%08X r4=%08X r5=%08X r6=%08X lr=%08X -> %08X\n", t.cls, t.slot, r3, r4, r5, r6,
                    static_cast<uint32_t>(ctx.lr), static_cast<uint32_t>(ctx.r[3]));
}

void trace_vtable(uint8_t* base, const char* cls, uint32_t vtable, int first, int last) {
    static uint32_t next = kFakeBase;
    auto patch = [&](int slot) {
        const uint32_t entry = vtable + slot * 8 + 4;
        const uint32_t fake = next;
        next += 4;
        g_traced[fake] = Traced{ppc::ld32(base, entry), cls, slot};
        ppc::register_indirect(fake, &traced_slot);
        ppc::st32(base, entry, fake);
    };
    patch(1);
    for (int s = first; s <= last; ++s) patch(s);
}

void (*g_previous_hook)(const char*) = nullptr;

void import_hook(const char* name) {
    if (g_previous_hook) g_previous_hook(name);
    static const char* const kLibs[] = {"nn_ac.", "nn_act.", "nn_fp.", "nsysnet.", "nlibcurl.", "nn_uds.", "nn_boss.", "nn_ndm."};
    const std::string_view n(name);
    bool hit = false;
    for (const char* lib : kLibs) hit = hit || n.rfind(lib, 0) == 0;
    if (!hit) return;
    static std::set<std::string_view> seen;
    std::lock_guard g(g_log);
    if (seen.insert(n).second) std::printf("net: import %s (first call)\n", name);
}

void reg(const char* qualified, ppc::GuestFunction fn) { ppc::register_import(qualified, fn); }
void ret_value(ppc::Context& c, uint32_t v) { c.r[3] = v; }

void write_string(uint8_t* b, uint32_t guest, const char* s) {
    if (guest) std::memcpy(b + guest, s, std::strlen(s) + 1);
}

// Result XAsync(callback(Result, void*), void* user): finishes at once, successfully.
PPC_FUNC(fp_async_ok) {
    const uint32_t cb = static_cast<uint32_t>(ctx.r[3]), user = static_cast<uint32_t>(ctx.r[4]);
    if (cb) {
        const uint64_t lr = ctx.lr;
        ctx.r[3] = 0;
        ctx.r[4] = user;
        ppc::call_indirect(ctx, base, cb);
        ctx.lr = lr;
    }
    ctx.r[3] = 0;
}

// MH3U_NET_PROBE_AREA="frame:dti_hex;frame:dti_hex": at that swap, the game's own area change, (*0x103D2EF4)->slot 11(DTI of the area),
// as aGalleryMenu::vf7 does it (0x02018EB0). Saves walking there in headless runs.
ppc::GuestFunction g_swap = nullptr;
struct AreaJump {
    uint32_t frame, dti;
};
std::vector<AreaJump> g_jumps;
uint32_t g_swaps = 0;
constexpr uint32_t kAreaSystem = 0x103D2EF4;

// sLobbyNetwork (*0x103A2F90): +0x38 state, +0x44 request bits (the online screen sets them: 0x2 at 026AE5E4, then 0x4 or 0x8).
// MH3U_NET_PROBE_FLAGS="frame:bits;frame:bits" sets bits as the screen would ("frame:i:bits": the Internet machine's
// requests instead); state changes are always logged.
constexpr uint32_t kLobbyNetwork = 0x103A2F90;
struct FlagPoke {
    uint32_t frame, bits;
    bool internet;
    int join_index = -1;  // "frame:j:index": the result to join (+0x8C4), then request bit 0x20
};
std::vector<FlagPoke> g_pokes;

void lobby_network_tick(uint8_t* base) {
    const uint32_t net = ppc::ld32(base, kLobbyNetwork);
    if (!net) return;
    static uint32_t last_state = 0xFFFFFFFF, last_flags = 0xFFFFFFFF;
    const uint32_t state = ppc::ld32(base, net + 0x38), flags = ppc::ld32(base, net + 0x44);
    if (state != last_state || flags != last_flags) {
        std::printf("net: swap %u sLobbyNetwork state %u flags %02X (+0x48 %08X +0x50 %08X)\n", g_swaps, state, flags,
                    ppc::ld32(base, net + 0x48), ppc::ld32(base, net + 0x50));
        last_state = state;
        last_flags = flags;
    }
    // second machine, the Internet side (02266E88, opens the manager in mode 4): +0x8AC state, +0x8B0 requests
    static uint32_t last_inet = 0xFFFFFFFF, last_inet_req = 0xFFFFFFFF;
    const uint32_t inet = ppc::ld32(base, net + 0x8AC), inet_req = ppc::ld32(base, net + 0x8B0);
    if (inet != last_inet || inet_req != last_inet_req) {
        std::printf("net: swap %u internet state %u requests %02X\n", g_swaps, inet, inet_req);
        last_inet = inet;
        last_inet_req = inet_req;
    }
    for (const FlagPoke& p : g_pokes)
        if (p.frame == g_swaps) {
            if (p.join_index >= 0) ppc::st32(base, net + 0x8C4, static_cast<uint32_t>(p.join_index));
            const uint32_t field = p.internet ? 0x8B0 : 0x44;
            ppc::st32(base, net + field, ppc::ld32(base, net + field) | p.bits);
            std::printf("net: swap %u set request bits %02X at +%X\n", g_swaps, p.bits, field);
        }
}

// Who is in the world: sPlayer (*0x10314B80) +0x40, 8 unit pointers (docs/online.md); +0xE30 its pad block, whose +0xE0 is the position (src/game/play_log.cpp).
constexpr uint32_t kPlayers = 0x10314B80;
constexpr uint32_t kQuestAreaVtable = 0x100027B8;

// uIDLobbyOnline::vf63 (entry 64), Neko's ferry menu: remembers the screen, to log its state
uint32_t g_online_screen = 0;
PPC_FUNC(online_screen_step) {
    g_online_screen = static_cast<uint32_t>(ctx.r[3]);
    sub_026B25C8(ctx, base);
}

// aQuestArea::vf5 (entry 6): remembers the area, to log how far its loading has got
uint32_t g_quest_area = 0;
PPC_FUNC(quest_area_step) {
    g_quest_area = static_cast<uint32_t>(ctx.r[3]);
    sub_02021F90(ctx, base);
}

void players_tick(uint8_t* base) {
    if (g_swaps % 300 != 0) return;
    const uint32_t players = ppc::ld32(base, kPlayers), net = ppc::ld32(base, kLobbyNetwork);
    if (!players || !net) return;
    std::printf("net: swap %u world: lobby +34=%u +8B4=%u +964=%08X", g_swaps, ppc::ld8(base, net + 0x34), ppc::ld8(base, net + 0x8B4),
                ppc::ld32(base, net + 0x964));
    for (uint32_t m = 0; m < 4; ++m) std::printf(" m%u=%u", m, ppc::ld8(base, net + 0x70 + m * 0x148));
    // the hub's send and receive gates (0x0279A388, 0x022648C4): sGame +0x48, +0x49, +0x4B my member, +0x61;
    // *0x10315C80 +0x115 / +0x116 area ids; lobby +0x72 per member
    const uint32_t game = ppc::ld32(base, 0x101FDB9C), areas = ppc::ld32(base, 0x10315C80);
    std::printf(" sGame+48=%u +49=%u +4B=%u +61=%u area=%X/%X lobby+72:%u,%u\n", ppc::ld8(base, game + 0x48), ppc::ld8(base, game + 0x49),
                ppc::ld8(base, game + 0x4B), ppc::ld8(base, game + 0x61), areas ? ppc::ld8(base, areas + 0x115) : 0,
                areas ? ppc::ld8(base, areas + 0x116) : 0, ppc::ld8(base, net + 0x72), ppc::ld8(base, net + 0x72 + 0x148));
    // MH3U_NET_PROBE_HUB="frame[:area:sub]": what aHarbor::vf5 stores (0x0201ACFC), which a debug area jump skips
    static const std::string hub = env("MH3U_NET_PROBE_HUB");
    uint32_t hub_at = 0, hub_area = 0, hub_sub = 0;
    const int hub_fields = hub.empty() ? 0 : std::sscanf(hub.c_str(), "%u:%x:%x", &hub_at, &hub_area, &hub_sub);
    if (hub_fields >= 1 && g_swaps >= hub_at) {
        ppc::st8(base, game + 0x61, 1);
        ppc::st8(base, game + 0x48, 2);
        if (hub_fields == 3 && areas) {
            ppc::st8(base, areas + 0x115, static_cast<uint8_t>(hub_area));
            ppc::st8(base, areas + 0x116, static_cast<uint8_t>(hub_sub));
        }
    }
    {
        const uint32_t proc = ppc::ld32(base, 0x103A2DB0), lobby = ppc::ld32(base, 0x103147A0);
        std::printf("net:   quest %u, sGame+4A=%d proc +C3B=%u +C3C=%u table %u/%u %u/%u arrived %02X%02X sync %02X%02X\n", ppc::ld16(base, lobby + 0x522C),
                    static_cast<int8_t>(ppc::ld8(base, game + 0x4A)), ppc::ld8(base, proc + 0xC3B), ppc::ld8(base, proc + 0xC3C), ppc::ld8(base, proc + 0xC40),
                    ppc::ld8(base, proc + 0xC41), ppc::ld8(base, proc + 0xC40 + 0xD0), ppc::ld8(base, proc + 0xC41 + 0xD0), ppc::ld8(base, proc + 0xF84),
                    ppc::ld8(base, proc + 0xF85), ppc::ld8(base, proc + 0xF8C), ppc::ld8(base, proc + 0xF8D));
    }
    // monsters: sEnemy (*0x10313260) +0x34; unit +0xE33 kind, +0x40 position (as the damage numbers mod reads it); +0x1BE0 state: +0x8B8 HP, +0x8BC max
    if (const uint32_t enemies = ppc::ld32(base, 0x10313260))
        std::printf("net:   sEnemy host flag +45A=%u +45B=%u\n", ppc::ld8(base, enemies + 0x45A), ppc::ld8(base, enemies + 0x45B));
    if (const uint32_t enemies = ppc::ld32(base, 0x10313260))
        for (uint32_t i = 0; i < 8; ++i) {
            const uint32_t monster = ppc::ld32(base, enemies + 0x34 + i * 4);
            if (!monster) continue;
            const uint32_t state = ppc::ld32(base, monster + 0x1BE0);
            // state +0x9C bit 8: shared between members; +0xFF this copy controls it; +0x101 the controlling member (0x025A3A1C)
            std::printf("net:   monster %u zone %u part0 %d id %04X shared=%u mine=%u owner=%d kind %u pos %.1f %.1f %.1f hp %d/%d\n", i, state ? ppc::ld8(base, state + 0xEB) : 255,
                        state ? static_cast<int32_t>(ppc::ld32(base, state + 0x8EC)) : 0, state ? ppc::ld16(base, state + 0x1C) : 0xFFFF,
                        state ? (ppc::ld32(base, state + 0x9C) >> 3) & 1 : 9, state ? ppc::ld8(base, state + 0xFF) : 9,
                        state ? static_cast<int8_t>(ppc::ld8(base, state + 0x101)) : -9,
                        ppc::ld8(base, monster + 0xE33), ppc::ldf32(base, monster + 0x40),
                        ppc::ldf32(base, monster + 0x44), ppc::ldf32(base, monster + 0x48),
                        state ? static_cast<int32_t>(ppc::ld32(base, state + 0x8B8)) : -1, state ? static_cast<int32_t>(ppc::ld32(base, state + 0x8BC)) : -1);
        }
    if (g_quest_area)
        std::printf("net:   quest area %08X +5=%u +6=%u +48=%d; sGame +158=%u +1B7=%u +3BC=%d\n", g_quest_area, ppc::ld8(base, g_quest_area + 5),
                    ppc::ld8(base, g_quest_area + 6), static_cast<int8_t>(ppc::ld8(base, g_quest_area + 0x48)), ppc::ld8(base, game + 0x158),
                    ppc::ld8(base, game + 0x1B7), static_cast<int32_t>(ppc::ld32(base, game + 0x3BC)));
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t unit = ppc::ld32(base, players + 0x40 + i * 4);
        if (!unit) continue;
        const uint32_t pad = ppc::ld32(base, unit + 0xE30);
        if (!pad) continue;
        std::printf("net:   unit %u at %08X flags %08X pad+3C=%u pos %.1f %.1f %.1f\n", i, unit, ppc::ld32(base, unit + 0xC),
                    ppc::ld32(base, pad + 0x3C), ppc::ldf32(base, pad + 0xE0), ppc::ldf32(base, pad + 0xE4),
                    ppc::ldf32(base, pad + 0xE8));
    }
}

PPC_FUNC(swap_with_jump) {
    lobby_network_tick(base);
    players_tick(base);
    ++g_swaps;
    // MH3U_NET_PROBE_SESSION="frame:host|join|leave;...": presses the settings page's buttons
    static const std::string session = env("MH3U_NET_PROBE_SESSION");
    for (const char* q = session.c_str(); q && *q;) {
        uint32_t at = 0;
        char what[8] = {};
        if (std::sscanf(q, "%u:%7[a-z]", &at, what) == 2 && at == g_swaps) {
            const std::string_view w(what);
            net_request(w == "host" ? NetRequest::Host : w == "join" ? NetRequest::Join : NetRequest::Leave);
        }
        q = std::strchr(q, ';');
        if (q) ++q;
    }
    // MH3U_NET_PROBE_POST="frame:number": post a quest as the quest counter does: 0x021E355C(sLobby, my pad block, number, 1), as
    // 0x021E44D4 calls it. Guests get it through the message dispatcher (0x022646F4 calls it with mode 2).
    static const std::string post = env("MH3U_NET_PROBE_POST");
    uint32_t post_at = 0, post_number = 0;
    if (!post.empty() && std::sscanf(post.c_str(), "%u:%u", &post_at, &post_number) == 2 && post_at == g_swaps) {
        const uint32_t game = ppc::ld32(base, 0x101FDB9C), players = ppc::ld32(base, kPlayers);
        const uint32_t me = ppc::ld32(base, players + 0x40 + ppc::ld8(base, game + 0x4B) * 4);
        const ppc::Context saved = ctx;
        std::printf("net: swap %u posting quest %u\n", g_swaps, post_number);
        ctx.r[3] = ppc::ld32(base, 0x103147A0);
        ctx.r[4] = ppc::ld32(base, me + 0xE30);
        ctx.r[5] = post_number;
        ctx.r[6] = 1;
        sub_021E355C(ctx, base);
        std::printf("net: posted, result %08X\n", static_cast<uint32_t>(ctx.r[3]));
        const uint64_t r1 = ctx.r[1];
        ctx = saved;
        ctx.r[1] = r1;
    }
    // MH3U_NET_PROBE_GO=frame: what the counter's depart does on the host (0x022049DC): sLobby +0x54AE |= 8, then 0x0225111C on the party object
    static const std::string go_env = env("MH3U_NET_PROBE_GO");
    if (!go_env.empty() && std::strtoul(go_env.c_str(), nullptr, 10) == g_swaps) {
        const uint32_t lobby = ppc::ld32(base, 0x103147A0), party_object = ppc::ld32(base, lobby + 0x4E20);
        const ppc::Context saved = ctx;
        ppc::st8(base, lobby + 0x54AE, ppc::ld8(base, lobby + 0x54AE) | 8);
        if (party_object) {
            ctx.r[3] = party_object;
            sub_0225111C(ctx, base);
        }
        std::printf("net: swap %u depart pressed; sLobby +144=%u +148=%u +14C=%u\n", g_swaps, ppc::ld32(base, lobby + 0x144), ppc::ld32(base, lobby + 0x148), ppc::ld8(base, lobby + 0x14C));
        const uint64_t r1 = ctx.r[1];
        ctx = saved;
        ctx.r[1] = r1;
    }
    // MH3U_NET_PROBE_FERRY=frame: opens Neko's menu as talking to him does (0x02798F68): sLobby::startFerry 0x021CBA58(sLobby, the NPC, 0). My own unit stands
    // in for the NPC, which it only keeps. Logs the screen's state: uIDLobbyOnline (sID +0xE8) +0x3F4 state, +0x408 cursor.
    static const std::string ferry = env("MH3U_NET_PROBE_FERRY");
    if (!ferry.empty()) {
        const uint32_t game = ppc::ld32(base, 0x101FDB9C), players = ppc::ld32(base, kPlayers), sid = ppc::ld32(base, 0x10314578);
        if (std::strtoul(ferry.c_str(), nullptr, 10) == g_swaps) {
            const ppc::Context saved = ctx;
            ctx.r[3] = ppc::ld32(base, 0x103147A0);
            ctx.r[4] = ppc::ld32(base, players + 0x40 + ppc::ld8(base, game + 0x4B) * 4);
            ctx.r[5] = 0;
            sub_021CBA58(ctx, base);
            std::printf("net: swap %u opened the ferry menu\n", g_swaps);
            const uint64_t r1 = ctx.r[1];
            ctx = saved;
            ctx.r[1] = r1;
        }
        // MH3U_NET_PROBE_ROW="frame:row": puts the list's cursor on a row (+0x404, +0x408), as a person would with the d-pad
        static const std::string row_env = env("MH3U_NET_PROBE_ROW");
        uint32_t row_at = 0, row = 0;
        if (g_online_screen && !row_env.empty() && std::sscanf(row_env.c_str(), "%u:%u", &row_at, &row) == 2 && row_at == g_swaps) {
            ppc::st32(base, g_online_screen + 0x404, row);
            ppc::st32(base, g_online_screen + 0x408, row);
            std::printf("net: swap %u cursor on row %u\n", g_swaps, row);
        }
        static uint32_t last_screen = 0xFFFFFFFF;
        const uint32_t lobby = ppc::ld32(base, 0x103147A0);
        const uint32_t now = (g_online_screen ? ppc::ld32(base, g_online_screen + 0x3F4) << 16 | ppc::ld8(base, g_online_screen + 0x408) << 8 : 0) | ppc::ld8(base, lobby + 0x5450);
        if (now != last_screen) {
            std::printf("net: swap %u ferry: screen state %X cursor %u; lobby mode %02X phase %u, +144=%u; place %u\n", g_swaps, now >> 16, (now >> 8) & 0xFF, now & 0xFF,
                        ppc::ld8(base, lobby + 0x9624), ppc::ld32(base, lobby + 0x144), g_online_screen ? ppc::ld32(base, g_online_screen + 0x400) : 0);
            last_screen = now;
        }
    }
    // MH3U_NET_PROBE_CHASE=frame: from then on my hunter follows large monster slot 0: to its zone (state +0xEB) with the game's zone change, and
    // beside it once there, to test damage without walking
    static const std::string chase = env("MH3U_NET_PROBE_CHASE");
    if (!chase.empty() && g_swaps >= std::strtoul(chase.c_str(), nullptr, 10)) {
        const uint32_t game = ppc::ld32(base, 0x101FDB9C), players = ppc::ld32(base, kPlayers), enemies = ppc::ld32(base, 0x10313260);
        const int8_t slot = static_cast<int8_t>(ppc::ld8(base, game + 0x4B));
        const uint32_t me = players && slot >= 0 ? ppc::ld32(base, players + 0x40 + static_cast<uint32_t>(slot) * 4) : 0;
        const uint32_t monster = enemies ? ppc::ld32(base, enemies + 0x34) : 0;
        const uint32_t state = monster ? ppc::ld32(base, monster + 0x1BE0) : 0;
        if (me && state && ppc::ld32(base, me + 0xE30)) {
            const uint32_t pad = ppc::ld32(base, me + 0xE30);
            const uint8_t zone = ppc::ld8(base, state + 0xEB), here = ppc::ld8(base, game + 0x186);
            static uint32_t last_hop = 0;
            if (zone != here && zone != 0xFF && g_swaps - last_hop > 900 && (ppc::ld8(base, game + 0x196) == 2 || ppc::ld8(base, game + 0x196) == 3)) {
                last_hop = g_swaps;
                std::printf("net: swap %u chase: monster in zone %u, I am in %u\n", g_swaps, zone, here);
                const ppc::Context saved = ctx;
                const uint32_t frame = static_cast<uint32_t>(ctx.r[1]) - 0x40;
                ppc::st32(base, frame, static_cast<uint32_t>(ctx.r[1]));
                ctx.r[1] = frame;
                ppc::stf32(base, frame + 0x10, ppc::ldf32(base, monster + 0x40) + 150.0f);
                ppc::stf32(base, frame + 0x14, ppc::ldf32(base, monster + 0x44));
                ppc::stf32(base, frame + 0x18, ppc::ldf32(base, monster + 0x48) + 150.0f);
                ctx.r[3] = game;
                ctx.r[4] = zone;
                ctx.r[5] = frame + 0x10;
                ctx.r[6] = 0;
                sub_02118F8C(ctx, base);
                const uint64_t r1 = ctx.r[1];
                ctx = saved;
                ctx.r[1] = r1;
                ctx.r[1] = saved.r[1];
            } else if (zone == here && g_swaps % 240 == 0 && g_swaps - last_hop > 900 && ppc::ldf32(base, monster + 0x44) < 500.0f) {
                ppc::stf32(base, pad + 0xE0, ppc::ldf32(base, monster + 0x40) + 150.0f);
                ppc::stf32(base, pad + 0xE4, ppc::ldf32(base, monster + 0x44));
                ppc::stf32(base, pad + 0xE8, ppc::ldf32(base, monster + 0x48) + 150.0f);
            }
        }
    }
    // MH3U_NET_PROBE_QJOIN="frame:number:entry": the real join: 0x021E355C(sLobby, my pad block, number, 2), party entry index at sLobby +0x6B38 +0x374
    static const std::string qjoin = env("MH3U_NET_PROBE_QJOIN");
    uint32_t qjoin_at = 0, qjoin_number = 0, qjoin_entry = 0;
    if (!qjoin.empty() && std::sscanf(qjoin.c_str(), "%u:%u:%u", &qjoin_at, &qjoin_number, &qjoin_entry) == 3 && qjoin_at == g_swaps) {
        const uint32_t game = ppc::ld32(base, 0x101FDB9C), players = ppc::ld32(base, kPlayers), lobby = ppc::ld32(base, 0x103147A0);
        const uint32_t me = ppc::ld32(base, players + 0x40 + ppc::ld8(base, game + 0x4B) * 4);
        const ppc::Context saved = ctx;
        const uint32_t party_object = ppc::ld32(base, lobby + 0x4E20);
        std::printf("net: swap %u joining quest %u, party object %08X\n", g_swaps, qjoin_number, party_object);
        if (party_object) {
            ctx.r[3] = party_object;
            sub_02250C58(ctx, base);
            const uint32_t entries = static_cast<uint32_t>(ctx.r[3]);
            for (uint32_t i = 0; i < 4; ++i)
                std::printf("net:   entry %u id %04X state %u mask %02X host %d\n", i, ppc::ld16(base, entries + i * 0x9C4 + 4), ppc::ld8(base, entries + i * 0x9C4 + 6),
                            ppc::ld8(base, entries + i * 0x9C4 + 7), static_cast<int8_t>(ppc::ld8(base, entries + i * 0x9C4 + 8)));
        }
        ppc::st32(base, lobby + 0x6B38 + 0x374, qjoin_entry);
        ctx.r[3] = lobby;
        ctx.r[4] = ppc::ld32(base, me + 0xE30);
        ctx.r[5] = qjoin_number;
        ctx.r[6] = 2;
        sub_021E355C(ctx, base);
        std::printf("net: join result %08X, sGame+4A=%d\n", static_cast<uint32_t>(ctx.r[3]), static_cast<int8_t>(ppc::ld8(base, game + 0x4A)));
        const uint64_t r1 = ctx.r[1];
        ctx = saved;
        ctx.r[1] = r1;
    }
    // MH3U_NET_PROBE_PARTY="frame:host_member:mask": both copies' party says who is in: my entry (0x9C4 each, from 0x02250C58) +0xB = mask,
    // then the game's table builder 0x0225B238, and sGame +0x4A = the party's host. Stands in for the join handshake.
    static const std::string party = env("MH3U_NET_PROBE_PARTY");
    uint32_t party_at = 0, party_host = 0, party_mask = 0;
    if (!party.empty() && std::sscanf(party.c_str(), "%u:%u:%x", &party_at, &party_host, &party_mask) == 3 && party_at == g_swaps) {
        const ppc::Context saved = ctx;
        const uint32_t lobby = ppc::ld32(base, 0x103147A0), game = ppc::ld32(base, 0x101FDB9C), proc = ppc::ld32(base, 0x103A2DB0);
        const uint32_t party_object = ppc::ld32(base, lobby + 0x4E20);
        if (party_object) {
            ctx.r[3] = party_object;
            sub_02250C58(ctx, base);
            const uint32_t entries = static_cast<uint32_t>(ctx.r[3]);
            ctx.r[3] = ppc::ld32(base, 0x103A33E8);
            sub_0226EFEC(ctx, base);
            const uint32_t me = static_cast<uint32_t>(ctx.r[3]);
            std::printf("net: party mask here was %02X\n", ppc::ld8(base, entries + me * 0x9C4 + 0xB));
            if (party_mask) ppc::st8(base, entries + me * 0x9C4 + 0xB, static_cast<uint8_t>(party_mask));
            ctx.r[3] = proc;
            sub_0225B238(ctx, base);
            ppc::st8(base, game + 0x4A, static_cast<uint8_t>(party_host));
            std::printf("net: swap %u party: member %u, entry +4A=%d, table count %u, rows %d %d\n", g_swaps, me,
                        static_cast<int8_t>(ppc::ld8(base, entries + me * 0x9C4 + 0x4A)), ppc::ld8(base, proc + 0xC3A),
                        static_cast<int8_t>(ppc::ld8(base, proc + 0xC41)), static_cast<int8_t>(ppc::ld8(base, proc + 0xC41 + 0xD0)));
        } else {
            std::printf("net: swap %u party: no party object\n", g_swaps);
        }
        const uint64_t r1 = ctx.r[1];
        ctx = saved;
        ctx.r[1] = r1;
    }
    // MH3U_NET_PROBE_ZONE="frame:zone;frame:zone": hop to a zone in a quest with the game's own 0x02118F8C(sGame, zone, position, facing)
    static const std::string zones = env("MH3U_NET_PROBE_ZONE");
    for (const char* q = zones.c_str(); q && *q;) {
        uint32_t at = 0, zone = 0;
        if (std::sscanf(q, "%u:%u", &at, &zone) == 2 && at == g_swaps) {
            const ppc::Context saved = ctx;
            const uint32_t game = ppc::ld32(base, 0x101FDB9C);
            std::printf("net: swap %u zone hop to %u (from %u, phase %u)\n", g_swaps, zone, ppc::ld8(base, game + 0x186), ppc::ld8(base, game + 0x196));
            const uint32_t frame = static_cast<uint32_t>(ctx.r[1]) - 0x40;
            ppc::st32(base, frame, static_cast<uint32_t>(ctx.r[1]));
            ctx.r[1] = frame;
            for (uint32_t o = 0; o < 12; o += 4) ppc::st32(base, frame + 0x10 + o, 0);  // position 0, 0, 0
            ctx.r[3] = game;
            ctx.r[4] = zone;
            ctx.r[5] = frame + 0x10;
            ctx.r[6] = 0;
            sub_02118F8C(ctx, base);
            ctx.r[1] = saved.r[1];
            const uint64_t r1 = ctx.r[1];
            ctx = saved;
            ctx.r[1] = r1;
        }
        q = std::strchr(q, ';');
        if (q) ++q;
    }
    // MH3U_NET_PROBE_QUEST="frame:number": the launch step of the hall's state machine (0x0214E8A8) without its menus
    static const std::string quest = env("MH3U_NET_PROBE_QUEST");
    uint32_t quest_at = 0, quest_number = 0;
    if (!quest.empty() && std::sscanf(quest.c_str(), "%u:%u", &quest_at, &quest_number) == 2 && quest_at == g_swaps) {
        const uint32_t game_quest = ppc::ld32(base, 0x101FDB9C) + 0x34, lobby = ppc::ld32(base, 0x103147A0);
        std::printf("net: swap %u starting quest %u\n", g_swaps, quest_number);
        const ppc::Context saved = ctx;
        ppc::st16(base, lobby + 0x522C, static_cast<uint16_t>(quest_number));  // read by aQuest::vf5 through 0x02178EE0
        ppc::st16(base, lobby + 0x54AC, static_cast<uint16_t>(quest_number));  // as the quest select 0x021E355C does
        ppc::st8(base, lobby + 0x5450, 0);
        ctx.r[3] = ppc::ld32(base, 0x10314890);  // the launch step's calls, in its order
        sub_021512E8(ctx, base);
        ctx.r[3] = ppc::ld32(base, 0x10315F50);
        sub_02197EBC(ctx, base);
        ctx.r[3] = ppc::ld32(base, 0x1031537C);
        sub_02166A1C(ctx, base);
        ppc::st32(base, game_quest + 0x24, 1);
        ppc::st32(base, game_quest + 0x30, quest_number);
        ppc::st8(base, lobby + 0x154, 0);
        ppc::st8(base, lobby + 0x5234, 0);
        const uint32_t system = ppc::ld32(base, kAreaSystem);
        ctx.r[3] = system;
        ctx.r[4] = 0x1030CEA8;  // aQuestArea
        ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, system) + 0x5C));
        ctx.r[3] = ppc::ld32(base, 0x1031537C);
        sub_021788E4(ctx, base);
        ctx.r[3] = ppc::ld32(base, kLobbyNetwork);  // the launch step's broadcast, message type 0x25 (0x0214E9A0)
        ctx.r[4] = 1;
        sub_02265070(ctx, base);
        const uint64_t r1 = ctx.r[1];
        ctx = saved;
        ctx.r[1] = r1;
    }
    for (const AreaJump& jump : g_jumps) {
        if (jump.frame != g_swaps) continue;
        const uint32_t g_area_dti = jump.dti;
        // MH3U_NET_PROBE_SUB=n: the sub-area the Harbor areas load: sLobby (*0x103147A0) +0x5450, +1 stage, +2 and +0x15 sub-area,
        // as 0x0214CE3C fills them; their vf5 pass +1 and +2 to the stage setter 0x02190C50 through 0x0214CEE0
        static const std::string sub = env("MH3U_NET_PROBE_SUB");
        if (!sub.empty()) {
            const uint32_t info = ppc::ld32(base, 0x103147A0) + 0x5450;
            const uint8_t n = static_cast<uint8_t>(std::strtoul(sub.c_str(), nullptr, 10));
            std::printf("net: stage info was %X/%X (+15=%X), sub-area now %u\n", ppc::ld8(base, info + 1), ppc::ld8(base, info + 2),
                        ppc::ld8(base, info + 0x15), n);
            ppc::st8(base, info + 2, n);
            ppc::st8(base, info + 0x15, n);
        }
        const ppc::Context saved = ctx;
        const uint32_t system = ppc::ld32(base, kAreaSystem);
        ctx.r[3] = system;
        ctx.r[4] = g_area_dti;
        ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, system) + 0x5C));
        std::printf("net: area change to %08X at swap %u\n", g_area_dti, g_swaps);
        const uint64_t r1 = ctx.r[1];
        ctx = saved;
        ctx.r[1] = r1;
    }
    g_swap(ctx, base);
}

void register_online(const std::vector<std::string>& fp_imports) {
    // nn_fp: nothing in the HLE yet, so a call would abort. Everything succeeds with zeroed answers unless listed below.
    for (const std::string& n : fp_imports)
        reg(("nn_fp." + n).c_str(), [](ppc::Context& c, uint8_t*) { ret_value(c, 0); });
    reg("nn_fp.IsInitialized__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret_value(c, 1); });
    reg("nn_fp.HasLoggedIn__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret_value(c, 1); });
    reg("nn_fp.IsJoinable__Q2_2nn2fpFPCQ3_2nn2fp14FriendPresenceUL", [](ppc::Context& c, uint8_t*) { ret_value(c, 1); });
    reg("nn_fp.GetMyPrincipalId__Q2_2nn2fpFv", [](ppc::Context& c, uint8_t*) { ret_value(c, kPrincipalId); });
    reg("nn_fp.LoginAsync__Q2_2nn2fpFPFQ2_2nn6ResultPv_vPv", &fp_async_ok);
    // nn_ac: connected
    reg("nn_ac.Connect__Q2_2nn2acFv", [](ppc::Context& c, uint8_t*) { ret_value(c, 0); });
    reg("nn_ac.GetLastErrorCode__Q2_2nn2acFPUi", [](ppc::Context& c, uint8_t* b) {
        if (c.r[3]) ppc::st32(b, static_cast<uint32_t>(c.r[3]), 0);
        ret_value(c, 0);
    });
    reg("nn_ac.IsApplicationConnected__Q2_2nn2acFPb", [](ppc::Context& c, uint8_t* b) {
        if (c.r[3]) b[static_cast<uint32_t>(c.r[3])] = 1;
        ret_value(c, 0);
    });
    reg("nn_ac.GetAssignedAddress__Q2_2nn2acFPUl", [](ppc::Context& c, uint8_t* b) {
        if (c.r[3]) ppc::st32(b, static_cast<uint32_t>(c.r[3]), 0x7F000001);
        ret_value(c, 0);
    });
    // nn_act: an account with an id
    reg("nn_act.HasNfsAccount__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret_value(c, 1); });
    reg("nn_act.GetPrincipalId__Q2_2nn3actFv", [](ppc::Context& c, uint8_t*) { ret_value(c, kPrincipalId); });
    reg("nn_act.GetPrincipalIdEx__Q2_2nn3actFPUiUc", [](ppc::Context& c, uint8_t* b) {
        if (c.r[3]) ppc::st32(b, static_cast<uint32_t>(c.r[3]), kPrincipalId);
        ret_value(c, 0);
    });
    reg("nn_act.GetAccountId__Q2_2nn3actFPc", [](ppc::Context& c, uint8_t* b) {
        write_string(b, static_cast<uint32_t>(c.r[3]), "ChaChaHunter");
        ret_value(c, 0);
    });
    reg("nn_act.AcquireIndependentServiceToken__Q2_2nn3actFPcPCc", [](ppc::Context& c, uint8_t* b) {
        write_string(b, static_cast<uint32_t>(c.r[3]), "chacha-token");
        ret_value(c, 0);
    });
}

}  // namespace

void register_net_probe(uint8_t* base, const std::vector<std::string>& fp_imports) {
    const std::string mode = env("MH3U_NET_PROBE");
    if (mode.empty()) return;
    const bool online = mode == "online";
    std::printf("net: probe on (%s)\n", online ? "reports online" : "stubs as they are");
    trace_vtable(base, "Uds::Context", 0x100F2F40, 11, 35);
    trace_vtable(base, "Nex::Context", 0x100F34CC, 11, 35);
    trace_vtable(base, "Snow::Context", 0x100F3C48, 11, 35);
    trace_vtable(base, "Nnac::Context", 0x100F3A98, 11, 35);
    trace_vtable(base, "Uds::Session", 0x100F3310, 11, 36);
    trace_vtable(base, "Nex::Session", 0x100F38DC, 11, 36);
    trace_vtable(base, "Snow::Session", 0x100F4060, 11, 36);
    trace_vtable(base, "Uds::P2p", 0x100F30FC, 11, 15);
    trace_vtable(base, "Nex::P2p", 0x100F36DC, 11, 15);
    trace_vtable(base, "Snow::P2p", 0x100F3E5C, 11, 15);
    {
        constexpr uint32_t kQuestAreaFake = 0x7FEFF000;
        ppc::register_indirect(kQuestAreaFake, &quest_area_step);
        ppc::st32(base, kQuestAreaVtable + 6 * 8 + 4, kQuestAreaFake);
        constexpr uint32_t kOnlineScreenFake = 0x7FEFF004;
        ppc::register_indirect(kOnlineScreenFake, &online_screen_step);
        ppc::st32(base, 0x1005FDC8 + 64 * 8 + 4, kOnlineScreenFake);
    }
    g_previous_hook = ppc::import_trace_hook;
    ppc::import_trace_hook = &import_hook;
    if (online) register_online(fp_imports);
}

void register_net_probe_jump() {
    if (env("MH3U_NET_PROBE").empty()) return;
    const std::string area = env("MH3U_NET_PROBE_AREA"), flags = env("MH3U_NET_PROBE_FLAGS");
    for (const char* q = area.c_str(); q && *q;) {
        AreaJump j{};
        if (std::sscanf(q, "%u:%x", &j.frame, &j.dti) == 2) g_jumps.push_back(j);
        q = std::strchr(q, ';');
        if (q) ++q;
    }
    for (const char* q = flags.c_str(); q && *q;) {
        FlagPoke p{};
        if (std::sscanf(q, "%u:j:%d", &p.frame, &p.join_index) == 2) {
            p.internet = true;
            p.bits = 0x20;
            g_pokes.push_back(p);
        } else if (std::sscanf(q, "%u:i:%x", &p.frame, &p.bits) == 2) {
            p.internet = true;
            g_pokes.push_back(p);
        } else if (std::sscanf(q, "%u:%x", &p.frame, &p.bits) == 2) {
            g_pokes.push_back(p);
        }
        q = std::strchr(q, ';');
        if (q) ++q;
    }
    g_swap = ppc::resolve_import("gx2.GX2SwapScanBuffers");
    if (g_swap) ppc::register_import("gx2.GX2SwapScanBuffers", &swap_with_jump);
}

}  // namespace mh3u::game
