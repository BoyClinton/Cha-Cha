# Input

All host input – keyboard + mouse and any number of controllers at once (buttons are OR'ed; for each stick the
source pushed furthest wins) – drives one Wii U controller, chosen with `MH3U_CONTROLLER`:
- `pro-only` (**default**): a **Wii U Pro Controller** on `padscore` channel 0 and no GamePad. The game plays on the
  TV with the Pro Controller layout; GamePad touch-screen features are not used.
- `pro`: the Pro Controller plus a connected GamePad whose only live input is the touch screen.
- `gamepad`: the **Wii U GamePad** (`vpad` channel 0), as in the first versions of the port.

Code: `runtime/input/host_input.*` (SDL3, host main thread) → `runtime/os/input.cpp` (`VPADRead`, motor).
Settings: `input.ini` in the data folder (not in git), written by the launcher's **Settings > Controls** page.

## Rebinding in the launcher
**F1** (or a controller's **touchpad click** on PlayStation pads, **Home / PS / Xbox** button, or the DualSense mic /
Xbox Share / Switch capture button) opens the settings over the running game; again, or Esc / B, returns to it. None
of them is bound to a game button (`input::is_settings_button`; an older `input.ini`'s `touchpad = tv` is ignored).
Changed settings are applied with the confirm button (A / Enter) on any value row – left / right change the value –
or with + / Ctrl+Enter / the Apply button. **F2** opens
MODS (quests, gameplay) the same way; F3 cycles the GamePad view.
**Settings > Controls** lists every game button and stick direction with its keyboard / mouse and controller
bindings. Choose a cell and press the key, mouse button, wheel direction, controller button or trigger for it
(Esc cancels, Delete clears, 6 s timeout); the game uses it at once and `input.ini` is saved. One source per game
button per device: binding replaces the old one, and the source leaves its previous game button. Stick directions
stay on the controller's sticks. Also there: controller layout (Xbox / Nintendo; switching resets the controller
bindings), mouse sensitivity, invert camera Y, reset to defaults.
`input.ini` sections `[bindings]` (keys / mouse) and `[controller]` (SDL gamepad button names, `lefttrigger`,
`righttrigger`) change the defaults line by line, or replace them when they start with `clear = 1` (as saved by the
launcher). Test: `bindings_test` (bind, save, reset, reload).

## Controllers
SDL3's gamepad API maps everything to one layout, so the same code serves:
- **Xbox** (XInput / Xbox Wireless), **PlayStation** DualShock 4 and DualSense (SDL's HIDAPI drivers, enabled
  explicitly), Switch Pro Controller and other SDL-known pads.
- Face buttons: `layout = nintendo` (default) maps by position like the GamePad (bottom = B, right = A).
  `layout = xbox` maps by Xbox label – A / Cross confirms as Wii U A, B / Circle is Wii U B.
- LB/RB = L/R, LT/RT = ZL/ZR (> 50 %), Back/Share = +, Start = −, stick clicks, d-pad. Radial deadzone (0.15),
  rescaled so the stick still reaches full deflection.
- **DualShock 4 / DualSense touchpad** → GamePad touch screen (finger 0, scaled to 854x480); touchpad click = the
  settings.
- Rumble: `VPADControlMotor` patterns rumble every connected controller (applied on the main thread).

## Keyboard + mouse
- **Mouse look.** Click in the window to capture the mouse (that click is not sent to the game); Esc releases it,
  as does losing focus. MH3U turns the camera with the right stick at a speed set by its deflection, so mouse movement
  since the game's previous `VPADRead` becomes right-stick deflection (`sensitivity` per pixel, averaged over two
  reads to smooth spikes, added to any stick input and clamped). A very fast flick saturates at full stick speed.
  `invert_y` flips vertical look.
- Defaults (all rebindable): WASD = left stick, IJKL = right stick, arrows = d-pad, Space/Backspace = B, Return/F = A,
  E = Y, Left Shift = R, Q = L, Z/C = ZL/ZR, Tab = +, M = −, V = left-stick click; left mouse = X, right mouse = A,
  middle = right-stick click, mouse 4/5 = ZL/ZR, wheel = d-pad left/right (a notch holds the button for 3 frames).
  The default action assignment follows the Wii U layout as far as known (X attack, A special, B roll, R dash,
  L camera) and has to be checked in game.
- Keys use SDL scancodes, so WASD stays in place on AZERTY etc.

## VPADStatus as written
hold/trigger/release (trigger and release from the previous read), both sticks (y up positive), stick-emulation bits
(stick past 0.5 → `VPAD_STICK_*_EMULATION_*`, used by menus), touch in tpNormal / tpFiltered1 / tpFiltered2, battery
full. Gyro/accelerometer stay zero. The game reads 16 samples per call; we return 1 (the newest), which it copies
from buffer 0 (`sub_02BCBC44` → +0x2D0 of its pad object).

## Pro Controller (default)
How MH3U finds a Pro Controller (`sPad::WiiPad`, one per channel at `sPad::mpInstance + 0x70 + chan * 0x4A0`):
`KPADSetConnectCallback(chan, 0x02BC9DCC)` → on `(chan, 0)` the game sets its flags (+0x498) to 1 (core),
registers `WPADSetExtensionCallback(chan, 0x02BC9CD4)` and asks `WPADGetInfoAsync`; the extension callback maps
type 31 (URCC) to flags 0x11. Only then does `sPad::WiiPad::vf7` (0x02BCB068) call `KPADReadEx(chan, buf, 16)` each
frame, copy sample 0 to +0x2D0 and `vf8` (0x02BCB108) decode the Pro buttons at status +0x60 (requires wpadErr 0).
The HLE reports channel 0 connected and queues those callbacks (connect, then extension type 31; GetInfoAsync's
result) to run on the game's thread at its next `VPADRead`/`KPADReadEx`, like the real asynchronous delivery.
`KPADReadEx` writes one sample: devType 31, data format 22, Pro buttons (`WPAD_PRO_*`, mapped from the GamePad
bits) with trigger/release, both sticks; `WPADControlMotor` rumbles. With a Pro Controller connected the game skips
the "Wii U GamePad has been detected" notice.
**Which controller the game listens to** is a game option, stored in the save ("The set controller could not be
detected. Please connect a controller with the correct settings, or change your controller settings from the
Options menu."). With `MH3U_CONTROLLER=pro` the GamePad stays connected (touch screen and GamePad screen work), and
a save created with the GamePad keeps listening to the GamePad until the controller is changed in the game's
Options – as on a real Wii U. `MH3U_CONTROLLER=pro-only` disconnects the GamePad: the game shows the notice above
once, A on the Pro Controller dismisses it, and the Pro Controller drives the game (verified headless: notice →
intro movie skipped with Pro A → title → main menu). The menu then reminds that the set controller is missing until
the controller setting is changed in the game's Options.

## Text entry (character name etc.)
There is no on-screen keyboard: when the game opens the Wii U software keyboard (swkbd), you type on the PC keyboard.
The window title shows the text (`Type: Ryo_  (Enter or A = OK, Esc or B = cancel)`); Backspace deletes, **Enter**
confirms, **Esc** cancels, and on a connected controller A (by the configured layout) or Start confirms, B cancels.
Game key bindings are suspended while typing. The game's own length limit applies (10 characters for a name).
Headless runs take the text from `MH3U_TEXT_INPUT` (default "Hunter") and confirm it at once.

Implementation: `input::begin_text` / `text_state` (runtime/input) and the swkbd HLE in `runtime/os/applets.cpp`
(input form: initial text and limit from the AppearArg; keyboard mode: text written to the game's receiver buffer
and its change callback called from `SwkbdCalc` on the game thread). The game only opens the keyboard after
`SwkbdInitLearnDic` succeeds (an empty "NJDC" learning dictionary) and `SwkbdCreate` takes ~1 s (as Cemu notes).

## Testing headless
`MH3U_INPUT_SCRIPT="frame[+frames]:BUTTON[|BUTTON];..."` presses buttons at given swap counts (6 frames by default),
e.g. `700:A;1000:A|R`. With A presses the boot gets past the "Wii U GamePad has been detected" notice to the
"extended data save settings" Yes/No prompt. Boot timing varies run to run, so scripts need slack.
