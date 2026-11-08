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

namespace mh3u::game {
namespace {

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

std::string env(const char* name) {  // empty when not set
    char* v = nullptr;
    size_t n = 0;
    std::string out;
    if (_dupenv_s(&v, &n, name) == 0 && v) out = v;
    std::free(v);
    return out;
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

PPC_FUNC(swap_with_jump) {
    lobby_network_tick(base);
    ++g_swaps;
    for (const AreaJump& jump : g_jumps) {
        if (jump.frame != g_swaps) continue;
        const uint32_t g_area_dti = jump.dti;
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
