# Damage Numbers

Shows the damage of every hit on a monster over the monster. Choose the Wilds, Rise or Classic style.

## Requirements

- Cha-Cha with mod support (MODS → Mods tab), Monster Hunter 3 Ultimate US with update v32.

## Installation

1. Close Cha-Cha.
2. Copy the whole `damage-numbers` folder (with `mod.ini` and `plugins/`) into `<data folder>/mods/`:
   - Windows: `%APPDATA%\Cha-Cha\mods\`
   - Linux: `~/.local/share/Cha-Cha/mods/`
   - Portable: `data/mods/` next to the program
3. Start Cha-Cha. **MODS → Mods** lists *Damage Numbers* with its plugin as *Running*.

`plugins/damage_numbers.so` is the Linux plugin, `plugins/damage_numbers.dll` the Windows one.

## Usage

Numbers appear for every hit that takes HP off a monster, large or small: weapons, shots, Felyne attacks, bombs.
Each part hit in a frame gets its own number, physical and element damage together.

| Style | Look |
|---|---|
| Wilds | outlined white digits that pop in and drift up |
| Rise | slanted warm digits with a slash behind them, flicked up and to the right |
| Classic | small plain digits rising straight up |

Sizes follow the height of the game picture.

## Settings

In **MODS → Mods → Damage Numbers** (F2 in game):

| Setting | Default | Effect |
|---|---|---|
| Show damage numbers | On | Turns the numbers on or off. |
| Show other damage | Off | Also shows HP lost that no hit on a part accounts for (the game's separate damage value, poison and the like), in grey. |
| Style | Wilds | Wilds, Rise or Classic. |
| Number size | 1.0x | 0.5x to 3.0x. |
| Duration | 0.9 s | 0.4 to 2.0 seconds. |

**Save settings** at the end of the Mods page keeps them for the next start, in `settings.ini` in the mod's folder.
**Restore defaults** there resets them (for every mod); press Save settings afterward to keep the defaults.

`settings.ini` can be edited while Cha-Cha is closed:

```ini
enabled = true
other = false
style = wilds
scale = 1.00
lifetime = 0.90
```

`style` is `wilds`, `rise` or `classic`. Missing or invalid values fall back to their defaults.

**Font:** the numbers use a bold system font. To use your own, put it at `fonts/damage.ttf` in the mod's folder.

## Known limits

- Numbers sit at the middle of the monster's body (its model bounds), spread a little sideways, not at the exact
  point of impact.
- Weak-point (hitzone) colouring and critical-hit marks, as in the MHGU mod, are not there yet: the game's hit
  details are not mapped yet. Every number is white; "other" damage is grey.
- Numbers show on the TV picture only, not on the GamePad screen.

## How it works

The plugin hooks the monster's per-frame damage check (`0x025CB37C`). Before it runs, the plugin reads the damage
each of the monster's 8 parts took that frame (`state+0x8EC`). Afterwards it checks that the HP (`state+0x8B8`)
really went down. Positions come from the monster's coordinates and bounds, projected through the game's camera
(`world_to_screen`). Details are in `docs/research-log.md` (2026-11-02).
