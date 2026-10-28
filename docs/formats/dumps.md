# Wii U dump formats

What users have when they dump their copy, and what the port reads (`runtime/fs/game_image.*`). The port needs the
**base game (title 0005000010118300) and update v32 (0005000E10118300)**. The update is never on the disc, so a disc
image alone is never enough.

| Format | What it is | Encrypted | Read by the port |
|---|---|---|---|
| `.wua` | Cemu's ZArchive: base, update and DLC as folders `<titleid>_v<version>` in one zstd-compressed file ([wua.md](wua.md)). Cemu: Title Manager → "Convert to compressed Wii U archive" | no | **yes**, in place |
| Title folder (`code/ content/ meta/`) | "loadiine" layout: Dumpling, cdecrypt output, Cemu game folders. One title per folder: the base and the update are separate folders | no | **yes** |
| Folder of title folders | e.g. Dumpling's `Games/` + `Updates/`, our `extracted/` (`scripts/extract.ps1`) | no | **yes** (searched up to 4 levels deep) |
| Cemu `mlc01` | `usr/title/00050000/10118300` (base) + `usr/title/0005000e/10118300` (update) | no | **yes** (choose `mlc01`) |
| `code/*.rpx` | The executable inside a title folder | no | **yes** (taken as its title folder) |
| `.wud` / `.wux` | Raw / compressed (deduplicated sectors) disc image | yes (disc key + common key) | no: the user converts it in Cemu, or dumps the decrypted folders |
| NUS / WUP (`title.tmd`, `title.tik`, `*.app`, `*.h3`) | eShop/installable format | yes (title key from the ticket, common key) | no: same advice |

Several sources can be combined (several `game=` lines in `config.ini`, or picking a game folder and then an update
folder in the setup screen): the base game and the update may come from different places.

## Identifying a title

`code/app.xml` has the title's own id and version: `<title_id>0005000E10118300</title_id>`,
`<title_version>0020</title_version>` (hex; 0x20 = 32). **`meta/meta.xml` is not reliable for updates**: MH3U's v32
update has `<title_id>0005000010118300</title_id>` (the base id) with `<title_version>32</title_version>` (decimal)
there. The reader uses `app.xml` and falls back to `meta.xml` (decimal version) only when `app.xml` is missing.
Verified on the US base v0 and update v32 (2026-10-26).

## The executable check

After choosing base and update, `/vol/code/MH3G_Cafe_US.rpx` (update over base) must have SHA-1
`d97aabfc5a45362f88dfacb09f23c6dcae6faac2` (`config/target.txt`). Messages distinguish: no MH3U at all / another
region, update missing, another update version, and a modified executable.

## Encrypted formats (not implemented)

Reading `.wud`/`.wux`/NUS would need AES-128-CBC with keys the user supplies (Cemu's `keys.txt` style: the Wii U
common key + per-title keys) and the FST/TMD parsers. It is only worth it together with an encrypted update (NUS),
since a disc never has v32.
