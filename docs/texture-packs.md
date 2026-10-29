# Texture packs

Cha-Cha can replace any of the game's textures with DDS files. Code: `runtime/gfx/texture_pack.*` (folder index, DDS
read/write), `VkRenderer::replace_texture` in `runtime/gfx/renderer.cpp`.

## Where

`<data folder>/texture_replacements/`. The data folder is `%APPDATA%\Cha-Cha` for a release, `data\` next to the exe
in portable mode, and the repo root when run from a source checkout. The folder is created at start. Each pack can
sit in its own subfolder, nested as deep as you like. When two files replace the same texture, the one whose path
sorts last wins. `MH3U_TEXTURE_DIR` points runs elsewhere.

Settings → VIDEO → **TEXTURE PACKS** switches them on and off while the game runs.

## File names

```
tex_<width>x<height>_<CRC32>.dds          e.g. tex_256x256_1A2B3C4D.dds
tex_<width>x<height>_<CRC32><anything>.dds   (text after the CRC is ignored: tex_256x256_1A2B3C4D_hunter_face.dds)
```

- `width` x `height` is the game texture's size.
- `CRC32` is taken over its level 0 exactly as it sits in Wii U memory (GX2-tiled), so the name is the same on
  every machine and every run.
- Don't work these out by hand: **dump** the textures and copy the files you change.

## Dumping

Settings → VIDEO → **DUMP TEXTURES**. It starts at once and applies to this session only. Every texture the game uses
from then on is written to `<data folder>/texture_dump/` under its replacement name, including textures already
loaded, the next time they are drawn. Toasts show the folder and a running count. For runs, `MH3U_DUMP_TEXTURES=1`
does the same, and `MH3U_DUMP_TEXTURES=<folder>` writes somewhere else.

- **Formats.** Dumps are DDS level 0 in the game's own format: BC1 / BC3 (DXT1 / DXT5), or RGBA8. They open in
  Paint.NET, GIMP and texconv.
- **Not dumped:**
  - videos and other textures the CPU rewrites every frame;
  - cube maps;
  - render targets.

## The replacement

- **Format.** Any size and aspect ratio works, though keeping the original's aspect ratio is wise. Supported formats:
  - BC1, BC2, BC3, BC4, BC5, BC7, from a DX10 header or the DXT1/3/5, ATI1/2 FourCCs;
  - RGBA8 / BGRA8.
- **Mips.** Include them in the file, or the texture shimmers in the distance. Only the levels that are present are
  used.
- **Colour space.** sRGB vs UNORM follows the original texture, not the file.
- **Loading.** A replacement is loaded the first time its texture is uploaded, and again when the game reloads it.

## 3DS packs

Packs made for Monster Hunter 3 Ultimate on 3DS don't load. Their file names are hashes of 3DS texture data, which
never match the Wii U textures, and Cha-Cha has no converter for them and won't get one. Packs are game art: keep them
out of git (`texture_replacements/` and `texture_dump/` are ignored).
