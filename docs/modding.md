# Modding Cha-Cha

Mods go in **`<data folder>/mods`**, one folder per mod. **MODS → Mods** (F2 in game) lists them, switches each on or
off, and shows the settings of mods that have any. A mod switched on or off takes effect on the next start: content
and plugins load once, before the game boots.

```
mods/
  damage-numbers/
    mod.ini                 name, author, version, description (all optional)
    plugins/
      damage_numbers.so     native plugins: .so on Linux, .dll on Windows
    fonts/damage.ttf        whatever the plugin uses (optional)
    settings.ini            written on "Save settings" (end of the Mods page: on_save)
  hd-monsters/
    mod.ini
    content/                files over the game's content folder
      nativeCafe/arc/...
```

A folder with none of `mod.ini`, `content/` or `plugins/` is not listed. On/off states are kept in
`<data folder>/mods.ini` as `mod.<folder>=on|off`, next to the quest packs' `pack.` lines. New mods start on,
unless their `mod.ini` says `default = off` (optional mods: True Attack, Key Quests, Charm Table Fix).

## Content mods (file replacement)

Everything in a mod's `content/` lies over the game's `/vol/content`, file by file: a file at the same path replaces
the game's, and a new path adds a file. Nothing is copied or changed in your dump. Paths are those of the game's
`content` folder (`nativeCafe/arc/...`). They are case-sensitive on Linux, so match the game's spelling.

When several mods have the same file, the mod whose folder name sorts first wins. Texture packs are separate
(`texture_replacements`, [texture-packs.md](texture-packs.md)) and switch on and off while you play.

## Plugins (native code)

A plugin is a shared library that exports one C function:

```c
#include "chacha_plugin.h"

CHACHA_EXPORT int chacha_plugin_init(const ChaChaApi* api, ChaChaPlugin* self) {
    self->name = "My Mod";
    self->on_hud = &draw;            /* draw over the game */
    self->on_settings = &settings;   /* rows in MODS -> Mods */
    api->hook(self, 0x025CB37C, &before, &after, NULL);
    return CHACHA_API_VERSION;       /* 0 = stay unloaded */
}
```

The whole API is in [`include/chacha_plugin.h`](../include/chacha_plugin.h). It is plain C, and a plugin links
against nothing of Cha-Cha's, so a plugin built once keeps working with later Cha-Cha builds of the same API version.

| Area | What a plugin gets |
|---|---|
| Game memory | `guest_base` and big-endian helpers (`chacha_ld32`, `chacha_ldf`...), `readable()` before touching an address |
| Hooks | `hook(addr, before, after)` on a game function: arguments in, results out, skip the original |
| Camera | `camera()`, `world_to_screen()` through the game's live camera, `game_rect()` |
| HUD | `on_hud`: `draw_text` (outline, slant, alignment), lines, rectangles, circles, polygons; `load_font` |
| Settings | `on_settings`: `ui_group`, `ui_switch`, `ui_choice`, `ui_float`, `ui_button`, `ui_info` (the launcher's rows, controller-friendly) |
| Storage | `config_get` / `config_set` / `config_save` → `settings.ini` in the mod's folder; `on_save` / `on_defaults` for the Mods page's Save settings / Restore defaults rows (check `chacha_has_save_hooks(api)` first) |
| Misc | `log`, `toast`, `time`, `mod_dir`, `data_dir`, `game_sha1` |

Check `api->game_sha1` before using any address: addresses are for MH3U US update v32
(`d97aabfc5a45362f88dfacb09f23c6dcae6faac2`).

**Threads.** Hooks run on the game's threads (up to three at once). `on_hud` and `on_settings` run on Cha-Cha's
main thread. Pass data between them under a lock.

### Hook points

Cha-Cha translates the game's code to C++ ahead of time, so a function can only be hooked where the build left a
hook point: every function in [`config/overrides.txt`](../config/overrides.txt). `hook()` returns 0 for any other
address. A mod that needs a new hook point adds a line there, `0xADDR name # why`, and Cha-Cha is rebuilt (one line;
the function then dispatches through a slot). Several plugins, and Cha-Cha's own replacements, can share a hook point:
they chain in load order.

| Address | Function | Arguments |
|---|---|---|
| `0x025CB37C` | `uEnemy::damageCheck?`: once a frame per monster; takes the frame's damage off its HP | r3 = uEnemy |
| `0x0227EDE8` | `uGameCamera::move?`: the game camera's per-frame update (Cha-Cha uses it for `world_to_screen`) | r3 = uGameCamera |
| `0x0259FB84` | `uEnemy::setMaxHp?` | r3 = uEnemy, r4 = base HP |
| `0x020ECA2C` | `sCockpit::obtainItem?` | |
| `0x02045108` | `itemDisplayAttack?`: a weapon's attack as menus show it (raw x class multiplier / 100) | r3 = item, r4 = raw attack; returns the shown value |
| `0x021E3978` | `sLobby::questListStep?`: the Quest Counter's quest list, once a frame while open | r4 = list: count +0x18, row +0x16, quest numbers u16 +0x1A[8] |
| `0x0217F63C` | `makeTalisman?`: rolls one talisman; every draw from RNG slot 1 (`*(0x101FDB9C) + 0x42`) | r4 = talisman out (+1 slots, +4/+5 skill 1, +6/+7 skill 2), r5 = charm 0x25-0x28 |

Every other entry in `config/overrides.txt` works too.

### Building a plugin

With Cha-Cha's source, add the mod under `mods/<name>/` and one line to `mods/CMakeLists.txt`:

```cmake
chacha_mod_plugin(my-mod my_mod my-mod/src/my_mod.cpp)
```

The build writes a ready folder to `<build>/mods/<name>/` (`plugins/` and `mod.ini`). Outside Cha-Cha's tree, any
compiler works: build a shared library from your source and `include/chacha_plugin.h`, exporting
`chacha_plugin_init`. A Linux example: `g++ -std=c++20 -shared -fPIC -fvisibility=hidden -Iinclude mod.cpp -o mod.so`.

## Example: Damage Numbers

`mods/damage-numbers` is a complete mod: the damage of every hit shows over the monster in the Wilds, Rise or Classic
style, with its settings in MODS → Mods. See [its README](../mods/damage-numbers/README.md).
