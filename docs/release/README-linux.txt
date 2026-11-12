Cha-Cha {VERSION} for Linux x86-64
A native PC port of Monster Hunter 3 Ultimate (Wii U), by static recompilation.

1. Extract the whole folder somewhere you can write to and keep lib/ next to
   Cha-Cha: it holds the libraries Cha-Cha needs (SDL3, FFmpeg's libavcodec and
   theirs), so the FFmpeg version of your distribution does not matter.
2. Put your dump of Monster Hunter 3 Ultimate (US) with update v32 in a folder
   named game next to Cha-Cha (or in the data folder below): a .wua archive, or
   the game and update folders (code, content, meta). The files are read where
   they are; nothing is copied.
3. Run ./Cha-Cha. Without the game's files it says what it needs and closes.
   On first start the game's shaders are prepared for your PC (a few seconds,
   once).

Settings, saves and caches are kept in ~/.local/share/Cha-Cha (or
$XDG_DATA_HOME/Cha-Cha); create an empty portable.txt next to Cha-Cha to keep
them in a data folder here instead. Saves are under <data folder>/save.

Command line: Cha-Cha --install <dump>  records a dump kept elsewhere;
              Cha-Cha --game <dump>     plays a dump without recording it.

In game: Cha-Cha's settings are in Options > Game Options (Display, Graphics,
Controller and Keyboard Settings) and in the pause menu's CFG tab (also Camera
Settings and Mods); the main menu's DLC screen holds the official DLC, custom
quests and mods. F3 cycles the GamePad screen view, F11 switches fullscreen.

Controls: keyboard + mouse or any controller SDL supports (played as a Wii U Pro
Controller). Key bindings: input.ini in the data folder (see input.example.ini).

Requirements: x86-64 with glibc 2.35 or newer and a Vulkan 1.2 graphics driver
(Mesa or your GPU vendor's). The Vulkan loader and the driver are your system's.

This archive contains no game assets, disc image, game executable or saves.
Only the US release with update v32 works: the recompiled code is specific to
that executable.

Cha-Cha is free software under the GNU GPL v3.0 (LICENSE); the source is on
GitHub. Third-party licences are in THIRD_PARTY_NOTICES.md. Monster Hunter is a
trademark of Capcom; Cha-Cha is a fan project, not affiliated with Capcom or
Nintendo.
