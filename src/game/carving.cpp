// Hold to gather: keep the button held to carve / gather / mine again and again until the spot is used up (every
// gatherable, not only carving; always on, no MODS switch - user 2026-10-28).
//
// Replaces: FUN_020ECA2C sCockpit::obtainItem? (cockpit, player, item, ..., flags, count, ...), the one step every
// carve, gathering spot and mining point goes through in a quest (seen 2026-10-28: a carve, gathers and mining, all
// from it): it adds the item (FUN_0285ECD4 in a quest, the sUserData pouch elsewhere) and shows "%s obtained."
// (FUN_020EF0A8 kind 2 text 8) when the add returns < 2. Status: wip - the original runs unchanged; after an item is
// obtained the input layer keeps "pressing" the held button (input::repeat_held), as if the player mashed it, until
// a few seconds pass without another item.
#include "carving.h"

#include <utility>

#include "functions.h"

namespace mh3u::game {
namespace {

std::function<void()> g_on_obtain;

PPC_FUNC(obtain_item) {
    sub_020ECA2C__orig(ctx, base);
    if (ctx.r[3] < 2 && g_on_obtain) g_on_obtain();  // obtained (the game's own test before "%s obtained.")
}

}  // namespace

void register_carving(std::function<void()> on_obtain) {
    g_on_obtain = std::move(on_obtain);
    ppc::set_override(0x020ECA2C, &obtain_item);
}

}  // namespace mh3u::game
