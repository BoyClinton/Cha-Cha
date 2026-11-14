# Mods

The main menu's **DLC** screen is Cha-Cha's add-on menu. It still looks and sounds like the game's own DLC screen
(Capcom's download server is gone, so Cha-Cha answers the game's downloads itself; nothing goes online). Its three
buttons:

In the Cha-Cha Plus fork, **F2** also opens the overlay's Mods page over the game: the plugin mods in
`<data folder>/mods` (content mods and plugins, [modding.md](modding.md)) with their settings.

| Button | What it opens | Folder |
|---|---|---|
| **Official** | the game's own DLC menus (Content Previews, Quests, Extras) with Capcom's event and challenge quests | `<data>/bonus` |
| **Quests** | the game's Quests screen with your custom quests | `<data>/custom` |
| **Mods** | a list of Cha-Cha's mods with a tick each: ticked = on, X saves | `<data>/mods` |

`<data>` is `%APPDATA%\Cha-Cha` for a release (or the `data` folder of a portable install) and the checkout itself
when running from source. The folders are made at start.

## Official and Quests: event and challenge quests

Put quests in `bonus` (Capcom's) or `custom` (yours), as:

- `q_<number>.quest` files: one quest each (QTDS data, at most 0x2000 bytes); subfolders are fine;
- `quest1` ... `quest4` copied from a Wii U save: every quest in them is offered.

Then open DLC > Official (or Quests) > Quests > Event Quests or Challenge Quests, tick the quests and choose
Download. The game's own download screen fills its bar, and the quests land in your save's quest slots
(`quest1`-`quest3` event, `quest4` challenge; 20 slots each), so they appear at the **Port Tanzia Gathering Hall**.
Quests you have already appear as downloaded.

The list shows what each quest file holds: title, goal, failure, client, summary, quest level, locale, time limit,
reward and fee, and the target monsters (names and icons in the game's own words).

### Quest numbers

The game's DLC lists show only two number ranges, and Cha-Cha lists exactly what the game will take:

| Numbers | Kind | Stored in |
|---|---|---|
| `q_20000` - `q_20999` | event quest | `quest1`-`quest3` |
| `q_61000` - `q_61999` | challenge quest (arena) | `quest4` |

A quest file numbered outside these ranges is skipped, and the log says so (`dlc: q_25001.quest skipped`). The number
is also written inside the quest data, so renaming the file does not change it: a custom quest needs a number in a
range above when it is made. Capcom's quests use 20101-20399 and 61000-61009, so custom event quests are safest from
20500 up; 20990-20999 are Cha-Cha's own (the Mods list's rows) and are skipped in quest folders.

Quests from the old `<data>/quests` folder (before 2026-11-05) still show: its official packs under Official, the rest
under Quests. Quests are no longer put into the save at start; they come through Download.

## Mods

DLC > Mods lists the gameplay mods, each with a tick: tick or untick with A (or a click); each change is saved at once, no Save
button. The choice is kept in `<data>/mods/mods.ini` (`quest_scaling=1`, `auto_carve=1`, ...) and applies at once.
Every mod is also in the pause menu's CFG tab > Mods (On / Off), on quests too: both lists come from the one table in
`src/game/mods.cpp`, so a new mod is in both by itself (user rule 2026-11-09).

- **Quest Scaling**: monster HP on Port Tanzia quests follows the number of hunters (65 % alone, 80 % for two, 90 %
  for three, full for four; the page's Solo / Duo / Trio HP).
- **Auto Carve**: hold the gather button to keep carving, gathering and mining.
- **Texture Packs**: textures from `<data>/texture_replacements` in place of the game's (`docs/texture-packs.md`).
- **Modern Camera** - no longer a mod since 2026-11-10: it is in the pause menu's CFG tab > **Camera Settings**,
  with **Camera Distance** (issue #17: Default, 110% ... 200% of the game's own distance; both saved in config.ini).
  On quests and in the field the camera is a free orbit camera, as in later
  games. The mouse moves it directly (Mouse Sensitivity and Invert Camera Y in Keyboard Settings), the right stick
  turns and tilts it at the same time, and after a short pause it drifts back behind you while you run. Its heights
  and distances follow the game's own six camera heights, smoothly, from a low view looking up to a high one (up to
  60 degrees). L resets and targets as before; the game's own Camera options (reverse left / right, up / down) still
  apply. Under water the classic camera stays. Code: `src/game/modern_camera.cpp`.
- **Instant Gathering** (off by default): one gathering, mining or bug net animation collects everything the spot
  has left. Each item still rolls its reward, the gathering skills and the tool's breakage as in the game; a broken
  tool, a full pouch or the spot running out ends it there. Endless spots give one item a swing, as usual. Carving
  monsters is Instant Carve. Code: `src/game/carving.cpp`.
- **Instant Carve** (off by default): one carving motion takes all the carves left on a monster, on land and under
  water. Each carve still rolls its own reward. Code: `src/game/carving.cpp` (a carve is a spot tied to a monster).
- **Auto Delivery** (off by default): a quest's item delivery goals are handed in on the spot (the game's own
  "delivered" message) as soon as your pouch holds the whole remaining amount - no trip to the delivery box. Items
  carried in your arms (eggs and the like; the game's own list of carried items) still go to camp. Not while a
  menu is open.
  Code: `src/game/delivery.cpp`.
- **Key Quest Markers** (off by default): in the Moga Village and Port Tanzia quest lists, key quests get a gold star
  and urgent quests a red-orange one, with the game's own highlight frame around the row in gold (75 %), while you
  still need them to progress; once cleared, star and frame turn grey. Key and urgent quests are the ones the game's
  own unlock checks wait for; quests that are only one choice of several get nothing. Code: `src/game/quest_markers.cpp`.
- **Texture Dump** is not a row but the list's button bar, **X Texture Dump** (Y or a click on it does the same).
  Say Yes, and the game's download screen shows the dump of every texture of the game going (its bar and sounds);
  Cancel download (B or a click) stops it. Files go to `<data>/texture_dump` as
  `tex_<w>x<h>_<CRC32>.dds`, ready to make texture packs from (`docs/texture-packs.md`). Files already there are kept.

Everything here works with the menu pointer: hovering moves the cursor, a click chooses, the L / R page arrows
turn pages (only shown with more rows than a page holds: five), and a click closes the game's notices.

Quest files are game data: they stay out of git (`bonus/`, `custom/` and `quests/` are ignored), and Cha-Cha ships
none. Code: `src/game/dlc_menu.cpp` (the menu), `runtime/boot/dlc_content.cpp` (the lists and files),
`runtime/os/dlc_server.cpp` (the download protocol), `src/game/mods.h` (the switches).

## Built-in gameplay changes

Item Sets is always on. Hold to gather is the **Auto Carve** mod and monster HP scaling the **Quest Scaling** mod:
both are on by default and switched in DLC > Mods (user decision 2026-11-05).

### Item Sets

Item Sets adds a seventh option, **Item Sets**, to the item box menu in your house, as in MHGU.
Choosing it opens a sub-list:

- **Register Item Set** and **Load Item Set** each open the list of **5 sets**, labelled with their first item, for
  example "Set 1: Blue Mushroom" or "Set 2 (empty)". The help box previews the highlighted set's items, and B goes
  back.
- Registering saves the items in your pouch, slot by slot, as that set.
- Loading puts everything in your pouch into the Item Box, on top of stacks of the same item first. Then it takes the
  set's items out of the box into the same pouch slots. A toast says how many items the box was short.

Each pouch has its own 5 sets: the Blademaster's Pouch (24 slots) and the Gunner's Pouch (32 slots), whichever your
weapon uses. Sets are kept per hunter name in `<data folder>/save/80000001/cha-cha-item-sets.ini`
(`<name>/<pouch>#<set>=item:count,...`). The pouch and the
box only change in memory: they reach your save when the game saves.

How it works: the box menu's option tables have an unused seventh slot, and the game's text has unused "DUMMY"
entries. Cha-Cha fills them and handles the new option in `src/game/item_sets.cpp` (details in the research log,
"Item box menu").

In the set list a pouch window shows the highlighted set: for Load, what you would end up with from what your pouch
and box hold; for Register, the set as stored. Left / right on the d-pad turn its pages.

### Hold to gather

Hold the gather button (Wii U A) on a carcass, a gathering spot or a mining point: once the first item comes in, the
button keeps repeating for you, as if you were mashing it, until you let go or about 3 seconds pass without another
item (the spot is used up). It works for every gatherable. Code: `src/game/carving.cpp` (the game's obtain step,
0x020ECA2C) and `input::repeat_held` in `runtime/input/host_input.cpp`.

### In development

- **Monster HP scaling**: monster health follows the number of hunters on the quest (1-4). Optional: Settings →
  General → Monster HP scaling (`monster_hp_scaling=0` in config.ini switches it off).

