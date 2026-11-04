# True Attack

Menus show a weapon's **true attack** instead of the bloated number. MH3U stores every weapon's true attack, the value
the damage formula uses, and multiplies it by a factor per weapon class only for display. With this mod an Iron Hammer
shows **70** instead of **364** (364 = 70 × 5.2).

- **Where:** every place the game shows weapon attack (Status, Equipment, Smithy, item box, Guild Card), because they
  all go through the same display function.
- **Damage is not changed:** the game's damage code never uses the displayed number.
- **Optional:** the mod starts **off**. Switch it on in **MODS → Mods → True Attack**. The **Show true attack** switch
  in its settings applies at once; reopen a menu to see it.
- **Needs:** Cha-Cha 0.2.2 or later (the hook point `0x02045108`) and MH3U US with update v32.

How it works: `FUN_02045108(item, raw)` returns `raw × table[weapon class] / 100`, where the table is at `0x1017C13C`
(u32 ×100: 480 140 520 230 148 130 130 330 460 230 120 140 460 170). The mod returns `raw` instead. Details are in
`docs/research-log.md` (2026-11-04).

`CHACHA_TRUE_ATTACK_LOG=1` logs the first 30 values (raw → what the game would show).
