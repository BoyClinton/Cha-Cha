# Product

<!-- impeccable:product-schema 1 -->

## Platform

Native Windows desktop app (C++20, Dear ImGui over Vulkan, SDL3). Not web: the UI is drawn in the game window.

## Users

Players of Monster Hunter 3 Ultimate on PC who supply their own dump of the game (US release, update v32). They
use the launcher and its panels before play and over the running game (F1 settings, F2 mods), mostly with a
controller from the couch, sometimes with keyboard and mouse.

## Product Purpose

Cha-Cha is a native PC port of MH3U by static recompilation. The launcher and its panels let players point it at
their dump, set display, graphics and input options, and install DLC quests, then get out of the way of the game.

## Capabilities and Constraints

- Settings panel tabs: General, Video, Input; Mods panel: Quests. Changes wait for Apply (Input applies at once).
- Every screen must work fully with a controller, using the player's own bindings (input.ini / the Input tab):
  the game's A selects, B goes back, L / R switch tabs, + applies, the d-pad moves and changes values.
- Button prompts must follow the bindings, the controller type (Xbox / PlayStation / Nintendo names) and the
  Nintendo / Xbox layout option; with no controller connected, keyboard keys are shown.
- System fonts only (Bahnschrift); nothing from the game and no bundled assets in the UI.
- Same panel before the game and over it; over the game it sits on a dimmed frame.

## Brand Commitments

- Name: Cha-Cha. Warm dark browns with an amber accent are the project's own colours (user, 2026-10-28: keep
  them, not GNOME's colour scheme).
- Settings structure in the spirit of GNOME Adwaita: rounded grouped rows, generous spacing, quiet controls,
  sentence-case labels, switches for on/off (user, 2026-10-28).
- Descriptions of the highlighted row stay in a side pane (user, 2026-10-28).

## Product Principles

- The game comes first: panels are quick to open, read and leave.
- Controller-first: nothing needs a mouse; the screen always says which button does what.
- Honest settings: say what a setting does and when it takes effect.
