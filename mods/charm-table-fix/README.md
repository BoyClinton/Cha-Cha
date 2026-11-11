# Charm Table Fix

Any talisman can come from any save.

MH3U ties every save to one of 17 **charm tables**, fixed when the save was started. Each table can only give some of
the game's talismans, and five of them (11, 12, 15, 16, 17, the **cursed tables**) give far fewer than the rest.
On the console the only way out was a new save made at a set clock time.

With this mod, each talisman is rolled as if your save were on a random one of the 17 tables, with the game's own
odds for skills, points and slots. Every talisman any table can give is possible, on every save.

- **Optional:** the mod starts **off**. Switch it on in **MODS → Mods → Charm Table Fix**. It takes effect on the
  next start, like every mod.
- **Your save:** keeps its own table, and talismans you already have don't change. Nothing is written to the save
  that the game wouldn't write itself. Switch the mod off and talismans roll exactly as they would have without it.
- **Charm-table guides** (table lookups, "sniping" a talisman by counting rolls) don't apply while it's on: rolls are
  random.
- **Needs:** Cha-Cha with mod support and MH3U US with update v32.

## How it works

The game draws every number of a talisman from one random-number generator that steps
`x → x × 176 mod 65363`. 65363 is not a prime (163 × 401), so the generator's 65,362 states split into 17 separate
loops: twelve of 5,400 states, two of 200 and three of 54. Your save stores the state and nothing ever reseeds it,
so a save never leaves its loop. A talisman is decided by the state its first draw starts from, so each loop is a
table, and the three 54-state loops and two 200-state loops are the cursed ones.

While the game rolls a talisman, the mod starts the generator from a state picked at random out of all 65,362.
Then it puts your save's own state back and moves it on by as many draws as the roll used, as the game would have.
Details are in `docs/research-log.md` (2026-11-11).
