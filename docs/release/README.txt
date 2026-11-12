Cha-Cha {VERSION} for Windows x64
A native PC port of Monster Hunter 3 Ultimate (Wii U), by static recompilation.

1. Extract the whole folder somewhere you can write to (not Program Files).
2. Put your dump of Monster Hunter 3 Ultimate (US) with update v32 in a folder
   named game next to Cha-Cha.exe (or in the data folder below): a .wua archive,
   or the game and update folders (code, content, meta). The files are read
   where they are; nothing is copied.
3. Run Cha-Cha.exe. Without the game's files it says what it needs and closes.
   On first start the game's shaders are compiled for your PC (a few seconds,
   once).

This is the portable version: because of portable.txt, settings, saves and
caches are kept in the data folder next to Cha-Cha.exe. Delete portable.txt to use
%APPDATA%\Cha-Cha instead. Saves are under data\save.

Command line: Cha-Cha --install <dump>  records a dump kept elsewhere;
              Cha-Cha --game <dump>     plays a dump without recording it.

In game: Cha-Cha's settings are in Options > Game Options (Display, Graphics,
Controller and Keyboard Settings) and in the pause menu's CFG tab (also Camera
Settings and Mods); the main menu's DLC screen holds the official DLC, custom
quests and mods (Quest Scaling switches the Port Tanzia monster HP scaling).
F3 cycles the GamePad screen view, F11 switches fullscreen.
Cha-Cha Plus adds an overlay: F1 (or Select + Options on a controller) opens its
settings (Input > Camera speed, aspect ratio, online), F2 its Mods page.

Online (very experimental): talk to Neko in Port Tanzia, then Host a Hunt or Join a
Hunt. To play across your network, first turn on Settings > Gameplay > Online > Play
over the network (F1), only with people you trust. Steps are in the README on GitHub.
Please report problems: https://github.com/SilversRayGA/Cha-Cha-Plus/issues

Mods: one folder each in the mods folder of the data folder (data\mods here).
Damage Numbers comes with Cha-Cha Plus (the damage of every hit over the monster);
switch plugin mods on or off and change their settings on the Mods page (F2).

Controls: keyboard + mouse or any controller SDL supports (played as a Wii U Pro
Controller). Key bindings: input.ini in the data folder (see input.example.ini).

Requirements: Windows 10/11 x64 and a Vulkan 1.2 graphics driver. SDL3 is
included; nothing else needs installing.

This archive contains no game assets, disc image, game executable or saves.
The menus are drawn with the game's own GUI textures, read from your dump
when Cha-Cha starts; none are included here.
Only the US release with update v32 works: the recompiled code is specific to
that executable, and Cha-Cha checks it at start.

Cha-Cha is free software under the GNU GPL v3.0 (LICENSE.txt); the source is on
GitHub. Third-party licences are in the licenses folder. Monster Hunter is a
trademark of Capcom; Cha-Cha is a fan project, not affiliated with Capcom or
Nintendo.
