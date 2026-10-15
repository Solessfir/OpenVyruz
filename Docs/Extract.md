# Extract and Compress

`--extract` turns the original archives into a tree of modern files that any engine or editor reads; `--compress` packs such a tree back into archives that both the original and the port read. Both are port-only tools (`Source/Tools`); the game itself still reads the original archives.

```
Vyruz --extract <game folder | archive.rsc> [<output folder>]
Vyruz --compress <extracted folder> [<output folder>]
Vyruz --verify-extract <game folder>
```

Given the game folder, `--extract` reads `vz-data.rsc` and `Vz-slide.rsc`; given one `.rsc`, only that archive. The output defaults to `Extracted` beside the input, and `--compress` writes `Packed` beside the tree; neither touches the game's own files. The data never belongs in this repository.

## Layout

Each archive path loses its zone's `DATA` folder and every part becomes PascalCase, a `+` written as `Plus` (CPU has both `ROOF` and `ROOF+`), so `COMMON\DATA\OBJECT\TURRET\V-TURRET-GUN-UDG.GAF` becomes `Common/Object/Turret/VTurretGunUdg.*`. A zone's level files gather in `<Zone>/Levels/<Id>/`, with their pages as `PageEn.txt` and `PageFr.txt`; its class libraries go to `<Zone>/Classes/`; the archive's loose texts to `Text/`; the slides to `Slides/`.

| Original | Extracted |
|---|---|
| `.gaf` | `Name.png`, an atlas of the GAF's bitmaps with the colour key transparent and each RGB555 channel widened as `(c << 3) \| (c >> 2)`; `Name.json` with each bitmap's place and trim, each animation's frames (bitmap, offset, duration in ticks and ms, mirroring, draw flags and blend, events, sound), and each font; `NameFontN.fnt`, BMFont text files over the same atlas; `NameSoundN.wav`, the SMP_ samples as 16-bit PCM |
| `.wav` | the same file |
| `.tga` | `.png` at full colour |
| `.txt` | UTF-8 text, from Windows-1252, or Windows-1251 when the archive's menu font draws Cyrillic (the Russian build) |
| `.3dc` | JSON of its blocks: kind, name, fields in order with repeats, children; the format line as `Format` |
| level `.3dx .3di .3do .3dp .3dg` | `<Id>.tmj`, a Tiled map; `Tileset.png`; `Terrain.json` |
| `DEMO.3DR` | `Recording.json`, each record's time, input bits, position and facing |

The Tiled map has one tile layer per `MAP_` layer over the embedded tileset (the `.3DG` blocks, each with its `Depth`), and four object layers: `Objects`, every `.3DO` object at its `Position` with its other fields as string properties, nested blocks as JSON in `Blocks`, and `Spawn = Player` on the player; `PatrolPaths`, each `.3DP` path as a polyline with its `Id`; `MapAnimations`, each animated tile at its cell with its GAF JSON and animation; and `Collision`, the static terrain shapes the port builds, in world pixels, as lines or polygons. The `.3DO` header fields are map properties. `Terrain.json` keeps what Tiled has no place for: the `.3DX` descriptors (terrain type, GAF and animation) and the `.3DI`'s four slots per cell, row-major.

`Manifest.json` at the root lists, per archive, its source path and text encoding, and per entry every file made from it with an FNV-1a hash.

## Compress

`--compress` walks each source archive in its own order. An entry whose files all still hash the same copies the source's bytes. An edited one converts back: a WAV as it is, a text into its code page, a PNG into an uncompressed 24-bit Targa keeping the original's row order, and a class library's JSON into the block grammar. Editing a GAF, a level or the demo is not supported yet: the run names the entry and fails rather than write a broken archive.

The archive it writes has unencrypted tables (key 0) behind the `_KX_` magic and its data right after the name table, which `RscOpen` (`004551a0`) requires (`DataOffset` must be where the name table ends); it finds entries through a sorted hash index, so their order does not matter to it.

`--verify-extract` extracts the game folder, packs the tree back and checks that every entry reads back byte for byte and that every class library's JSON parses back to the same blocks; then it edits a text and an image and checks both come back through their converters.
