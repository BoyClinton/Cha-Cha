Cha-Cha {VERSION} for Windows x64
A native PC port of Monster Hunter 3 Ultimate (Wii U), by static recompilation.

1. Extract the whole folder somewhere you can write to (not Program Files).
2. Run Cha-Cha.exe.
3. On first start, choose your own dump of Monster Hunter 3 Ultimate (US) with
   update v32: a .wua archive, or the game and update folders (code, content,
   meta). The files are read where they are; nothing is copied. The port then prepares the game's shaders for your PC (a few
   seconds, once).

This is the portable version: because of portable.txt, settings, saves and
caches are kept in the data folder next to Cha-Cha.exe. Delete portable.txt to use
%APPDATA%\Cha-Cha instead. Saves are under data\save.

Command line: Cha-Cha --install <dump>  records the dump without the setup screen;
              Cha-Cha --game <dump>     plays a dump without recording it.

In game: F1 opens the settings, F2 the mods (DLC quests and mods), F3 cycles the
GamePad screen view, F11 switches fullscreen / window. Settings > Input > Camera speed slows the camera down;
Settings > General > Monster HP scaling switches the Port Tanzia HP scaling off.

Mods: one folder each in the mods folder of the data folder (data\mods here).
Damage Numbers comes with Cha-Cha (the damage of every hit over the monster);
switch mods on or off and change their settings in MODS > Mods (F2).

Controls: keyboard + mouse or any controller SDL supports (played as a Wii U Pro
Controller). Key bindings: input.ini in the data folder (see input.example.ini).

Requirements: Windows 10/11 x64 and a Vulkan 1.3 graphics driver. SDL3 is
included; nothing else needs installing.

This archive contains no game assets, disc image, game executable or saves.
Only the US release with update v32 works: the recompiled code is specific to
that executable, and the setup screen checks it.

Cha-Cha is free software under the GNU GPL v3.0 (LICENSE.txt); the source is on
GitHub. Third-party licences are in the licenses folder. Monster Hunter is a
trademark of Capcom; Cha-Cha is a fan project, not affiliated with Capcom or
Nintendo.
