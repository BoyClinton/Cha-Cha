<p align="center">
  <img src="docs/images/cha-cha.png" width="128" alt="Cha-Cha">
</p>

<h1 align="center">Cha-Cha</h1>

<p align="center">
  <b>Monster Hunter 3 Ultimate, running natively on PC.</b><br>
  A static recompilation of the Wii U game for Windows and Linux, with an experimental Android build.
</p>

<p align="center">
  <a href="../../releases/latest"><img src="https://img.shields.io/github/v/release/SilversRayGA/Cha-Cha-modsupport?label=release&color=ecb254" alt="Latest release"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-3b332a" alt="Windows and Linux">
  <img src="https://img.shields.io/badge/graphics-Vulkan-3b332a" alt="Vulkan">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-3b332a" alt="GPL-3.0"></a>
</p>

<p align="center">
  <a href="#how-to-install">Install</a> ·
  <a href="#getting-started">Getting started</a> ·
  <a href="#whats-new-compared-to-the-original">What's new</a> ·
  <a href="#the-dlc-screen-quests-and-mods">DLC and mods</a> ·
  <a href="#mods">Plugin mods</a> ·
  <a href="#texture-packs">Texture packs</a> ·
  <a href="#online-play-very-experimental">Online</a> ·
  <a href="#whats-next">What's next</a> ·
  <a href="#building-from-source">Building</a>
</p>

---

Cha-Cha translates the game's PowerPC code into C++ ahead of time and compiles it for your PC. It runs on our own
reimplementation of the Wii U system libraries it uses, with graphics on Vulkan and input and audio on SDL3. It is
not an emulator, and there is no emulator code in it.

> [!IMPORTANT]
> **No game files are included.** You need your own copy of **Monster Hunter 3 Ultimate (US) with update v32**.
> Cha-Cha is a fan project, not affiliated with or endorsed by Capcom or Nintendo.

## How to install

You need your own **Monster Hunter 3 Ultimate (US) with update v32** (a `.wua` file, or the game and update folders).

**Windows**
1. Download `Cha-Cha-<version>-windows-x64.zip` from [Releases](../../releases/latest).
2. Extract it to a folder you can write to (not Program Files).
3. Put your dump in a folder named `game` next to `Cha-Cha.exe` (details in [Getting started](#getting-started)), then run `Cha-Cha.exe`.

**Linux / Steam Deck**
1. Download `Cha-Cha-<version>-x86_64.AppImage` (or the folder `Cha-Cha-<version>-linux-x86_64.tar.gz`) from [Releases](../../releases/latest).
2. Make it executable: `chmod +x Cha-Cha-*.AppImage` (or Properties → Allow executing as program).
3. Put your dump in a `game` folder in the data folder (or start once with `--install <path>`), then run it.

The first start translates the game's shaders (a few seconds, once), then the game starts by itself. Cha-Cha's own
settings are inside the game's menus (Options → Game Options, and the in-game menu's CFG tab). Cha-Cha Plus adds an
overlay on top: **F1** or **Select + Options** on a controller opens its settings (camera speed, aspect ratio,
online), **F2** its Mods page for plugin mods.

**Adding mods:** put each mod's folder into the `mods` folder of your data folder (`data\mods` next to
`Cha-Cha.exe` on Windows, `~/.local/share/Cha-Cha/mods` on Linux) and restart Cha-Cha.

<p align="center">
  <img src="docs/images/0.2.0/graphics-settings.jpg" width="100%" alt="Cha-Cha's Graphics Settings page inside the game's own Game Options">
</p>

> [!TIP]
> **New in 0.2.0:** no launcher any more, every setting inside the game's own menus, the Modern Camera and Camera
> Distance, switchable gameplay mods, the DLC screen working again, and a first Android build.
> [Release notes](../../releases/tag/v0.2.0).

## What's new compared to the original

<table>
  <tr>
    <td width="33%" valign="top">
      <h3>Item Sets</h3>
      Loadouts as in MH Generations Ultimate, inside the game's own item box menu (the house box and the small
      boxes such as the Port Tanzia dock).
      <ul>
        <li><b>Register Item Set</b> saves your pouch, <b>Load Item Set</b> refills it.</li>
        <li>5 sets each for Blademaster and Gunner.</li>
        <li>Loading takes only what your pouch and box really hold, and tells you what was missing.</li>
        <li>The game's pouch window previews the result while you choose; left / right turn its pages.</li>
      </ul>
    </td>
    <td width="33%" valign="top">
      <h3>Monster HP scaling</h3>
      Port Tanzia quests are balanced for four hunters. With <b>Quest Scaling</b> on (the default), monsters there
      have:
      <table>
        <tr><th>Hunters</th><th>HP</th></tr>
        <tr><td>1</td><td>65%</td></tr>
        <tr><td>2</td><td>80%</td></tr>
        <tr><td>3</td><td>90%</td></tr>
        <tr><td>4</td><td>100%</td></tr>
      </table>
      At every rank: Low, High and G. Moga Village quests are unchanged. The <b>Quest Scaling</b> mod (DLC →
      Mods or CFG → Mods) switches it off (the game's own HP).
    </td>
    <td width="33%" valign="top">
      <h3>A camera for PC</h3>
      The <b>Modern Camera</b> orbits freely on quests: the mouse moves it directly, the stick turns and tilts at
      once, and it drifts back behind you as you run. <b>Camera Distance</b> pulls it back up to 200 % so big
      monsters stay on screen.
      <h3>DLC quests again</h3>
      The event and challenge quests, once free downloads from a server that is long gone, come in through the game's
      own DLC screen. <a href="#the-dlc-screen-quests-and-mods">See below.</a>
    </td>
  </tr>
</table>

### Gameplay mods, each one a switch

| Mod | What it does | Default |
|---|---|---|
| **Quest Scaling** | Monster HP on Port Tanzia quests follows the number of hunters (table above). | On |
| **Auto Carve** | Hold the gather button on a carcass, a gathering spot or a mining point and you keep going until the spot is used up. | On |
| **Texture Packs** | Your replacement textures, [see below](#texture-packs). | On |
| **Instant Gathering** | One gather, mine or bug-net swing takes everything a spot has left; each item still rolls its reward and tool breakage. Carving is Instant Carve. | Off |
| **Instant Carve** | One carving motion takes every carve a monster has left, on land and under water; each carve still rolls its own reward. | Off |
| **Auto Delivery** | Delivery goals are handed in as soon as your pouch holds enough (eggs and other carried items still go to camp). | Off |
| **Key Quest Markers** | A star on key and urgent quests: gold while you still need them, grey once cleared. | Off |

Switch them in the main menu's **DLC → Mods** or in the in-game menu's **CFG → Mods**, also on a quest.

### Made for PC

- **Settings inside the game, no launcher.** Game Options gains **Display Settings** and **Graphics Settings**,
  **Controller Settings** remaps your controller and **Keyboard Settings** your keys and mouse. The in-game menu's new
  **CFG** tab holds Display, Graphics and Camera Settings and the mods, changed on the spot
  ([how](#change-settings-while-you-play-the-cfg-tab)). Every page works with a controller, the keyboard or the
  mouse, and says what each setting does.
- **Your resolution and frame rate:** internal resolution from 0.5× to 4× (up to 5120×2880), a frame rate limit up
  to unlimited, V-sync, windowed / borderless / fullscreen, smooth or sharp scaling to the window.
- **Tri look:** the softer, muted, hazier picture of Monster Hunter Tri on the Wii, applied to the world only (menus
  and the HUD stay as they are).
- **Effects the original never had:** global illumination and screen-space reflections on water (each with a Lite mode for
  weaker GPUs), ambient occlusion, an enhanced depth of field and a vignette, each optional. The game's own bloom and
  depth of field can be switched off.
- **Skippable cutscenes:** press **+** (Start / Options, or the keyboard key bound to +) to skip a cutscene movie.
- **Wider pictures:** 21:9 and 32:9 aspect ratios (or the window's own shape) with the HUD kept in the middle (F1 → Display).
- **Controls your way:** keyboard and mouse, or any controller SDL supports, played as a Wii U Pro Controller. Every
  button can be rebound, and the pages show which button does what for your controller (Xbox, PlayStation or
  Nintendo names).
- **Menus you can point at:** in the game's menus the mouse is a pointer; hovering a row selects it and a click picks
  it, from the main and in-game menus to the item box, shops, Smithy, Quest Counter, Canteen, house, Farm, Ferry,
  Argosy, Fishmongress, Supply Box and their prompts.
- **An on-screen keyboard in the game's style** whenever the game asks for text, for the controller or the mouse; the
  PC keyboard types too.
- **F3** cycles the GamePad screen (hidden, picture-in-picture, GamePad only); **F11** switches between fullscreen
  and a window and frees the mouse.

### Change settings while you play: the CFG tab

You don't have to go back to the title screen to change anything:

1. Open the in-game menu with **−** (**M** on the keyboard), in town or on a quest.
2. Move right past the game's last tab to **CFG** (d-pad / arrow keys left and right).
3. Pick a row:

| Row | What you change |
|---|---|
| **Display Settings** | Display Mode (windowed, borderless, fullscreen), Window Size, Internal Resolution, Scaling (smooth or sharp), Frame Rate Limit, VSync |
| **Graphics Settings** | Look (Original or Tri), Bloom, Depth of Field, Ambient Occlusion, Global Illumination, Reflections, Vignette |
| **Camera Settings** | Modern Camera (on / off), Camera Distance (the game's own up to 200 %) |
| **Mods** | Quest Scaling, Auto Carve, Texture Packs, Instant Gathering, Instant Carve, Auto Delivery, Key Quest Markers |

Left / right (or a click on a value) changes it, and it is saved at once; most changes show straight away, while
Internal Resolution, Frame Rate Limit and VSync apply the next time you start. **B** goes back. Display and Graphics
Settings are also in the title screen's Options → Game Options, next to Controller and Keyboard Settings.

<p align="center">
  <img src="docs/images/0.2.0/game-options.jpg" width="49%" alt="Game Options with Display Settings and Graphics Settings in a second column">
  <img src="docs/images/0.2.0/controller-settings.jpg" width="49%" alt="Controller Settings: remapping the game's buttons">
</p>

## Getting started

| | Windows | Linux |
|---|---|---|
| **Download** | `Cha-Cha-<version>-windows-x64.zip` from [Releases](../../releases/latest) | `Cha-Cha-<version>-x86_64.AppImage` or `Cha-Cha-<version>-linux-x86_64.tar.gz` from [Releases](../../releases/latest) |
| **Needs** | Windows 10 / 11 x64 | x86-64 with glibc 2.35 or newer (Ubuntu 22.04, Fedora 36, Steam Deck…) |
| **Graphics** | a Vulkan 1.2 driver | a Vulkan 1.2 driver (Mesa or your GPU vendor's) |
| **Run** | extract the zip anywhere you can write to, run `Cha-Cha.exe` | `chmod +x Cha-Cha-*.AppImage`, then run it; or extract the tarball and run `./Cha-Cha` (its `lib/` brings SDL3 and FFmpeg, so your distribution's FFmpeg version does not matter) |

1. **Put your dump in a folder named `game`** next to `Cha-Cha.exe` (or in the data folder below): a `.wua` archive,
   or the game and update folders (`code`, `content`, `meta`). Cha-Cha checks that it is the US v32 executable and
   reads the files where they are; nothing is copied. Elsewhere on disk works too: start once with
   `--install <path to your dump>`, which records it.
2. **Start Cha-Cha.** Without the game's files it says what it needs and closes.
3. **Wait a few seconds** on the loading screen while the game's shaders are compiled for your PC. This happens once;
   then the game's intro plays.

Your **data folder** keeps settings, saves, caches, quests, mods and texture packs:

| | Data folder |
|---|---|
| Windows | `%APPDATA%\Cha-Cha` |
| Linux | `~/.local/share/Cha-Cha` (or `$XDG_DATA_HOME/Cha-Cha`) |
| Portable | `data/` next to the program, when a file named `portable.txt` sits beside it (the Windows zip ships with one) |
| Folder | the program's own folder, when `Cha-Cha.ini` sits beside it (the Linux folder release ships with one) |
| Android | `Android/data/io.github.boyclinton.chacha/files` |

Saves live in `<data folder>/save`.

### Android (experimental)

`Cha-Cha-<version>-android-arm64.apk` runs the same game code on **Android 10 or newer, arm64, with Vulkan 1.1**. It
has not run on a real device yet, so it may not start on yours; reports are welcome on [#18](../../issues/18).

- **Controls:** a controller is needed for now: a handheld's built-in controls, a Bluetooth or USB controller, or a
  clip-on one. Touch controls are not there yet.
- **Your dump:** connect the device to a PC over USB and copy it into a folder named `game` inside the data folder
  above. `log.txt` in the same folder says what went wrong if it does not start.

### The folder release

Everything in one folder, like other recomps: unpack it anywhere you can write to.

```
Cha-Cha-<version>/
├── Cha-Cha                 the program
├── Cha-Cha.ini             settings (makes this folder Cha-Cha's data folder)
├── game/                   your dump: the game and update folders, or a .wua (found by itself)
├── mods/                   mods, one folder each (Damage Numbers, True Attack, Key Quests, Charm Table Fix included)
│   └── damage-numbers/
├── licenses/               Cha-Cha's GPL-3.0 and the bundled libraries' licences
├── README.txt
│   made on the first start:
├── cache/                  translated shaders and pipelines
├── save/                   your saves
├── quests/                 event and challenge quests
├── texture_replacements/   texture packs
└── log.txt                 the log (when not started from a terminal)
```

It needs FFmpeg's `libavcodec` from your distribution (for the movies). On another distribution than it was built on,
use the AppImage.

### Default controls

| Wii U Pro Controller | Keyboard and mouse | Controller |
|---|---|---|
| Left stick / right stick | WASD / O K L ; (or the mouse: click in the window, Esc releases) | sticks |
| A / B / X / Y | F, Return or right click / Space or Backspace / left click / E | the face buttons by position, as on the Wii U |
| X + A together | mouse 4 | |
| L / R / ZL / ZR | Q or Left Ctrl / Left Shift or mouse 5 / Z / C | LB / RB / LT / RT |
| + / − | Tab / M | Back / Start |
| D-pad | arrow keys, mouse wheel | d-pad |

Options → Game Options → **Controller Settings** remaps the controller (Nintendo layout by default, Custom once you
remap a button) and **Keyboard Settings** the keyboard and mouse. Bindings are saved in `<data folder>/input.ini`
([docs/input.md](docs/input.md)). In the Cha-Cha Plus overlay, F1 → Input → **Camera speed** slows the camera down
(30% to 100% of the game's own turn rate, for the stick, the camera keys and the mouse).

## The DLC screen: quests and mods

Capcom's download service for MH3U is gone, so Cha-Cha answers the game's own **DLC** screen itself, on your PC;
nothing goes online. It has three sections:

| Section | What it holds | Folder |
|---|---|---|
| **Official** | the game's own DLC menus with Capcom's event and challenge quests | `<data folder>/bonus` |
| **Quests** | your custom quests | `<data folder>/custom` |
| **Mods** | the [gameplay mods](#gameplay-mods-each-one-a-switch), ticked on or off, and **Texture Dump** | `<data folder>/mods` |

1. Put quests in `bonus` or `custom`: `q_<number>.quest` files, or `quest1` ... `quest4` copied from a Wii U save
   that has the downloaded quests. Subfolders are fine.
2. In the main menu open **DLC → Official** (or **Quests**) **→ Quests**, tick the ones you want and choose
   **Download**. Each quest's page shows its level, locale, time limit, reward, fee and monsters.
3. They appear at the **Port Tanzia Gathering Hall** with the event and challenge quests.

Quest numbers and details: [docs/mods.md](docs/mods.md). Quest files are game data, so Cha-Cha ships none.

<p align="center">
  <img src="docs/images/0.2.0/dlc-mods.jpg" width="100%" alt="The DLC screen's Mods list">
</p>

## Mods

Plugin mods (the Cha-Cha Plus mod API; upstream's built-in gameplay mods are in [DLC → Mods](#the-dlc-screen-quests-and-mods)).

Put mods in `<data folder>/mods`, one folder each. the overlay's **Mods** page (F2 in game) lists them, switches each on or off
(from the next start) and shows their settings.

- **Content mods** carry a `content/` folder whose files replace or add to the game's own (same paths as the game's
  `content` folder). Nothing in your dump is changed.
- **Plugins** (`plugins/*.dll` on Windows, `plugins/*.so` on Linux) are native code: they can hook game functions,
  draw over the game and add settings rows. The API is a single C header, [`include/chacha_plugin.h`](include/chacha_plugin.h).

Included:
- **Damage Numbers** ([mods/damage-numbers](mods/damage-numbers/README.md)): the damage of every hit over the monster,
  in the Wilds, Rise or Classic style.
- **True Attack** ([mods/true-attack](mods/true-attack/README.md), optional, starts off): menus show weapons' true
  attack instead of the bloated number (Iron Hammer 70 instead of 364). Damage is not changed.
- **Key Quests** ([mods/key-quests](mods/key-quests/README.md), optional, starts off): a gold key icon, "2 OF 5" and URGENT tags on
  the Quest Counter's list, for the quests that unlock the next urgent quest.
- **Charm Table Fix** ([mods/charm-table-fix](mods/charm-table-fix/README.md), optional, starts off): any talisman can
  come from any save. Cursed charm tables no longer lock talismans out, and your save keeps its own table.

Switch them on or off on the Mods page (F2 in game). Making mods: [docs/modding.md](docs/modding.md).

## Texture packs

**Using a pack:** put the DDS files in `<data folder>/texture_replacements` (subfolders are fine). The **Texture
Packs** mod switches them on and off while you play.

**Making a pack:**

1. In the main menu open **DLC → Mods** and choose **Texture Dump** (X). Every texture in the game is written to
   `<data folder>/texture_dump` as a DDS file, already named for replacement: `tex_<width>x<height>_<CRC32>.dds`.
   The game's download bar shows how far it is; Cancel stops it.
2. Edit the ones you want. Keep the name (you may add text after the CRC), include mipmaps, and copy them into
   `texture_replacements`.

Packs made for the 3DS version don't match the Wii U textures and won't load. Details:
[docs/texture-packs.md](docs/texture-packs.md).

## Online play (very experimental)

> [!WARNING]
> **Online is very experimental.** It is new, it has barely been played by real people, and things will break. Back up
> your `save` folder before trying it, and expect to restart the game. A session can end badly or drift out of sync.

**Found a problem? Please [open an issue](../../issues)**, that is how it gets fixed. It helps to say:
- your Cha-Cha version and system
- one PC or two, and who was hosting
- what you did, what happened, and what you expected
- `log.txt` from the data folder of **each** copy, if you can

### How it works

Nintendo's servers are gone, and Cha-Cha does not use a server of its own either. The game keeps its own online code,
and Cha-Cha replaces the part that talked to Nintendo. One player hosts, and the others' copies send their data
directly to each other over UDP (ports 47800-47807). The host picks the quest and the others follow.

Network play is off by default, and then only copies on the same PC can meet. Nothing is encrypted and the data other
players send goes straight into the game, so only switch it on with people and networks you trust.

### Try it

Every player needs the same version of Cha-Cha, their own copy of the game and a save that has reached Port Tanzia.

1. **Both players:** in Settings (F1) → Gameplay → Online, turn on **Play over the network** and press Apply. Allow
   Cha-Cha through your firewall if asked.
2. **Host:** in Port Tanzia, talk to Neko and pick **Host a Hunt** from the Online section of the Destination list.
   Wait for "Hosting a hunt".
3. **Everyone else:** do the same with **Join a Hunt**. Joining can take about 30 seconds longer if the game plays its
   Port Tanzia camera flyover first.
4. **Host:** choose a quest at the counter in the hall and depart. The others follow.
5. **To leave:** pick **Leave Online** from Neko, or Leave session in the settings.

If Join finds nobody on your network, add the host's IPv4 address to the settings file (`config.ini` in the data
folder, `Cha-Cha.ini` in the Linux folder release) as `online_host=192.168.1.20`, using the host's real address, and
restart.

**Alone on one PC:** copy your `save` folder into a new folder, start a second copy with `Cha-Cha --data-dir <new folder>`
and choose your dump. Leave Play over the network off on both. A controller reaches both windows, so play one of them
with the keyboard.

### What has been tried

| | |
|---|---|
| Hosting and joining | Played by a person on Linux, two copies on one PC |
| Both hunters in the hall, leaving and rejoining, quests together, a large monster moving the same way on both copies, matching damage | Automated tests only |
| The real quest counter and depart button, carving, rewards, chat, a full hunt | **Not tried** |
| Two PCs, three or four players, the Windows version, playing over the internet | **Not tried** |

The details are in [docs/online.md](docs/online.md).

## What's next

- **Online play:** make [what's there now](#online-play-very-experimental) reliable, then add a way to find each other
  over the internet, with HP scaling using the real number of hunters.
- **Touch controls** for Android phones, in the game's style.
- **Quest maker:** build your own quests in the game and share them with a code: copy it, send it, and anyone who
  pastes it gets the same quest.

## Building from source

The build translates **your own dump's** executable into C++, so put a dump in place first.

1. Put your dump at `game/mh3u.wua` ([game/README.md](game/README.md)), then run
   `powershell -File scripts/extract.ps1` and `powershell -File scripts/fingerprint.ps1`.
2. **Windows:** Visual Studio 2026 with "Desktop development with C++" and the Vulkan SDK
   ([tools/README.md](tools/README.md)):

   ```
   cmake -S . -B build/cmake
   cmake --build build/cmake --config Release
   ctest --test-dir build/cmake -C Release --output-on-failure
   ```

   A clang-cl build (`cmake -S . -B build/clang -T ClangCL`) compiles the generated code faster.
   `scripts/package-release.ps1` makes the release zip.
3. **Linux** (or WSL2 on Windows): clang 15+, Ninja, CMake 3.25+, the LunarG Vulkan SDK, `libvulkan-dev`,
   `libavcodec-dev` and SDL3's build dependencies. `scripts/build-linux.sh` builds it, `scripts/package-appimage.sh`
   makes the AppImage and the tarball ([docs/linux.md](docs/linux.md)).
4. **Android** (from Windows): Android Studio with NDK 30 and CMake 4.1.2, plus WSL2 for FFmpeg; with the Windows
   build configured, `scripts/build-android.ps1` makes the APK ([docs/android.md](docs/android.md)).

   On a Linux desktop with the distribution's SDL3 (Fedora: `SDL3-devel`, `libshaderc-devel`, `vulkan-loader-devel`,
   `libavcodec-free-devel`), configure with `-DMH3U_SYSTEM_SDL3=ON`. An SDL3 built from source without the PipeWire /
   PulseAudio / ALSA headers has no sound:

   ```
   cmake -S . -B ~/cha-cha-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DMH3U_SYSTEM_SDL3=ON \
         -DMH3U_RECOMP_OUT=$HOME/cha-cha-build/recomp-out
   cmake --build ~/cha-cha-build --target mh3u damage_numbers
   scripts/package-folder.sh ~/cha-cha-build <version>      # the folder release, in build/release/
   ```

Inside the checkout Cha-Cha finds `extracted/` by itself and uses the checkout as its data folder (all of it
gitignored). The release packages never contain game files, but the program itself contains the recompiled game
code, translated from your executable at build time.

<details>
<summary><b>Repository layout</b></summary>

| Path | What lives there | In git? |
|---|---|---|
| `game/`, `extracted/` | Your dump, and its unpacked contents | no |
| `config/` | Target hashes, symbol map, function list, known structures | yes |
| `recomp/` | The recompiler; `recomp/out/` is the generated C++ | source only |
| `runtime/` | The PC layer replacing the Wii U system: `os`, `gfx` (GX2 on Vulkan), `audio`, `input`, `fs`, `boot` | yes |
| `src/` | Hand-written game code: item sets, mods, the camera, Cha-Cha's menu pages, online… | yes |
| `include/` | `chacha_plugin.h`, the plugin API for mods | yes |
| `mods/` | Plugin mods built with the tree: Damage Numbers, True Attack, Key Quests, Charm Table Fix | yes |
| `android/` | The Android app around the game library (SDL's activity, manifest, Gradle) | yes |
| `tools/`, `scripts/`, `ghidra/` | Shader translator, helper scripts, reverse-engineering scripts | yes |
| `docs/` | Formats, the runtime and engine, mods, input, Linux, Android, online notes | yes |
| `build/` | Build output | no |

How the port works: [docs/runtime.md](docs/runtime.md), [docs/engine.md](docs/engine.md),
[docs/formats/](docs/formats).

</details>

## Credits

- **Dusklight**: the Twilight Princess PC port whose front end Cha-Cha's first front end followed.
- **Yakumo**: the asset-free release model Cha-Cha follows: a release build plus your own dump, nothing from the
  game shipped.
- **Ghidra**, for reverse engineering.
- **Libraries:** SDL3, zstd, the Vulkan SDK (shaderc, glslang, SPIRV-Tools; on Android the NDK's shaderc), FFmpeg's
  libavcodec on Linux and Android ([docs/release/THIRD_PARTY_NOTICES.md](docs/release/THIRD_PARTY_NOTICES.md)).

Monster Hunter is a trademark of Capcom. Wii U is a trademark of Nintendo.

## License

Cha-Cha is free software under the **GNU General Public License v3.0** ([LICENSE](LICENSE)): you may use, study,
change and share it, and if you share a changed version you share its source under the same licence. Third-party
libraries keep their own licences. The licence covers Cha-Cha's own code only, not the game: Monster Hunter 3
Ultimate and everything translated from it remain Capcom's.
