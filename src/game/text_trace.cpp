// Game text lookups: traced (which code asks for which text - finding the code behind a menu from its labels) and
// replaced (our own entries in place of unused "DUMMY" ones, e.g. the item box menu's Item Sets).
//
// Replaces: sID::vf37 0x02141DE0 = sID::getText? (this, group < 0xAF, index) -> string: the message resource kept for
// the group at sID+0x78 (vf+0x54 on it). Every text lookup ends here: FUN_0205AD00(group, index) (302 callers) is a
// tail call into it through sID's vtable (+0x134), and code can call the vtable itself. Status: wip - the original
// always runs; its result is returned unchanged unless set_text() replaced that (group, index).
//
// MH3U_TRACE_TEXT=<first>-<last>[:<group>] logs each (group, index, caller) once for indices in [first, last]
// (and that group), e.g. MH3U_TRACE_TEXT=389-394:46 for the item box menu's options (GUI\font\lobby\Shop text).
#include "text_trace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

#include "functions.h"
#include "guest_memory.h"

namespace mh3u::game {
namespace {

uint32_t g_first = 1, g_last = 0;
int g_group = -1;
bool g_installed = false;
std::mutex g_m;
std::set<std::tuple<uint32_t, uint32_t, uint32_t>> g_seen;
struct Replacement {
    std::string text;
    uint32_t guest = 0, cap = 0;  // the string in guest memory (made on first use) and its room
};
// by (group, index, caller); caller 0 = any caller, a caller of its own wins
std::map<std::tuple<uint32_t, uint32_t, uint32_t>, Replacement> g_replace;

// the replacement's guest copy (refreshed: the text may have changed), made again when the text outgrew it
uint32_t guest_text(Replacement& r, uint8_t* base) {
    if (!r.guest || r.text.size() + 1 > r.cap)
        if (core::GuestMemory* gm = core::GuestMemory::current()) {
            r.cap = std::max<uint32_t>(static_cast<uint32_t>(r.text.size() + 1), 160);
            r.guest = gm->alloc_hle(r.cap, 4);
        }
    if (r.guest) std::memcpy(base + r.guest, r.text.c_str(), r.text.size() + 1);
    return r.guest;
}

PPC_FUNC(get_text) {
    const uint32_t group = ctx.r[4], index = ctx.r[5], caller = static_cast<uint32_t>(ctx.lr);
    sub_02141DE0__orig(ctx, base);
    {
        std::lock_guard g(g_m);
        auto it = g_replace.find({group, index, caller});
        if (it == g_replace.end()) it = g_replace.find({group, index, 0u});
        if (it != g_replace.end())
            if (const uint32_t s = guest_text(it->second, base)) ctx.r[3] = s;
        if (index < g_first || index > g_last || (g_group >= 0 && group != static_cast<uint32_t>(g_group))) return;
        if (!g_seen.emplace(group, index, caller).second) return;
    }
    std::string text;
    for (uint32_t a = ctx.r[3]; a && text.size() < 60; ++a) {
        const uint8_t c = ppc::ld8(base, a);
        if (!c) break;
        text += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
    }
    std::printf("text: group 0x%02X index %u from 0x%08X: \"%s\"\n", group, index, caller, text.c_str());
}

void install() {
    if (!g_installed) ppc::set_override(0x02141DE0, &get_text);
    g_installed = true;
}

}  // namespace

void register_text_trace() {
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, "MH3U_TRACE_TEXT") == 0 && v) {
        unsigned first = 0, last = 0, group = 0;
        const int got = sscanf_s(v, "%u-%u:%u", &first, &last, &group);
        if (got >= 2) {
            g_first = first;
            g_last = last;
            g_group = got >= 3 ? static_cast<int>(group) : -1;
            install();
            std::printf("text trace: indices %u-%u%s\n", first, last, got >= 3 ? " (one group)" : "");
        }
    }
    std::free(v);
}

void set_text(uint32_t group, uint32_t index, const std::string& utf8, uint32_t caller) {
    {
        std::lock_guard g(g_m);
        g_replace[{group, index, caller}].text = utf8;  // its guest copy is refreshed at the next lookup
    }
    if (!g_installed) install();
}

}  // namespace mh3u::game
