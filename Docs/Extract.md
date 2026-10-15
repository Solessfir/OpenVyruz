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
| `.gaf` | a folder `Name/` (below) |
| `.wav` | the same file |
| `.tga` | `.png` at full colour |
| `.txt` | UTF-8 text, from Windows-1252, or Windows-1251 when the archive's menu font draws Cyrillic (the Russian build) |
| `.3dc` | JSON of its blocks: kind, name, fields in order with repeats, children; the format line as `Format` |
| level `.3dx .3di .3do .3dp .3dg` | `<Id>.tmj`, a Tiled map; `Tileset.png`; `Terrain.json` |
| `DEMO.3DR` | `Recording.json`, each record's time, input bits, position and facing |

A GAF's folder holds what sprite importers expect, every image with the colour key transparent and each RGB555 channel widened as `(c << 3) | (c >> 2)`:

- **One sheet**, `Name.png`, of every animation: a grid of `Columns` by `Rows` cells of `CellWidth` by `CellHeight`, one size for the whole GAF. Each animation starts a new row at its `FirstCell` and runs left to right, top to bottom, one cell per frame; one whose frames draw nothing takes no cells and has `FirstCell` -1. Each frame is drawn as the game draws it, at its offset plus its bitmap's trim and mirrored when the frame is, so the sprite's anchor is at (`PivotX`, `PivotY`) in every cell. A frame added to an animation needs a free cell, so move its `FirstCell` past the others or leave it at the end.
- **A page per font**, `FontN.png`, with its BMFont text file `FontN.fnt` (the base bitmap of each glyph; the overlays stay in the JSON), packed as BMFont expects; and `Bitmaps.png` for bitmaps no animation or font shows, an even grid in bitmap order with each one at its trim inside its cell, so frames cut from one sprite line up. Each page's `Places` give each bitmap's corner.
- **`SoundN.wav`**, the SMP_ samples as 16-bit PCM, exact for 8-bit and 16-bit sources.
- **`Name.json`**: the sheet's layout, each bitmap's size and trim, each animation's first cell and frames (bitmap, offset, duration in ticks and ms, mirroring, draw flags and blend, events, sound), the sounds, and each font with its glyphs.

The Tiled map has one tile layer per `MAP_` layer over the embedded tileset (the `.3DG` blocks, each with its `Depth`), and four object layers: `Objects`, every `.3DO` object at its `Position` with its other fields as string properties, nested blocks as a JSON array in `Blocks`, and `Spawn = Player` on the player; `PatrolPaths`, each `.3DP` path as a polyline with its `Id`; `MapAnimations`, each animated tile at its cell with its GAF JSON and animation; and `Collision`, the static terrain shapes the port builds, in world pixels, as lines or polygons. The `.3DO` header fields are map properties. `Terrain.json` keeps what Tiled has no place for: the `.3DX` GAF list (`Gafs`, each editor path as written and its extracted JSON), its descriptors (`Gaf` 0 with a bitmap path as `Name`, or a 1-based `Gafs` entry with an animation name; `Type` is the folder above that bitmap or GAF, shown for reading), and the `.3DI`'s four slots per cell, row-major.

`Manifest.json` at the root lists, per archive, its source path and text encoding, and per entry every file made from it with an FNV-1a hash.

## Compress

`--compress` walks each source archive in its own order. An entry whose files all still hash the same copies the source's bytes. An edited one converts back: a WAV as it is, a text into its code page, a PNG into a Targa in the original's form (its depth, 16-bit A1R5G5B5 or 24-bit, its row order, run-length coding and TGA 2.0 footer), a class library's JSON into the block grammar, `Recording.json` into the demo's record count and 20-byte records, and GAFs and levels as below. A file that cannot convert back names its entry and fails the run rather than write a broken archive.

A GAF is rewritten over the original, so what the extracted files do not describe stays as it was:

- **Sheets and pages**: a cell that still shows its original bitmap leaves it alone. An edited cell is trimmed to its opaque pixels and becomes the bitmap, un-mirrored, with its size and trim set so it is drawn exactly where it was painted; the frame records and rectangles the GOB_ entry keeps per bitmap follow. A pixel under half opaque becomes the colour key, and an opaque one that lands on the key moves to the nearest blue. A bitmap that several frames or pages show must be edited the same in each, or the run names both places. Font and leftover pages keep each bitmap's size. The bitmap count is fixed, and the JSON's bitmap sizes and trims are for reading.
- **Animations**: rebuilt from the JSON, so frames and animations can be added, removed and renamed. `Flags`, `DrawFlags`, `Duration` and the event fields are what packs, except that `Loop`, `PingPong`, `Reverse`, `MirrorX` and `MirrorY` set their bits; `Blend`, `DurationMs` and `Sound` are for reading. Offsets are as drawn, mirrored ones already negated.
- **Fonts**: each font's settings and glyphs (by `Byte`) from the JSON; the font count and each font's glyph count are fixed. The `.fnt` files only mirror the JSON, so editing one fails with a message.
- **Samples**: an edited `SoundN.wav` (8-bit or 16-bit PCM) replaces that sample as 16-bit PCM.

A level is rebuilt whole from its `.tmj`, `Tileset.png` and `Terrain.json`. The tileset's tiles become the `.3DG` blocks (tiles can be added; a new one takes its `Depth` property or the first block's), the tile layers its `MAP_` layers (saved as CSV, no flipped tiles), the `Objects` layer the `.3DO` in order with `OBJECT-COUNT` counted again, `PatrolPaths` the `.3DP`, and `Terrain.json` the `.3DX` and `.3DI`. The map's `WorldExtentX` and `WorldExtentY` must give its width and height (`extent / cell + 1`). `Collision` and `MapAnimations` only show what the terrain makes and are not read back: change the terrain in `Terrain.json`.

What the original holds a rebuilt level to, each found by running it:

- **Terrain stays off the last column and the last two rows.** `LevelBuildWalkGrid` (`0041ed30`) marks each terrain cell in a grid exactly the map's size and also in the next column and the next two rows, so terrain there writes past the grid and crashes the game; no original level has any. `--compress` refuses such a `Terrain.json`.
- **Each `MAP_` layer record gives the layer's size in bytes** (`width * height * 4`) and its width and height; `GafParseSlot3` (`0047bdb0`) copies that many bytes of cells per layer, so a resized map with the old records crashes or draws holes.
- **The text files keep the original's form:** the `.3DO`, `.3DP` and `.3DX` with bare LF line ends (a CR crashes the game), the `.3DC` with CR LF; the `.3DO` header fields as `NAME [value]`, a class library's top-level fields as `name = value` (the bracket form is a syntax error there).

The GAF format's own facts the writer follows: each `HEAD` entry is a 36-byte descriptor (slot 0 record count and size, slot 2 count and record size, the `DATA` entry's file offset, which `GafReadData` (`00453c90`) seeks, and slot 3's size); `GafReadSlotStream` (`0049ae30`) reads a stored slot's exact size but a coded one's size rounded up to 4. Every slot written is LZSS coded with a greedy hash-chain match search; matches only reach back into bytes already written, since the decoder's ring tail before them is left from an earlier call. A full rebuild of `vz-data.rsc` is 52.4 MB against the original's 51.9 MB, the original's LZSS coder finding slightly longer matches, and of `Vz-slide.rsc` 15.54 MB against 15.58 MB.

The archive it writes has unencrypted tables (key 0) behind the `_KX_` magic and its data right after the name table, which `RscOpen` (`004551a0`) requires (`DataOffset` must be where the name table ends); it finds entries through a sorted hash index, so their order does not matter to it. The original exe starts a new game on a `vz-data.rsc` with every entry rebuilt, and plays a level cut down to 20 by 40 cells. A test copy of the game must point the `rsc000` value (the CD installer left it in `HKLM\SOFTWARE\WOW6432Node\c2v\Vyruz`) at itself, as it does `datapath`, or the exe reads the installed folder's archive instead.

`--verify-extract` extracts the game folder, packs the tree back and checks that every entry reads back byte for byte; then it rebuilds every class library, GAF, level and the demo from its files and checks that what the game reads from each is the same (blocks; each image's pixels, and its bytes unless it was run-length coded; every decoded GAF slot and table-of-contents word, samples aside; bitmaps, animations, samples and fonts; the demo byte for byte; and the loaded level, apart from `.3DI` records naming no descriptor, which the loader drops anyway); then it edits a text, an image and a sprite sheet and checks all three come back through their converters, the sprite's painted pixel trimmed into its bitmap and drawn where it was painted.
