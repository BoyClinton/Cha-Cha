# Mods

**MODS** is on the title menu, and **F2** opens it over the game. F1 switches to the settings, and F2 again or Esc / B
closes it. It has one tab, QUESTS; the gameplay changes below are always on.

## QUESTS: event and challenge quests

MH3U got its event and challenge quests as free downloads from Capcom's server (`goshawk.capcom.co.jp`). That server
is gone. The game keeps downloaded quests in its save, in four files:

| Save file | Holds | Slots |
|---|---|---|
| `quest1`, `quest2`, `quest3` | event quests (`q_2xxxx`) | 20 each |
| `quest4` | challenge quests (`q_61xxx`) | 20 |

Each slot holds one quest: its `.quest` data (`QTDS`, at most 0x2000 bytes), followed by its 16-byte file name. The game
creates the files, empty, together with the save.

Cha-Cha installs quests into those slots from the **quest folder** `<data folder>/quests`. That's
`%APPDATA%\Cha-Cha\quests` for a release and `quests\` in a source checkout.

### Packs

Each top-level folder in the quest folder is a **pack**. Files directly in the quest folder are **single quests**.

```
quests/
  DLC MH3U EU/            official pack: quest1-quest4 copied from a Wii U save
    quest1  quest2  quest4
  Frontier Pack/          custom pack
    pack.json             {"name": "Frontier Pack", "author": "...", "description": "..."}
    cover.png             the pack's picture (or pack.png)
    q_25001.quest         a quest
    q_25001.png           its picture (.png, .jpg, .webp or .bmp)
    q_25001.txt           its description
  q_29999.quest           a single quest
```

- **Quest files.** A quest file is `q_<number>.quest`, holding SDTQ quest data of at most 0x2000 bytes. Numbers
  60000-69999 are challenge quests, and everything else is an event quest. Subfolders inside a pack are fine.
- **Official packs.** A pack whose quests come from a Wii U save's `quest1`-`quest4` files (for example a
  console save backup) is Capcom's DLC. It's marked official and shows no pictures.

Every pack has a mode, chosen on its row:

- **All**: every quest of the pack is kept installed, including quests added to the folder later. This is the default
  for official packs, so dropping the DLC files in is enough.
- **Choose**: the pack's quests are listed under it, to install or remove one by one. This is the default for custom
  packs.
- **Off**: none of the pack's quests are installed.

Modes are saved in `<data folder>/mods.ini` (`pack.<folder>=all|choose|off`). They're applied when Cha-Cha starts and
whenever the QUESTS tab is opened or changed.

### The QUESTS tab

- **QUEST FOLDER** opens the folder, and **REFRESH** reads it again.
- **INSTALLED** counts the slots in use.
- **PACKS** lists every pack with its mode, then the **SINGLE QUESTS**, and **ONLY IN YOUR SAVE** lists installed
  quests that aren't in the folder, each with **Remove**.
- The selected quest's or pack's text and picture show on the right.

Installed quests appear at the **Port Tanzia Gathering Hall**, not in Moga Village.

**While the game runs**, the game reads its quest files into memory once. To make a change show there without
reloading, Cha-Cha remembers where the game read each save file to (`runtime/os/save_reads.h`). When a slot changes, it
also writes the new slot into that copy, provided the copy still holds the old bytes. If the game keeps the list
somewhere else, the toast says to reload the save. The log shows `save: game read questN ...` when the game loads a
file, and `quests: questN slot ...: game copies N, updated N` for each change.

Quest files are game data: they stay out of git (`quests/` is ignored), and Cha-Cha ships none.

Code: `runtime/boot/quests.*` (scan, packs, install, remove; `quests_test`), `runtime/boot/launcher_image.*` (pictures),
and the QUESTS tab in `runtime/boot/setup.cpp`.

## Built-in gameplay changes

These are always on; the MODS panel has no switches for them (user decision 2026-10-28).

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

- **Monster HP scaling**: monster health follows the number of hunters on the quest (1-4).

