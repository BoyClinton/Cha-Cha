// Item sets (always on, no MODS switch): a seventh option "Item Sets" in the item box menu, as in MHGU, with a
// sub-list "Register Item Set" (the pouch becomes the set) / "Load Item Set" (the pouch goes into the box, then the
// set's items come out of the box into the same pouch slots).
//
// Replaces: FUN_021F0E7C sLobby::boxMenuStep? (one step of the box menu's option list; called only by the box state
// machine FUN_02201508, state 2), and wraps the menu's drawing - FUN_026A6B80 (option rows) and FUN_02687D38 (screen,
// sub-list pop-up) and cGUIInstAnimation::updateTransforms? 0x02AE1E40 - so Item Sets is drawn in row 3, in the tan
// upper part, with the separator and the scroll's lower part one row down. Status: wip - read in Ghidra
// (docs/research-log.md, "Item box menu"); seen working in a window (2026-10-27): register / load, row 3, the set list.
//
// The box menu (state at sLobby+0x6F10): +0x6F16 option, +0x6F18 sub-list cursor, +0x6F1A level (0 options,
// 1 sub-list), +0x6F1C option count, +0x6F1E sub-list count. The step returns 1 when a sub-list entry is chosen,
// 2 when the menu is left. Per-option u16 tables: sub-list counts 0x10170F30, label bases (Shop text) 0x10170F40, help
// bases (Help text) 0x10170F50. The game has 6 options; slot 6 of each table exists and is used by nothing, and the
// state machine does nothing (a sound) for option 6 - so: count 7, slot 6 = 2 entries at Shop 386/387 ("DUMMY"),
// labels Shop 395 / help 180-182 (also "DUMMY") replaced with ours, and a chosen option-6 entry handled here.
//
// Pouch / box (sUserData, hunter = sUserData+0x38): u16 item id + u16 count per slot. Blademaster pouch hunter+0xA8
// (24 slots), gunner pouch hunter+0x108 (32), FUN_02199E48(sUserData, hunter) != 0 when the gunner pouch is in use;
// box sUserData+0x1C0, FUN_0219880C(sUserData, hunter) slots. Sets are kept per hunter name and pouch in
// <data>/save/80000001/cha-cha-item-sets.ini, 5 sets each; choosing Register / Load lists them (with a preview of the
// highlighted set's items in the help box).
#include "item_sets.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

#include "functions.h"
#include "guest_memory.h"
#include "text_trace.h"

namespace mh3u::game {
namespace {

constexpr uint32_t kSUserData = 0x10315F50;
constexpr uint32_t kCounts = 0x10170F30, kLabels = 0x10170F40, kHelp = 0x10170F50;
constexpr uint32_t kOption = 6;              // the new option's index
constexpr uint16_t kLabelBase = 386, kHelpBase = 180;
constexpr int kMaxStack = 99;                // box stacks
// the options in the order they are drawn: Item Sets sits in row 3, under Combine/Manage Items
constexpr uint16_t kOrder[7] = {0, 1, 2, kOption, 3, 4, 5};
constexpr int kPos[7] = {0, 1, 2, 4, 5, 6, 3};  // option -> place in kOrder

std::atomic<bool> g_on{false};
std::filesystem::path g_file;
std::function<void(const std::string&)> g_notify;
bool g_patched = false;

struct Slot {
    uint16_t id = 0, count = 0;
};

// The game's menu sounds (FUN_020BFC74(handle, id), as the box menu plays them): 0 a sub-list opens, 1 back, 2 not
// possible, 5 a choice made, 6 a page turned (Store Items' paging, FUN_020431F8). The handle is an 8-byte object on
// the guest stack.
enum Sound : uint32_t { kOpen = 0, kBack = 1, kError = 2, kChoose = 5, kPage = 6 };
void play(ppc::Context& ctx, uint8_t* base, Sound id) {
    const ppc::Context saved = ctx;
    ctx.r[1] -= 0x40;
    ctx.r[3] = ctx.r[1] + 0x20;
    ctx.r[4] = id;
    sub_020BFC74(ctx, base);
    ctx = saved;
}

void notify(const std::string& s) {
    std::printf("item sets: %s\n", s.c_str());
    if (g_notify) g_notify(s);
}

// a guest function's result (its registers saved and restored around the call)
template <class F>
uint32_t call(ppc::Context& ctx, uint8_t* base, F fn, uint32_t a, uint32_t b) {
    const ppc::Context saved = ctx;
    ctx.r[3] = a;
    ctx.r[4] = b;
    fn(ctx, base);
    const uint32_t r = ctx.r[3];
    ctx = saved;
    return r;
}

struct Pouch {
    uint32_t hunter = 0, addr = 0, slots = 0;
    bool gunner = false;
    std::string key;  // "<hunter name>/<pouch>"
};

Pouch pouch(ppc::Context& ctx, uint8_t* base) {
    Pouch p;
    const uint32_t ud = ppc::ld32(base, kSUserData);
    if (!ud) return p;
    p.hunter = ud + 0x38;
    p.gunner = call(ctx, base, sub_02199E48, ud, p.hunter) != 0;
    p.addr = p.hunter + (p.gunner ? 0x108 : 0xA8);
    p.slots = p.gunner ? 32 : 24;
    std::string name;  // the hunter's name (hunter data +3, as in the save file)
    for (uint32_t i = 0; i < 24; ++i) {
        const uint8_t c = ppc::ld8(base, p.hunter + 3 + i);
        if (!c) break;
        name += static_cast<char>(c);
    }
    p.key = (name.empty() ? std::string("hunter") : name) + (p.gunner ? "/gunner" : "/blademaster");
    return p;
}

std::vector<Slot> read_slots(uint8_t* base, uint32_t addr, uint32_t n) {
    std::vector<Slot> v(n);
    for (uint32_t i = 0; i < n; ++i) v[i] = {ppc::ld16(base, addr + i * 4), ppc::ld16(base, addr + i * 4 + 2)};
    return v;
}
void write_slot(uint8_t* base, uint32_t addr, uint32_t i, Slot s) {
    ppc::st16(base, addr + i * 4, s.id);
    ppc::st16(base, addr + i * 4 + 2, s.count);
}

// cha-cha-item-sets.ini: "<hunter name>/<pouch>#<set>=id:count,id:count,..." (one entry per pouch slot; a key
// without #<set>, from the first version, is set 1)
std::map<std::string, std::vector<Slot>> load_file() {
    std::map<std::string, std::vector<Slot>> sets;
    std::ifstream f(g_file);
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.rfind('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        std::vector<Slot> v;
        std::stringstream ss(line.substr(eq + 1));
        for (std::string e; std::getline(ss, e, ',');) {
            unsigned id = 0, n = 0;
            if (sscanf_s(e.c_str(), "%u:%u", &id, &n) == 2) v.push_back({static_cast<uint16_t>(id), static_cast<uint16_t>(n)});
        }
        std::string key = line.substr(0, eq);
        if (key.find('#') == std::string::npos) key += "#1";
        sets[key] = std::move(v);
    }
    return sets;
}
std::string set_key(const Pouch& p, int set) { return p.key + "#" + std::to_string(set + 1); }

// an item's name, as the game shows it (text group 4, index = item id)
std::string item_name(ppc::Context& ctx, uint8_t* base, uint16_t id) {
    const uint32_t s = call(ctx, base, sub_0205AD00, 4, id);
    std::string out;
    for (uint32_t a = s; a && out.size() < 64; ++a) {
        const uint8_t c = ppc::ld8(base, a);
        if (!c) break;
        out += static_cast<char>(c);
    }
    return out;
}
bool save_file(const std::map<std::string, std::vector<Slot>>& sets) {
    std::ofstream f(g_file, std::ios::trunc);
    f << "# Cha-Cha item sets (MODS -> GAMEPLAY -> ITEM SETS): <hunter name>/<pouch>#<set>=item:count per pouch slot\n";
    for (const auto& [k, v] : sets) {
        f << k << '=';
        for (size_t i = 0; i < v.size(); ++i) f << (i ? "," : "") << v[i].id << ':' << v[i].count;
        f << '\n';
    }
    return static_cast<bool>(f);
}

void register_set(ppc::Context& ctx, uint8_t* base, int set) {
    const Pouch p = pouch(ctx, base);
    if (!p.addr) return;
    auto sets = load_file();
    std::vector<Slot>& v = sets[set_key(p, set)] = read_slots(base, p.addr, p.slots);
    int items = 0;
    for (const Slot& s : v) items += s.id ? 1 : 0;
    if (!save_file(sets)) {
        play(ctx, base, kError);
        return notify("Couldn't save the item set");
    }
    play(ctx, base, kChoose);
    notify("Item set " + std::to_string(set + 1) + " registered (" + std::to_string(items) + (items == 1 ? " item, " : " items, ") +
           (p.gunner ? "Gunner's Pouch)" : "Blademaster's Pouch)"));
}

// What loading a set does, on copies of the pouch and the box: the pouch's items go into the box, then the set's
// come out of it (as many as the box holds). Used by Load and by the pouch preview.
struct Loaded {
    std::vector<Slot> pouch, box;
    int short_by = 0;  // items of the set the box did not have
};
Loaded simulate_load(std::vector<Slot> pch, std::vector<Slot> b, const std::vector<Slot>& want_slots) {
    // 1. the pouch into the box: onto stacks of the same item first, then into empty slots
    for (Slot& s : pch) {
        if (!s.id) continue;
        for (Slot& t : b)
            if (s.count && t.id == s.id && t.count < kMaxStack) {
                const uint16_t n = static_cast<uint16_t>(std::min<int>(s.count, kMaxStack - t.count));
                t.count = static_cast<uint16_t>(t.count + n);
                s.count = static_cast<uint16_t>(s.count - n);
            }
        for (Slot& t : b)
            if (s.count && !t.id) {
                t = s;
                s.count = 0;
            }
        if (!s.count) s.id = 0;  // left in the pouch when the box is full
    }
    // 2. the set out of the box, slot by slot
    int short_by = 0;
    for (size_t i = 0; i < want_slots.size() && i < pch.size(); ++i) {
        if (!want_slots[i].id) continue;
        size_t to = i;
        if (pch[to].id && pch[to].id != want_slots[i].id)  // a leftover there: the next free slot
            to = static_cast<size_t>(std::find_if(pch.begin(), pch.end(), [](const Slot& s) { return !s.id; }) - pch.begin());
        if (to >= pch.size()) {
            short_by += want_slots[i].count;
            continue;
        }
        int want = want_slots[i].count - (pch[to].id == want_slots[i].id ? pch[to].count : 0);
        for (Slot& t : b)
            if (want > 0 && t.id == want_slots[i].id) {
                const int n = std::min<int>(want, t.count);
                t.count = static_cast<uint16_t>(t.count - n);
                if (!t.count) t.id = 0;
                pch[to].id = want_slots[i].id;
                pch[to].count = static_cast<uint16_t>(pch[to].count + n);
                want -= n;
            }
        short_by += std::max(want, 0);
    }
    return {std::move(pch), std::move(b), short_by};
}

uint32_t box_slot_count(ppc::Context& ctx, uint8_t* base, const Pouch& p) {
    return std::min<uint32_t>(call(ctx, base, sub_0219880C, ppc::ld32(base, kSUserData), p.hunter), 2000);
}

void load_set(ppc::Context& ctx, uint8_t* base, int set) {
    const Pouch p = pouch(ctx, base);
    if (!p.addr) return;
    const auto sets = load_file();
    const auto it = sets.find(set_key(p, set));
    if (it == sets.end()) {
        play(ctx, base, kError);
        return notify("Item set " + std::to_string(set + 1) + " is empty");
    }
    const uint32_t box = ppc::ld32(base, kSUserData) + 0x1C0, box_slots = box_slot_count(ctx, base, p);
    const Loaded r = simulate_load(read_slots(base, p.addr, p.slots), read_slots(base, box, box_slots), it->second);
    for (uint32_t i = 0; i < box_slots; ++i) write_slot(base, box, i, r.box[i]);
    for (uint32_t i = 0; i < p.slots; ++i) write_slot(base, p.addr, i, r.pouch[i]);
    const std::string name = "Item set " + std::to_string(set + 1) + " loaded";
    play(ctx, base, kChoose);
    notify(r.short_by ? name + " - " + std::to_string(r.short_by) + " missing (not in the Item Box)" : name);
}

// Choosing Register / Load turns the sub-list into the list of sets (another level, drawn by the same pop-up): its
// labels move to Shop 380-384 ("DUMMY") with each set's name, and the help box shows the highlighted set's items
// (Help 181-185, replaced for the box menu's help lookup only).
enum class Pick { None, Register, Load };
Pick g_pick = Pick::None;
constexpr int kSets = 5;
std::vector<Slot> g_preview[kSets];  // the pouch window's contents for each set (made when the set list opens)
Pouch g_preview_pouch;
int g_shown_set = -1;          // the set the pouch window shows
int g_page = 0;                // its page of 8 slots (left / right in the set list)
constexpr uint16_t kSetLabelBase = 380;
constexpr uint32_t kHelpCaller = 0x026508C4;  // the box menu's help lookup (FUN_02650890)

void menu_texts() {  // the Register / Load sub-list
    set_text(0x2F, kHelpBase + 1, "Choose a set to register the items\r\nin your pouch as.", kHelpCaller);
    set_text(0x2F, kHelpBase + 2, "Choose a set: your pouch goes into the\r\nItem Box and the set comes out of it.", kHelpCaller);
}

void enter_pick(ppc::Context& ctx, uint8_t* base, uint32_t lobby, Pick pick) {
    const Pouch p = pouch(ctx, base);
    const auto sets = load_file();
    for (int k = 0; k < kSets; ++k) {
        const auto it = sets.find(set_key(p, k));
        std::vector<std::pair<std::string, int>> items;
        if (it != sets.end())
            for (const Slot& s : it->second)
                if (s.id) items.emplace_back(item_name(ctx, base, s.id), s.count);
        std::string label = "Set " + std::to_string(k + 1);
        if (items.empty()) {
            label += " (empty)";
        } else {
            std::string first = items.front().first;
            if (first.size() > 16) first = first.substr(0, 15) + ".";
            label += ": " + first;
        }
        // the preview: up to three lines of "Name x3, ..."
        std::string preview, line;
        int lines = 0;
        size_t shown = 0;
        for (const auto& [name, n] : items) {
            const std::string e = name + " x" + std::to_string(n);
            if (!line.empty() && line.size() + 2 + e.size() > 40) {
                if (++lines == 3) break;
                preview += line + ",\r\n";
                line.clear();
            }
            line += (line.empty() ? "" : ", ") + e;
            ++shown;
        }
        if (lines < 3) preview += line;
        if (shown < items.size()) preview += " ... (" + std::to_string(items.size() - shown) + " more)";
        if (items.empty()) preview = pick == Pick::Register ? "Empty. Register your pouch here." : "No items registered in this set.";
        set_text(0x2E, kSetLabelBase + k, label);
        set_text(0x2F, kHelpBase + 1 + k, preview, kHelpCaller);
        // the pouch window's preview: what Load would leave in the pouch (from what the pouch and box hold), or for
        // Register the set as it is
        std::vector<Slot>& pv = g_preview[k];
        pv.assign(p.slots, Slot{});
        if (it != sets.end()) {
            if (pick == Pick::Load) {
                const uint32_t box = ppc::ld32(base, kSUserData) + 0x1C0;
                pv = simulate_load(read_slots(base, p.addr, p.slots), read_slots(base, box, box_slot_count(ctx, base, p)), it->second).pouch;
            } else {
                for (size_t i = 0; i < it->second.size() && i < pv.size(); ++i) pv[i] = it->second[i];
            }
        }
    }
    g_preview_pouch = p;
    g_shown_set = -1;  // the window shows the new previews
    g_page = 0;
    ppc::st16(base, kLabels + kOption * 2, kSetLabelBase);
    ppc::st16(base, kCounts + kOption * 2, kSets);
    ppc::st16(base, lobby + 0x6F1E, kSets);
    ppc::st16(base, lobby + 0x6F18, 0);
    g_pick = pick;
    play(ctx, base, kChoose);
}

void leave_pick(uint8_t* base, uint32_t lobby) {
    const Pick was = g_pick;
    g_pick = Pick::None;
    menu_texts();
    ppc::st16(base, kLabels + kOption * 2, kLabelBase);
    ppc::st16(base, kCounts + kOption * 2, 2);
    ppc::st16(base, lobby + 0x6F1E, 2);
    ppc::st16(base, lobby + 0x6F18, was == Pick::Load ? 1 : 0);
}

PPC_FUNC(box_menu_step) {
    const uint32_t lobby = ctx.r[3];
    const bool on = g_on;
    if (on && !g_patched) {  // the menu tables' unused slot 6: two entries, our labels and help
        ppc::st16(base, kCounts + kOption * 2, 2);
        ppc::st16(base, kLabels + kOption * 2, kLabelBase);
        ppc::st16(base, kHelp + kOption * 2, kHelpBase);
        g_patched = true;
    }
    // the small box (lobby+0x6F12 set: 3 options, Store / Remove / Combine, the 3-row scroll; FUN_02201508 leaves the
    // camera alone for it) gets the same 7 options with only 0-2 and Item Sets reachable and drawn
    const bool small = ppc::ld8(base, lobby + 0x6F12) != 0;
    const uint16_t normal = small ? 3 : 6, count = ppc::ld16(base, lobby + 0x6F1C);
    if (on && count == normal) ppc::st16(base, lobby + 0x6F1C, 7);
    if (!on && count == 7) {  // switched off while the menu is open
        ppc::st16(base, lobby + 0x6F1C, normal);
        if (ppc::ld16(base, lobby + 0x6F16) >= normal) ppc::st16(base, lobby + 0x6F16, normal - 1);
    }
    const uint16_t before = ppc::ld16(base, lobby + 0x6F16), level = ppc::ld16(base, lobby + 0x6F1A);
    sub_021F0E7C__orig(ctx, base);
    // up / down among the options follow the order on screen (Item Sets in row 3, under Combine/Manage Items)
    const uint16_t after = ppc::ld16(base, lobby + 0x6F16);
    if (on && level == 0 && ppc::ld16(base, lobby + 0x6F1A) == 0 && ppc::ld16(base, lobby + 0x6F1C) == 7 && after != before &&
        before < 7) {
        const int step = (after - before + 7) % 7 == 1 ? 1 : (after - before + 7) % 7 == 6 ? -1 : 0;
        const int n = small ? 4 : 7;  // the small box: the first 4 of kOrder (0, 1, 2, Item Sets)
        if (step) ppc::st16(base, lobby + 0x6F16, kOrder[(std::min(kPos[before], n - 1) + step + n) % n]);
    }
    const uint16_t level_now = ppc::ld16(base, lobby + 0x6F1A);
    if (g_pick != Pick::None) {
        // left / right (menu direction bits from FUN_0221A430, with key repeat: 1 up, 2 down, 4 left, 8 right - seen
        // in a window) turn the pouch window's pages, as Store Items turns the box's
        if (level == 1) {
            const uint32_t dirs = call(ctx, base, sub_0221A430, lobby, 0);
            const int pages = static_cast<int>((g_preview_pouch.slots + 7) / 8);
            if (pages > 1 && (dirs & 0xC)) {
                g_page = (g_page + ((dirs & 8) ? 1 : pages - 1)) % pages;
                play(ctx, base, kPage);
            }
        }
        if (level_now == 0 && level == 0) {  // the menu was left while the set list was up
            leave_pick(base, lobby);
        } else if (level == 1 && level_now == 0 && ctx.r[3] == 0) {  // back (B): to Register / Load
            leave_pick(base, lobby);
            ppc::st16(base, lobby + 0x6F1A, 1);
        } else if (ctx.r[3] == 1 && ppc::ld16(base, lobby + 0x6F16) == kOption) {
            const int set = ppc::ld16(base, lobby + 0x6F18);
            if (g_pick == Pick::Register) register_set(ctx, base, set);
            else load_set(ctx, base, set);
            leave_pick(base, lobby);
            ppc::st16(base, lobby + 0x6F1A, 0);  // back to the options
            ctx.r[3] = 0;
        }
        return;
    }
    if (!on || ctx.r[3] != 1 || ppc::ld16(base, lobby + 0x6F16) != kOption) return;
    enter_pick(ctx, base, lobby, ppc::ld16(base, lobby + 0x6F18) == 0 ? Pick::Register : Pick::Load);
    ctx.r[3] = 0;  // the sub-list stays open, now listing the sets
}

// The menu's layout (uIDLobbyMaki widget, GUI tree at +0xF4; instance: id +4, first child +0x60, next sibling +0x64,
// flags +0x54 (bit 0 shown), x +0xA0, y +0xA4). The rows container (459) has 8 row slots 54 apart: plates +0x2B0[i],
// highlight frames +0x2AC[i], texts +0x2B8[i]; slots 4-7 sit in a plain group (574). With 6 options the game uses
// slots 0-2 and 4-6, and slot 3 is the gap where the scroll's tan upper part ends and the separator line runs.
// With Item Sets the rows become: slots 0-2, Item Sets in slot 3, the gap in slot 4, the other three in slots 5-7 -
// and the gap moves a row down: the scroll (animated instance 428) and the separator (593) are drawn from object
// trees (instance +0xFC, cGUIObject: first child +0x68, next sibling +0x6C; cGUIObj2D PosY +0x84), and in the
// scroll's tree (root children: background group, tan panel, its seam, top roller group, bottom roller group, a
// texture set) the tan panel, the seam and the bottom roller group move one row down, so the scroll is a row longer (seen in captures, 2026-10-27).
constexpr float kRow = 54.0f;

// MH3U_DUMP_GUI: the first time the box menu draws, every instance of its widget and which row slot uses it, and the
// objects of the scroll and the separator.
bool g_dump_gui = false;
void dump_gui(uint8_t* base, uint32_t maki) {
    std::map<uint32_t, std::string> role;
    for (auto [off, what] : {std::pair<uint32_t, const char*>{0x2AC, "highlight"}, {0x2B0, "plate"}, {0x2B8, "text"}})
        if (const uint32_t arr = ppc::ld32(base, maki + off))
            for (uint32_t i = 0; i < 8; ++i)
                if (const uint32_t e = ppc::ld32(base, arr + i * 4)) role[e] = std::string(what) + " " + std::to_string(i);
    std::printf("box menu GUI (widget %08X):\n", maki);
    std::vector<std::pair<uint32_t, int>> stack;
    if (const uint32_t root = ppc::ld32(base, maki + 0xF4)) stack.push_back({root, 0});
    for (int n = 0; !stack.empty() && n < 600; ++n) {
        const auto [e, depth] = stack.back();
        stack.pop_back();
        const auto it = role.find(e);
        std::printf("  %*s%08X vt %08X id %u flags %08X x %.1f y %.1f %s\n", depth * 2, "", e, ppc::ld32(base, e), ppc::ld32(base, e + 4),
                    ppc::ld32(base, e + 0x54), ppc::ldf32(base, e + 0xA0), ppc::ldf32(base, e + 0xA4),
                    it != role.end() ? it->second.c_str() : "");
        if (const uint32_t id = ppc::ld32(base, e + 4); id == 428 || id == 593) {
            std::vector<std::pair<uint32_t, int>> objs;
            if (const uint32_t o = ppc::ld32(base, e + 0xFC)) objs.push_back({o, depth + 2});
            for (int k = 0; !objs.empty() && k < 80; ++k) {
                const auto [o, d] = objs.back();
                objs.pop_back();
                std::printf("  %*sobject %08X vt %08X x %.1f y %.1f\n", d * 2, "", o, ppc::ld32(base, o), ppc::ldf32(base, o + 0x80),
                            ppc::ldf32(base, o + 0x84));
                if (const uint32_t next = ppc::ld32(base, o + 0x6C)) objs.push_back({next, d});
                if (const uint32_t child = ppc::ld32(base, o + 0x68)) objs.push_back({child, d + 1});
            }
        }
        if (const uint32_t next = ppc::ld32(base, e + 0x64)) stack.push_back({next, depth});
        if (const uint32_t child = ppc::ld32(base, e + 0x60)) stack.push_back({child, depth + 1});
    }
}

// MH3U_GUI_TEST: a screenshot of the box menu (build/frame_<n>.bmp) half a second after it first shows Item Sets, once
// for the full box and once for the small one.
std::function<void()> g_capture;
bool g_gui_test = false;
int g_test_frames[2] = {};

uint32_t find_instance(uint8_t* base, uint32_t maki, uint32_t id) {
    std::vector<uint32_t> stack;
    if (const uint32_t root = ppc::ld32(base, maki + 0xF4)) stack.push_back(root);
    for (int n = 0; !stack.empty() && n < 600; ++n) {
        const uint32_t e = stack.back();
        stack.pop_back();
        if (ppc::ld32(base, e + 4) == id) return e;
        if (const uint32_t next = ppc::ld32(base, e + 0x64)) stack.push_back(next);
        if (const uint32_t child = ppc::ld32(base, e + 0x60)) stack.push_back(child);
    }
    return 0;
}

uint32_t nth_child(uint8_t* base, uint32_t obj, int n) {
    uint32_t o = obj ? ppc::ld32(base, obj + 0x68) : 0;
    for (; o && n > 0; --n) o = ppc::ld32(base, o + 0x6C);
    return o;
}

// An object moved a row down while the box menu with Item Sets is drawn. The animation's tracks write PosY when the
// frame changes (the scroll opening), so the shift is applied again whenever PosY is not the value written last.
// Once the menu is no longer drawn the objects get their own PosY back and are forgotten (the GUI may be freed).
struct Moved {
    uint32_t inst, root, obj, vt;
    float base = 0, written = 0;
};
std::vector<Moved> g_moved;
uint32_t g_moved_widget = 0;
// the shift is on while drawBoxMenuOptions? ran lately: it sets this, each transform update of the scroll counts down
int g_layout_frames = 0;

void find_moved(uint8_t* base, uint32_t maki) {
    g_moved.clear();
    g_moved_widget = maki;
    const auto add = [&](uint32_t inst, uint32_t root, uint32_t obj, uint32_t vt) {
        if (!obj || ppc::ld32(base, obj) != vt) {
            std::printf("item sets: box menu layout object not found (instance %08X)\n", inst);
            return;
        }
        const float y = ppc::ldf32(base, obj + 0x84);
        g_moved.push_back({inst, root, obj, vt, y, y});
    };
    constexpr uint32_t kNull = 0x100A7CB0, kTexture = 0x100A7F30;  // cGUIObjNull, cGUIObjTexture vtables
    if (const uint32_t scroll = find_instance(base, maki, 428)) {
        const uint32_t root = ppc::ld32(base, scroll + 0xFC);
        add(scroll, root, nth_child(base, root, 1), kTexture);  // the tan panel
        add(scroll, root, nth_child(base, root, 2), kTexture);  // its seam
        add(scroll, root, nth_child(base, root, 4), kNull);     // the bottom roller
    }
    if (const uint32_t line = find_instance(base, maki, 593)) {
        const uint32_t root = ppc::ld32(base, line + 0xFC);
        add(line, root, nth_child(base, root, 0), kTexture);  // the separator
    }
}

// cGUIInstAnimation::updateTransforms? (0x02AE1E40, r3 = the instance; vf13 of every animated instance): the moved
// objects get their PosY before the instance redoes its objects' transforms (object root +0x8C bit 1 asks for that).
PPC_FUNC(update_transforms) {
    const uint32_t inst = ctx.r[3];
    if (!g_moved.empty() && inst == g_moved.front().inst && g_layout_frames > 0) --g_layout_frames;
    const bool active = g_on && g_layout_frames > 0;
    for (Moved& m : g_moved) {
        if (m.inst != inst || ppc::ld32(base, m.obj) != m.vt) continue;
        const float y = ppc::ldf32(base, m.obj + 0x84);
        if (y != m.written) m.base = y;  // the animation wrote it
        m.written = m.base + (active ? kRow : 0.0f);
        if (m.written == y) continue;
        ppc::st32(base, m.obj + 0x84, std::bit_cast<uint32_t>(m.written));
        ppc::st32(base, m.obj + 0x54, ppc::ld32(base, m.obj + 0x54) | 0x20000);
        ppc::st32(base, m.root + 0x8C, ppc::ld32(base, m.root + 0x8C) | 2);
    }
    if (!active) {  // put back above: forget this instance's objects
        std::erase_if(g_moved, [&](const Moved& m) { return m.inst == inst; });
        if (g_moved.empty()) g_moved_widget = 0;
    }
    sub_02AE1E40__orig(ctx, base);
}

// The option rows (uIDLobbyMaki::drawBoxMenuOptions? 0x026A6B80, r3 = the menu widget, r4 = the state): option i is
// drawn in row slot i (i < 3) or i + 1 from the widget's arrays. With 7 options the highlight frame and text arrays
// are rearranged for the call so that options 3-5 land in slots 5-7 and Item Sets (6) in slot 3; afterwards the
// plates of slots 3 and 7 are shown (the game's 6-option layout leaves them bare) and slot 4's hidden (the gap). In
// the small box (state +2; the game's 3-row layout) rows 4-7 are hidden instead.
PPC_FUNC(draw_options) {
    const uint32_t maki = ctx.r[3], state = ctx.r[4];
    if (g_dump_gui) {
        g_dump_gui = false;
        dump_gui(base, maki);
    }
    const bool ours = g_on && ppc::ld16(base, state + 0xC) == 7;
    const uint32_t frames = ppc::ld32(base, maki + 0x2AC), texts = ppc::ld32(base, maki + 0x2B8), plates = ppc::ld32(base, maki + 0x2B0);
    // the game's slot k gets the element of slot kTo[k]: 3 (unused) -> 4, 4 -> 5, 5 -> 6, 6 -> 7, 7 (Item Sets) -> 3
    constexpr int kTo[8] = {0, 1, 2, 4, 5, 6, 7, 3};
    uint32_t saved_frames[8] = {}, saved_texts[8] = {};
    const auto rearrange = [&](uint32_t arr, uint32_t (&saved)[8]) {
        if (!arr) return;
        for (int k = 0; k < 8; ++k) saved[k] = ppc::ld32(base, arr + k * 4);
        for (int k = 0; k < 8; ++k) ppc::st32(base, arr + k * 4, saved[kTo[k]]);
    };
    const auto restore = [&](uint32_t arr, const uint32_t (&saved)[8]) {
        if (arr)
            for (int k = 0; k < 8; ++k) ppc::st32(base, arr + k * 4, saved[k]);
    };
    if (ours) {
        rearrange(frames, saved_frames);
        rearrange(texts, saved_texts);
    }
    sub_026A6B80__orig(ctx, base);
    if (!ours) return;
    restore(frames, saved_frames);
    restore(texts, saved_texts);
    const auto show = [&](uint32_t elem, bool on) {
        if (!elem) return;
        const ppc::Context saved = ctx;
        ctx.r[3] = maki;
        ctx.r[4] = elem;
        ctx.r[5] = on ? 1 : 0;
        sub_0228D1B4(ctx, base);
        ctx = saved;
    };
    // the texts are GUI objects, not instances: hidden as the game hides unused rows' texts (FUN_02AF349C(text, 0)),
    // shown by their flag bit 0 (what FUN_02ADB830 does when showing; hiding with it walks instance children)
    const auto show_text = [&](uint32_t text, bool on) {
        if (!text) return;
        if (on) {
            ppc::st32(base, text + 0x54, ppc::ld32(base, text + 0x54) | 1);
            return;
        }
        const ppc::Context saved = ctx;
        ctx.r[3] = text;
        ctx.r[4] = 0;
        sub_02AF349C(ctx, base);
        ctx = saved;
    };
    const auto plate = [&](uint32_t slot) { return plates ? ppc::ld32(base, plates + slot * 4) : 0; };
    show(plate(3), true);
    if (ppc::ld8(base, state + 2)) {  // the small box: rows 0-3 only
        for (uint32_t slot = 4; slot < 8; ++slot) {
            show(plate(slot), false);
            show_text(saved_texts[slot], false);
        }
    } else {
        show(plate(4), false);  // the gap
        show(plate(7), true);   // Equipment Sets: slot 7 is bare in the game's 6-option layout
        show_text(saved_texts[7], true);
    }
    if (maki != g_moved_widget) find_moved(base, maki);
    g_layout_frames = 8;
    if (const int kind = ppc::ld8(base, state + 2) ? 1 : 0; g_gui_test && ++g_test_frames[kind] == 30 && g_capture) {
        std::printf("gui test: capture (%s box)\n", kind ? "small" : "full");
        g_capture();
    }
}

// The pouch window: uIDCommonPauch, the shared pouch widget (sID sub-unit 2 (FUN_0213F040) +0x94; the same one
// Store Items shows). Store Items draws it every frame with FUN_026697C0: open it (vf+0xE4 1, flags +0xC |= 0x100000,
// +0x3F4 = 8, its animation +0x2D4), reset (FUN_0265E308), frame "Item List" (FUN_0265F010 mode 1), layout
// (FUN_0265FAE4 8), then FUN_026611B8(widget, slots, entries, page (1-based), pages, 0, flags 0x800, &out, 8) lists 8
// slots (u16 id + u16 count each) from any slot array - entries: 8 x 0x18 bytes, +1 highlighted, +2 shown, +4 slot
// index. The preview runs the same steps with the set's slots in a guest buffer of its own; the pouch is not touched.
uint32_t g_preview_slots = 0, g_preview_entries = 0;  // guest buffers (made once)

uint32_t pouch_widget(ppc::Context& ctx, uint8_t* base) {
    const uint32_t ui = call(ctx, base, sub_0213F040, ppc::ld32(base, 0x10314578), 2);  // sID::mpInstance
    return ui ? ppc::ld32(base, ui + 0x94) : 0;
}

void show_preview(ppc::Context& ctx, uint8_t* base, int set) {
    const Pouch& p = g_preview_pouch;
    if (!p.addr || set < 0 || set >= kSets || g_preview[set].size() != p.slots) return;
    const uint32_t w = pouch_widget(ctx, base);
    core::GuestMemory* gm = core::GuestMemory::current();
    if (!w || !gm) return;
    if (!g_preview_slots) {
        g_preview_slots = gm->alloc_hle(32 * 4 + 16, 16);
        g_preview_entries = gm->alloc_hle(8 * 0x18 + 16, 16);
    }
    for (uint32_t i = 0; i < 32; ++i) write_slot(base, g_preview_slots, i, i < p.slots ? g_preview[set][i] : Slot{});
    const uint32_t pages = (p.slots + 7) / 8, page = static_cast<uint32_t>(g_page) % pages;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t e = g_preview_entries + i * 0x18;
        for (uint32_t b = 0; b < 0x18; ++b) ppc::st8(base, e + b, 0);
        ppc::st8(base, e + 2, 1);
        ppc::st16(base, e + 4, static_cast<uint16_t>(page * 8 + i));
    }
    // sID's pouch-window selector (FUN_0214218C(sID) -> u32): Store Items sets 6 before drawing (8 for other screens;
    // the box menu resets it to 0 when it opens)
    if (const uint32_t sel = call(ctx, base, sub_0214218C, ppc::ld32(base, 0x10314578), 0)) ppc::st32(base, sel, 6);
    const ppc::Context saved = ctx;
    const auto run = [&](auto fn, uint32_t a, uint32_t b, uint32_t c) {
        ctx = saved;
        ctx.r[3] = w;
        ctx.r[4] = a;
        ctx.r[5] = b;
        ctx.r[6] = c;
        fn(ctx, base);
    };
    ppc::st16(base, w + 0x3F4, 8);
    ctx = saved;  // its animation (+0x2D4), as FUN_026697C0
    ctx.r[3] = w + 0x2D4;
    ctx.r[4] = 0;
    sub_0203787C(ctx, base);
    ctx = saved;
    ctx.r[3] = w + 0x2D4;
    ctx.r[4] = 3;
    ctx.r[5] = 0;
    sub_02037600(ctx, base);
    ppc::st32(base, w + 0xC, ppc::ld32(base, w + 0xC) | 0x100000);
    ctx = saved;
    ctx.r[3] = w;
    ctx.r[4] = 1;
    ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, w) + 0xE4));
    run(sub_0265E308, 0, 0, 0);
    run(sub_0265F010, 1, 0, 0);
    run(sub_0265FAE4, 8, 0, 0);
    ppc::st16(base, w + 0x52C, 0);
    // FUN_026611B8(widget, slots, entries, page, pages, mode, flags, &out, 8): the last two on the stack as the game
    // passes them (r10 = &out, 8 at sp+8)
    ctx = saved;
    ctx.r[1] -= 0x40;
    ctx.r[3] = w;
    ctx.r[4] = g_preview_slots;
    ctx.r[5] = g_preview_entries;
    ctx.r[6] = page + 1;
    ctx.r[7] = pages;
    ctx.r[8] = 0;
    ctx.r[9] = 0x800;
    ctx.r[10] = g_preview_entries + 8 * 0x18;
    ppc::st32(base, static_cast<uint32_t>(ctx.r[1]) + 8, 8);
    sub_026611B8(ctx, base);
    ctx = saved;
    // Store Items' widget also has state bits 0x30000 (+0xC; 44930040 there, 44900040 from the steps alone): without
    // them the window is not drawn (seen in a window 2026-10-28)
    ppc::st32(base, w + 0xC, ppc::ld32(base, w + 0xC) | 0x30000);
    g_shown_set = set;
}

void hide_preview(ppc::Context& ctx, uint8_t* base) {
    if (g_shown_set < 0) return;
    g_shown_set = -1;
    if (const uint32_t sel = call(ctx, base, sub_0214218C, ppc::ld32(base, 0x10314578), 0)) ppc::st32(base, sel, 0);
    if (const uint32_t w = pouch_widget(ctx, base)) {  // closed as it was opened: vf+0xE4(widget, 0)
        const ppc::Context saved = ctx;
        ctx.r[3] = w;
        ctx.r[4] = 0;
        ppc::call_indirect(ctx, base, ppc::ld32(base, ppc::ld32(base, w) + 0xE4));
        ctx = saved;
    }
}

// The box menu screen (uIDLobbyMaki::drawBoxMenu? 0x02687D38, r3 = screen, r4 = state) places the sub-list pop-up
// (children 0x22F, 0x1DA, 600) at the chosen option's row: y = base + row slot * row height (constants 0x1005C520..34;
// slot = option, + 1 past option 2), moved up if it would leave the screen. With Item Sets the slots differ (Item
// Sets 3, options 3-5 in slots 5-7): placed again by the same formula.
PPC_FUNC(draw_box_menu) {
    const uint32_t screen = ctx.r[3], state = ctx.r[4];
    sub_02687D38__orig(ctx, base);
    // the set list: the pouch window shows the highlighted set (state +8 = the sub-list cursor)
    if (g_pick != Pick::None && ppc::ld16(base, state + 0xA) == 1) show_preview(ctx, base, static_cast<int16_t>(ppc::ld16(base, state + 8)));
    else if (g_shown_set >= 0) hide_preview(ctx, base);
    if (!g_on || ppc::ld16(base, state + 0xC) != 7) return;
    if (ppc::ld8(base, state + 3) != 0 || static_cast<int16_t>(ppc::ld16(base, state + 0xA)) <= 0) return;
    const uint16_t option = ppc::ld16(base, state + 6);
    if (option < 3 || option > kOption) return;
    const int slot = option == kOption ? 3 : option + 2;
    const float row = ppc::ldf32(base, 0x1005C520), extra = ppc::ldf32(base, 0x1005C524), pad = ppc::ldf32(base, 0x1005C530),
                limit = ppc::ldf32(base, 0x1005C534);
    const float sub_h = static_cast<float>(static_cast<int16_t>(ppc::ld16(base, state + 0xE))) * row + extra;
    const float y_row = static_cast<float>(slot) * row;
    const float y0 = static_cast<float>(static_cast<int32_t>(ppc::ld32(base, screen + 0x106 * 4))) + y_row;
    const float y1 = static_cast<float>(static_cast<int32_t>(ppc::ld32(base, screen + 0x107 * 4))) + y_row;
    const float shift = limit < y0 + sub_h ? limit - (y0 + sub_h) : pad;
    const ppc::Context saved = ctx;
    for (auto [id, y] : {std::pair<uint32_t, float>{0x22F, y0}, {0x1DA, y1}, {600, y1}}) {
        ctx.r[3] = screen;
        ctx.r[4] = id;
        ctx.r[5] = 1;
        sub_02A72CE8(ctx, base);  // the child
        if (!ctx.r[3]) continue;
        ctx.f[1].ps0 = static_cast<double>(y + shift);
        sub_02ADEF64(ctx, base);  // its y
    }
    ctx = saved;
}

// MH3U_ITEMSET_PROBE=<seconds>: once, that long after start, where the pouch the save file holds (user1: hunter data
// from 0x28, blademaster pouch +0xA8) is in guest memory, against sUserData+0x38 (finding the pouch's address).
void probe(std::filesystem::path save_dir, int seconds) {
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    core::GuestMemory* gm = core::GuestMemory::current();
    if (!gm) return;
    uint8_t* base = gm->base();
    std::ifstream f(save_dir / "user1", std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (b.size() < 0x28 + 0xA8 + 12) return;
    const uint32_t ud = ppc::ld32(base, kSUserData);
    std::printf("item sets probe: sUserData %08X, hunter (+0x38) bytes:", ud);
    for (uint32_t i = 0; ud && i < 16; ++i) std::printf(" %02X", ppc::ld8(base, ud + 0x38 + i));
    std::printf("\n  save file hunter bytes:");
    for (uint32_t i = 0; i < 16; ++i) std::printf(" %02X", b[0x28 + i]);
    std::printf("\n  save file pouch:");
    for (uint32_t i = 0; i < 12; ++i) std::printf(" %02X", b[0x28 + 0xA8 + i]);
    std::printf("\n");
    for (uint32_t at : gm->find(&b[0x28 + 0xA8], 12, 0x10000000, 0x50000000))
        std::printf("  pouch pattern at %08X = sUserData%+d, hunter%+d\n", at, static_cast<int>(at - ud),
                    static_cast<int>(at - (ud + 0x38)));
    for (uint32_t at : gm->find(&b[0x28 + 1], 7, 0x10000000, 0x50000000))  // the name
        std::printf("  name at %08X = sUserData%+d\n", at, static_cast<int>(at - ud));
}

}  // namespace

void register_item_sets(const std::filesystem::path& save_dir, std::function<void(const std::string&)> notify_fn) {
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_ITEMSET_PROBE") == 0 && v) {
        std::thread(probe, save_dir, std::atoi(v)).detach();
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_DUMP_GUI") == 0 && v) {
        g_dump_gui = true;
        std::free(v);
    }
    if (char* v = nullptr; _dupenv_s(&v, nullptr, "MH3U_GUI_TEST") == 0 && v) {
        g_gui_test = true;
        std::free(v);
    }
    g_file = save_dir / "cha-cha-item-sets.ini";
    g_notify = std::move(notify_fn);
    g_on = true;
    set_text(0x2E, 0x185 + kOption, "Item Sets");
    set_text(0x2E, kLabelBase, "Register Item Set");
    set_text(0x2E, kLabelBase + 1, "Load Item Set");
    set_text(0x2F, kHelpBase, "Save your pouch as an item set and\r\nrefill it from the Item Box in one go.", kHelpCaller);
    menu_texts();
    ppc::set_override(0x021F0E7C, &box_menu_step);
    ppc::set_override(0x026A6B80, &draw_options);
    ppc::set_override(0x02687D38, &draw_box_menu);
    ppc::set_override(0x02AE1E40, &update_transforms);
}

void set_frame_capture(std::function<void()> capture) { g_capture = std::move(capture); }

}  // namespace mh3u::game
