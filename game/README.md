# game/

Put your **own** dump of MH3U here as a single `.wua` (Cemu's Wii U Archive format).

```
game/
  mh3u.wua        <- base game + latest update merged (recommended)
```

- Dump from your own Wii U (e.g. dumpling), then convert to `.wua` in Cemu:
  *Tools → Title Manager → right-click title → Convert to compressed Wii U archive (.wua)*.
  Include the **update** so we target the final game version; DLC is optional.
- Everything in this folder is gitignored. Do not share or commit it.
- After placing it, run `scripts/extract.ps1` and then `scripts/fingerprint.ps1`.
