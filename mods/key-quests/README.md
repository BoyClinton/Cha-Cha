# Key Quests

Marks the quests that move the story forward on the Quest Counter's list:

| Tag | Meaning |
|---|---|
| gold **key badge** | Needed to unlock the next urgent quest |
| **2 OF 5** (blue) | Any 2 of the quests tagged like this will do (the game needs only some of a group) |
| **URGENT** (red) | The urgent quest itself |

It covers Moga Village (1–9★) and Port Tanzia (low and high rank).

- **Optional:** the mod starts **off**. Switch it on in **MODS → Mods → Key Quests**. The settings there turn each kind
  of tag on or off and move the mark from the quest name's box to the left of the list.
- **Needs:** Cha-Cha with mod support and MH3U US with update v32.

## Where the list comes from

The game's own quest data (412 quests):
- **Unlock fields:** every quest has *open* bits it needs before it's offered, *source* bits its clear sets, and a
  count of how many clears are needed.
- **Key quests:** the urgent quests (2xxx village, 12xxx Port Tanzia) need bits that ordinary quests set. Following
  that chain backwards gives the key quests, plus the game's own key flag on village 1–5★ quests.
- **Checked:** the result matches the community wiki's 1–7★ village key quests.
- **Two exceptions:** *Walk 'n' Roll* and *The Desert Gourmand* (5★) are taken from the wiki. The progress bit they
  lead to is set by game code, not by quest data.

Not done yet: dimming key quests you've already cleared. Details are in `docs/research-log.md` (2026-11-04).
