// Monster maximum HP (monster_hp.cpp): logged for every monster that spawns; scaling by hunters to follow.
#pragma once

namespace mh3u::game {

void register_monster_hp(bool scaling);
// Monster HP scaling on / off (Settings -> General -> Monster HP scaling): for monsters that spawn from then on.
void set_monster_hp_scaling(bool on);

}  // namespace mh3u::game
