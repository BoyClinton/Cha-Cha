#include "mods.h"

#include <atomic>

namespace mh3u::game {
namespace {

constexpr int kCount = static_cast<int>(Mod::Count);

const ModInfo kInfo[kCount] = {
    {"quest_scaling", "Quest Scaling",
     {"Monster HP follows the number", "of hunters on a Port Tanzia quest."},
     "Large monsters on Port Tanzia\nquests get the health for the\nhunters on the quest, as in\nlater games: 65% alone, 80% for\ntwo, 90% for three, full health\nfor four.", true},
    {"auto_carve", "Auto Carve",
     {"Hold the button to keep carving,", "gathering and mining."},
     "Hold the gather button on a\ncarcass, a gathering spot or a\nmining point: it keeps going\nuntil you let go or the spot is\nused up.", true},
    {"texture_packs", "Texture Packs",
     {"Replacement textures from the", "texture_replacements folder."},
     "Textures in the\ntexture_replacements folder (in\nCha-Cha's data folder) are used\nin place of the game's own. Turn\nit off to see the original ones.", true},
    {"instant_gathering", "Instant Gathering",
     {"One gather, mine or net swing", "takes all a spot has to offer."},
     "One gathering, mining or bug net\nanimation collects everything\nthe spot has left. Each item\nstill rolls its reward and the\ntool's breakage as usual; a\nbroken tool stops it there.\nCarving is Instant Carve.", false},
    {"instant_carve", "Instant Carve",
     {"One carve takes every carve", "a monster has left."},
     "One carving motion takes all\nthe carves left on a monster,\non land and under water. Each\ncarve still rolls its own\nreward, as usual.", false},
    {"auto_delivery", "Auto Delivery",
     {"Delivery goals are handed in as", "soon as your pouch holds enough."},
     "On gathering quests the items\nasked for are delivered on the\nspot as soon as your pouch holds\nenough of them - no trip to the\ndelivery box. Items you carry in\nyour arms, like eggs, still go\nto camp as usual.", false},
    {"key_quest_markers", "Key Quest Markers",
     {"A star on key and urgent quests,", "coloured by what you still need."},
     "In the village and Port Tanzia\nquest lists, key quests get a\ngold star and urgent quests a\nred one while you still need\nthem to progress; cleared ones\nturn grey. Quests that are one\nchoice of several get no star.", false},
};

std::atomic<bool> g_on[kCount] = {kInfo[0].default_on, kInfo[1].default_on, kInfo[2].default_on, kInfo[3].default_on,
                                  kInfo[4].default_on, kInfo[5].default_on, kInfo[6].default_on};
static_assert(kCount == 7, "g_on: one default per mod");
std::function<void(Mod, bool)> g_listener;

}  // namespace

const ModInfo& mod_info(Mod m) { return kInfo[static_cast<int>(m)]; }
bool mod_enabled(Mod m) { return g_on[static_cast<int>(m)].load(std::memory_order_relaxed); }
void set_mod_enabled(Mod m, bool on) {
    g_on[static_cast<int>(m)].store(on, std::memory_order_relaxed);
    if (g_listener) g_listener(m, on);
}
void set_mod_listener(std::function<void(Mod, bool)> listener) { g_listener = std::move(listener); }

}  // namespace mh3u::game
