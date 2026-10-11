# Vyruz Data Formats

Reverse-engineered formats of the original game data, moved from the original data repository (`the original-data repository`). Python extraction scripts referenced here live in that repository under `Scritps\`.

## Zones and Game Data
- `COMMON` (shared weapons/enemies/UI/sound)
- `UNDERGROUND` (T1U1–T3U3, 7 levels)
- `CAVE` (T1C1–T3C3, 9 levels + `DEMO`)
- `HITEK` (T1H1–T3H3, 9 levels)
- `CPU` (T2P1–T3P2, 3 levels)

Level name format: `T[tier][zone][index]` - `T2C1` = Tier 2, Cave, level 1.<br>Zone letters: `U` = Underground, `C` = Cave, `H` = Hitek, `P` = CPU.

## .RSC - Archive Container

Two archives: `vz-data.rsc` (game data), `Vz-slide.rsc` (cutscene slides), both in `Game\vyruz\`. Flat file archive, stores full relative Windows paths. Unpacked by `extr_rsc.exe` (separate tool, not in this repo, not shipped with the game - required again only if re-extracting from scratch). The native port reads it directly (`Source\Pg\ResourceArchive.cpp`, from `RscOpen`/`RscFileRead` at `004551a0`/`004559f0`).

32-byte header, all little-endian:

| Offset | Size | Field |
|---|---|---|
| `+0x00` | 2 | Entry count (477 in `vz-data.rsc`) |
| `+0x02` | 1 | Non-zero: magic at `+0x1C` must match |
| `+0x03` | 1 | Key: non-zero encrypts the tables, `1` also inverts flagged entry data |
| `+0x04` | 4 | Entry table offset |
| `+0x08` | 4 | Entry table size (count * 12) |
| `+0x0C` | 4 | Name table offset |
| `+0x10` | 4 | Name table size |
| `+0x14` | 4 | Data start (end of name table) |
| `+0x18` | 4 | Data size |
| `+0x1C` | 4 | Magic `"_KX_"` |

Both tables decrypt as `byte[i] = ~rotr8(byte[i], i & 7)`. Entry (12 bytes): `int16` name offset into the name table, `uint8` unused, `uint8` flags (bit 0 = data stored bitwise-inverted), `uint32` offset from data start, `uint32` size. Names are NUL-terminated, matched case-insensitively. `UNDERGROUND\DATA\OBJECT\TURRET\V-TURRET-GUN-UDG.GAF` appears twice with identical bytes.

## Generic GAF Record Format

Used by every `.GAF` and `.3DG` file. 16-byte header, repeats recursively (container tags hold child records of the same format):

| Offset | Size | Type | Field |
|---|---|---|---|
| `+0x00` | 4 | `char[4]` | Tag: `"GAF_"`, `"HEAD"`, `"DATA"`, `"GOB_"`, `"ANM_"`, `"SMP_"`, `"NAM_"`, `"BLK_"`, `"MAP_"`, `"BMP_"` |
| `+0x04` | 4 | `uint32` | Payload size in bytes (excludes this 16-byte header) |
| `+0x08` | 4 | `uint32` | Version magic, always `0x19990208` |
| `+0x0C` | 4 | `uint32` | Extra - child/entry count |

Top-level structure:
```
GAF_                                  @ 0x00
HEAD  - table of contents             @ 0x10
  extra = N declared object types (e.g. 3: GOB_, ANM_, NAM_)
  each TOC entry = 16B sub-header + fixed 36-byte summary body (not real data)
DATA  - real payload                  @ right after HEAD (offset varies per file)
  extra = N real entries, same tag set as HEAD's TOC
  entries contain the actual descriptors + compressed data
```

## LZSS Compression

Okumura-variant LZSS. Used for every compressed stream inside `GOB_`/`BLK_`/`MAP_` entries.

| Parameter | Value |
|---|---|
| Ring buffer size | 4096 bytes |
| Ring buffer init | `0x20` (all bytes) |
| Ring start position | 4078 (`0xFEE`) |
| Flag bit order | LSB-first (bit 0 processed first) |
| Flag bit meaning | 1 = literal byte, 0 = match token (2 bytes) |
| Match offset | `byte1 \| ((byte2 & 0xF0) << 4)` |
| Match length | `(byte2 & 0x0F) + 3` |

## BMP_ Pixel Chunk Format

68-byte header + raw pixel data. Used inside decompressed `GOB_` entries (sprite frames) and decompressed `BLK_` entries (level tiles) - identical layout in both cases.

| Offset | Field | Notes |
|---|---|---|
| `+0x00` | tag | `"BMP_"` |
| `+0x04` | SIZE | Pixel data byte length `width * height * 2`, padded up to a multiple of 4; the next `BMP_` starts right after it |
| `+0x08` | signature | Type tag, not a fixed constant - `0x40000122` for single-part sprites, `0x40000322` for composite/multi-part objects, `0x00000311` for `.3DG` tile chunks |
| `+0x0C` | header_ext_size | Constant `56` in every variant |
| `+0x10` | signature | Repeat of `+0x08` |
| `+0x14` | flags/type | `1` |
| `+0x18` | depth | `16` (bpp) |
| `+0x1C` | depth | repeat of `+0x18` |
| `+0x20` | width | `uint32` |
| `+0x24` | height | `uint32` |
| `+0x28` | stride | Runtime/DirectDraw-padded value, `ceil(width/4)*4*2` - **do not use for on-disk reads** |
| `+0x2C` | reserved | `0` |
| `+0x30` | reserved | `0` |
| `+0x34` | width2 | Usually `width - 1` |
| `+0x38` | height2 | Usually `height - 1` |
| `+0x3C` | colorkey | `uint16`, RGB555, always `0x7C1F` |
| `+0x40` | reserved | `0` |
| `+0x44` | pixel data | Raw RGB555, row-major, on-disk stride = `width * 2` (tight-packed, no padding) |

Pixel format: RGB555. `r = (word >> 10) & 0x1F`, `g = (word >> 5) & 0x1F`, `b = word & 0x1F`. Pixel value equal to colorkey → transparent.

## .GAF - Sprite / Animation

Depends on: Generic GAF Record Format, LZSS, BMP_ Chunk Format.

`DATA` holds one `GOB_` entry per sprite. `GOB_` body layout:
```
16-byte GOB_ header
96-byte, 4-slot descriptor
slot 0 LZSS stream          (44-byte frame metadata records)
slot 1 LZSS stream          (back-to-back BMP_ pixel chunks)
slot 2 LZSS stream          (20-byte runtime surface records)
```
Each descriptor slot is 24 bytes:
`[constant=256, 0, 0, compressed_size, decompressed_size, 0]`. Populated
streams follow the descriptor in slot order and start on 4-byte boundaries.

Slot 0 has one 44-byte (`11 x int32`) record per frame. Confirmed fields are
`[1, width, height, 0, trim_x, trim_y, 0, -(trim_x+width),
-(trim_y+height), 0, 0]`. The old extractor discarded `trim_x/trim_y`, causing
tightly cropped directional frames to wobble when centered independently.
`vyruz_gaf_extract.py --atlas ... --canvas 96x96` restores those crops to a
common frame before atlas packing.

`ANM_` slot 0 contains 32-byte animation records: frame count, flags, loop start, and five reserved words. Slot 2 concatenates their 28-byte visual frame records in animation order: 1-based GOB index (zero is invisible), signed X/Y hotspot offsets, draw flags, event flags, event value, and duration. `UpdateAnimation` (`0045e7e0`) negates the hotspot X for draw flag `0x100` and Y for `0x200`.

`NAM_` slot 0 contains 8-byte groups (name count, target tag). Slot 2 contains 12-byte name records (stored string size, 1-based target index, reserved), and slot 3 contains NUL-terminated strings padded to their stored size. Name order is not animation order; resolve each name through its target index.

`DrawAnimationFrame` (`0045eab0`) adds the animation hotspot to the supplied screen position. `DrawGobFrame` (`00466f40`) then adds the GOB crop, using `-(trim_x + width)` or `-(trim_y + height)` for mirrored axes. Object bodies are raised by altitude while shadows stay at ground level. `RadixSortDrawList` (`0047f780`) and `FlushSortedSprites` (`00466c80`) draw ascending depth keys with stable ordering for equal keys. Normal bodies use `ground_y * 4`, shadows use `ground_y * 4 | 1`, and `In-ground` bodies use layer 3. `Z-shadow` shifts the body's depth Y back by 64 pixels, clamped to zero, and uses layer 1 without moving the sprite. `In-ground` takes precedence over that depth shift.

The object draw passes the full animation draw flags to the bitmap renderer. `DrawTexturedPolygon` (`00496280`) implements modes 6/7 as additive `ONE + ONE` and modes 8/9 as `ZERO + INVSRCCOLOR`: shadows darken the existing scene with `destination * (1 - source_rgb)`. Bit 0 enables color keying. Shadow masks must retain their RGB values rather than being drawn as opaque grayscale sprites.

Depth sorting is only the queue order. `LevelCompositeSprites` (`00420070`) first calls `LevelFlushSpriteLayer` (`0041fec0`) for layer 3 with a threshold beyond the visible region, drawing all `In-ground` decals before the remaining sprite layers. It then interleaves terrain with layers 1, 2, and 0. A flat sort across all four layers incorrectly draws floor decals over nearby shadows, producing bright speckles.

Some animation GAFs also carry an `SMP_` entry containing uncompressed sound
effects. Each sample is a `SND_` record with this 36-byte header followed by
interleaved PCM (unsigned 8-bit or signed little-endian 16-bit):

| Offset | Size | Type | Field |
|---|---|---|---|
| `+0x00` | 4 | `char[4]` | `SND_` |
| `+0x04` | 4 | `uint32` | Padded PCM byte count |
| `+0x08` | 4 | `uint32` | Format magic |
| `+0x0C` | 4 | `uint32` | Metadata size, `24` |
| `+0x10` | 4 | `uint32` | Repeated format magic |
| `+0x14` | 4 | `uint32` | Sample rate |
| `+0x18` | 4 | `uint32` | Bits per sample |
| `+0x1C` | 4 | `uint32` | Channel count |
| `+0x20` | 4 | `uint32` | PCM frame count |

Meaningful PCM bytes are `frame_count * channels * (bits / 8)`; the declared
byte count can include up to two alignment bytes. `EXPLO.GAF` contains four
such samples. Its ANM visual records are seven `int32` values, and the sound
trigger fields map animation index 1 (`Explosion`, `Big-Explosion`, and
`Medium-Explosion`) to sample 4, while animation index 5 (`Small-Explosion`)
maps to sample 1. Both trigger on their corresponding animation frame, not when
the object is first spawned.

Every object GAF has a `V-`-prefixed shadow variant (e.g. `V-TURRET-ROCKET-UDG.GAF`) - smaller, sparser, same format.

Example sizes: `TURRET-GUN-UDG.GAF` 86,508 bytes on disk → 146,804 bytes decompressed (25 `BMP_` frames). `V-TURRET-ROCKET-UDG.GAF` 16,652 → 64,240 bytes (25 frames).

## .3DG - Level Tile Bitmap Sheet

Depends on: Generic GAF Record Format, LZSS, BMP_ Chunk Format.

Loaded by the generic `GafLoad` (`00453960`); `GafReadData` dispatches each DATA entry by tag (`GOB_ ANM_ FNT_ RNG_ SMP_ BLK_ MAP_ NAM_` are type indices 0-7) and `GafParseSlots` (`0047bf00`) decodes the four slot streams for every type. The path comes from the `.3DO` `3DG-FILENAME`, relative to the zone folder. All 25 levels decode with every BLK_ block 64x32, every MAP_ index within the BLK_ count, and MAP_ grid dimensions equal to the `.3DI` formulas (`--verify-levels`).

`DATA` holds two entries: `BLK_` (tile bitmaps) and `MAP_` (tile grid). Each entry body opens with a **96-byte, 4-slot descriptor**, not a single stream like sprite `GOB_`:

```
slot[i] @ i*24: [ constant=256, 0, 0, compressed_size, decompressed_size, 0 ]   i = 0..3
```
Unused slots are all-zero. Streams for populated slots follow the descriptor
in slot order. Advance by `align_up(compressed_size, 4)` after each stream:
`FUN_0049ae30` reads a whole number of dwords, so ignoring this padding corrupts
later slots even when decompression appears to return the expected byte count.

**BLK_**: one populated slot decompresses to a flat, back-to-back array of
`BMP_` chunks - uniform 64x32px blocks, same header format as sprite chunks,
`signature = 0x00000311`. Tile count varies per file (T1C1: 673, T3H3:
634, T2P1: 835, T3U3: 713). Other populated slots are small index/header
tables.

**MAP_ is the exact static visual map.** It is not a semantic tilemap and is
not superseded by `.3DI`. Descriptor slot 0 decompresses to this 24-byte
header:

| Index | Type | Field |
|---:|---|---|
| 0 | `uint32` | visual layer count |
| 1 | `uint32` | original render-cache/viewport width |
| 2 | `uint32` | original render-cache/viewport height |
| 3 | `uint32` | grid width |
| 4 | `uint32` | grid height |
| 5 | `uint32` | reserved; normally 0, `0xCCCCCCCC` in DEMO |

Descriptor slot 3 is exactly
`layer_count * grid_width * grid_height` little-endian `uint32` values. It is
layer-major, then row-major inside each layer. Value `0` is transparent/empty;
a nonzero value is a **1-based index** into the decompressed BLK bitmap table.
Draw layers from 0 upward, alpha-compositing each referenced 64x32 BLK at:

```text
cell_index = y * grid_width + x
position   = (x * 64, y * 32)
```

This reproduces the original static level image exactly. The MAP layers are
render strata, not named gameplay layers: do not assume that a particular
plane means ground, wall, roof, or collision. The header viewport fields are
the original render-cache dimensions and must not be used to rescale the map.

Examples:

| Level | Layers | Grid | Static image |
|---|---:|---:|---:|
| T1U1 | 6 | 80x130 | 5120x4160 |
| T1C1 | 5 | 122x122 | 7808x3904 |
| T2P1 | 6 | 122x122 | 7808x3904 |

## .3DC - Object Class Library

Plain text, line-terminated block grammar with format version `1.3`. Values end
at the newline; they must not be parsed as a whitespace-only token stream.
Supported forms across all shipped `.3DC`, `.3DO`, and `.3DP` files are:

```text
BLOCK-KIND [ ... ]
BLOCK-KIND block-name [ ... ]
field = scalar
field = x, y
DIRECTIVE optional arguments
```

`TYPE Explosion [ ... ]`, `ADD Weapon [ ... ]`, and similar constructs are
named blocks (`kind=TYPE`, `name=Explosion`), not a block called `TYPE` and not
part of the preceding field. Repeated fields and blocks are significant:
patrol paths repeat `Point`, and classes may contain multiple `TYPE Explosion`
or `ADD Weapon` blocks. Bare directives include `HAVE`, `RANDOM`,
`Override-Attributes`, `override-random-objects`, and `override-objects`.
`DIFFICULTY.3DC` is the one library that also uses repeated top-level
assignments.

`DEFAULT-PROPERTY` supplies per-library class defaults. `OBJECT-CLASS` fields include `Name`, `GAF-name`, `Behaviour`, scalar `Bounding-Box-Size`, and optional `Derived-From`. `LoadObjectClassLibrary` (`0040a010`) starts with the library defaults, then replaces the entire class record with the resolved parent's record when `Derived-From` is present, then applies the class fields. Unrelated child-library defaults therefore do not survive inheritance. `.3DO` instance overrides are applied last. Class `TYPE` and `ADD` blocks add repeated attributes; instance `TYPE` overrides select the first unused matching inherited attribute. `Override-Attributes` clears inherited attributes. Missing parents, cycles, duplicate class definitions, and missing instance classes are errors.

`LoadObjectGaf` (`00403fe0`) resolves relative `GAF-name` and `Shadow-GAF` under the current zone's `Data` directory first, then `Common\Data`, regardless of the class library's directory. `GAF-index` and `Shadow-index` are 1-based animation indices; zero disables that animation. Directional behaviours select from consecutive animation indices. `.3DO` `Dir` uses 24 directions: SW=0, S=3, SE=6, E=9, NE=12, N=15, NW=18, W=21, with an optional `_0`/`_1`/`_2` suffix added to the base direction (`ParseObjectDirection`, `00407970`).

`AnimInitializeGraphics` (`00417b30`) gives the player and other 24-direction subtypes consecutive body animations, with a single shadow animation. Heli uses four body animations and one shadow; `HeliSelectBodyPose` (`004334e0`) selects its body pose from movement speed and accumulated state, starting at pose zero. Spider uses four body and four shadow animations, selected by `direction / 6`. Unspecified startup direction is zero (SW).

Known files per zone: `DECOR.3DC`, `ITEM.3DC`, `SPECIFIC.3DC`. For T1U1 the
compiler loads all eight `COMMON` libraries plus those three `UNDERGROUND`
libraries. The resulting graph has 416 classes and resolves all 793 instances
without duplicate definitions, missing parents, or missing instance classes.

## .3DX - Tile / Texture Manifest

Plain text, line-based (not block grammar). Per level. Two sections:
1. Ordered GAF file list (count + paths) - animated map assets used by the level.
2. Texture list (count + entries) - one line per semantic tile descriptor, in
   `.3DI` `tile_id` order (1-based). Each line is `<flag> <ref>`.

`flag = 0` means `ref` is normally a static `.tga` path. A **nonzero flag is a
1-based index into the preceding GAF list**, not a boolean. For an animated
entry, `ref` may be only a short alias and may not equal the GAF basename;
resolve its semantic folder and animation asset through `gaf_files[flag - 1]`.

The file starts with a `1.0` version line ("Unknown .3dx file format" otherwise); `#` starts a comment anywhere on a line. GAF paths are absolute editor paths (`d:\vzed\levels\game\underground\data\anim\obstacle\rightbeton.gaf`); `LevelLoad3dx` (`00429d80`) keeps the last three components under the zone data folder, giving the archive path `UNDERGROUND\DATA\ANIM\OBSTACLE\RIGHTBETON.GAF`. This resolves every GAF of all 25 levels. A descriptor's semantic type is the name of the folder holding its file (`...\bitmap\rightwall\gouffre002.tga` -> `RightWall`), matched against the names registered by `VzCreateTypeTable`; every descriptor of every level resolves to one of the 15 terrain types.

`tile_id` range in a level's `.3DI` always matches its `.3DX` texture count exactly (e.g. T1C1: both 176).

## .3DI - Sparse Tilemap

Binary. Depends on: `.3DX` (`tile_id` descriptor resolution). This is the
semantic/gameplay grid used for collision and animated-map placement. It is
not the exact static visual map; that comes from `.3DG` MAP_ layers.

```
header: 4 bytes
  uint16 world_extent_x
  uint16 world_extent_y
records: repeating 6 bytes until EOF
  uint16 x        (grid column)
  uint16 y        (grid row)
  uint16 tile_id  (1-based, indexes into .3DX texture list)
```

Entry count is `(file_size - 4) / 6`. The header is not an entry count. Its
grid-dimension formulas are:

```text
grid_width  = (world_extent_x >> 6) + 1
grid_height = (world_extent_y >> 5) + 1
world_pos   = (x * 64, y * 32)
```

These formulas match the `.3DG` MAP dimensions in all 25 levels. For T1U1,
header `(5056, 4128)` gives an `80x130` grid. Multiple records may occupy one
coordinate because the loader separates descriptors into four semantic slots.
Records are stored in ascending `(x, y)` order, and every occupied coordinate
satisfies `(x + y) % 2 == 0` in all 25 levels. This is a staggered isometric
lattice. If an editor needs logical `128x64` diamond cells rather than the
native preprojected coordinates, the lossless conversion is:

```text
map_x = (x + y) >> 1
map_y = (y - x) >> 1
```

The original runtime cell is 12 bytes:

| Offset | Type | Semantic slot |
|---:|---|---|
| `+0x00` | `uint16` | slot 0: Ground, Water, Corner, Half-Ground, HorizontalWall |
| `+0x02` | `uint16` | slot 1: Roof, Half-Roof |
| `+0x04` | `uint16` | slot 2: Bridge, Roof+ |
| `+0x06` | `uint16` | slot 3: walls, Obstacle, WallCorner, Ground+Wall |
| `+0x08` | pointer | runtime dynamic/decor linked list; not stored in `.3DI` |

The semantic type IDs registered by `VzCreateTypeTable` (`00447430`) are:

| ID | Type | ID | Type |
|---:|---|---:|---|
| 1 | Bridge | 9 | LeftWall |
| 2 | Corner | 10 | Obstacle |
| 3 | Ground | 11 | RightWall |
| 4 | Ground+LeftWall | 12 | Roof |
| 5 | Ground+RightWall | 13 | Roof+ |
| 6 | Half-Ground | 14 | WallCorner |
| 7 | Half-Roof | 15 | Water |
| 8 | HorizontalWall | | |

Slot assignment follows the semantic type, not source-record position. If a
slot is already occupied, a later record for that cell overwrites it, matching
the original loader.

## Original Static Terrain Collision

The game contains no separate collision-map file. `FUN_0041ed30` derives a
dense directional-marker grid from the four `.3DI` semantic slots, and
`FUN_0041adb0` turns the surviving marker bytes into fixed collision lines.
This is the recovered algorithm:

1. Start a zeroed `uint32[grid_width][grid_height]` marker grid.
2. Traverse cells with `x` as the outer loop and `y` as the inner loop.
3. Process each cell's slots in byte-offset order `+0,+6,+2,+4`, or logical
   slot order `0,3,1,2`. This order matters because later semantics clear bits.
4. For types 2, 3, 4, and 5, OR the following masks at offsets relative to the
   semantic cell:

```text
marker[x    ][y + 1] |= 0x1E
marker[x + 1][y + 1] |= 0x78
marker[x    ][y + 2] |= 0x87
marker[x + 1][y + 2] |= 0xE1
```

Types 6 and 8 OR only the lower `0x87/0xE1` pair. Type 10 clears all four
corresponding masks. Type 7 clears only the lower pair. Type 15 ORs high bit
`0x100` into all four cells; the later edge switch uses only the low byte.

For each marker-grid cell, `FUN_0041adb0` recognizes only exact low-byte values
`0x1E`, `0x78`, `0x87`, and `0xE1`. In native engine coordinates it emits:

```text
0x1E / 0xE1: (64*(x+1), 32*y) -> (64*x,     32*(y+1))
0x78 / 0x87: (64*x,     32*y) -> (64*(x+1), 32*(y+1))
```

When the low byte is zero, the engine also synthesizes edges around slot-3
Obstacle cells:

```text
if y == 0:
    no edge
elif slot3(x, y-1) == Obstacle:
    emit 0xE1 at (x, y)
elif slot0(x, y) == Water and x > 0 and slot3(x, y) == LeftWall:
    emit 0xE1 at (x+1, y)
elif y > 1 and slot3(x, y-2) == Obstacle:
    emit 0x78 at (x, y)
elif x > 0 and slot3(x-1, y-1) == Obstacle:
    emit 0x87 at (x, y)
elif x > 0 and y > 1 and slot3(x-1, y-2) == Obstacle:
    emit 0x1E at (x, y)
```

`FUN_0041aa00` merges compatible neighboring line objects. The resulting fixed
objects are inserted through `FUN_0040f090 -> FUN_0040ee40` into the collision
manager at `session + 0x25AC`; `FUN_004105d0` performs movement-time
broad-phase queries. `FUN_00405df0` is actor-vs-actor AABB scanning and is not
the static terrain rule. A Godot reimplementation was manually validated on
T1U1 against the spawn bunker, entrance, exterior cliff boundary, and gameplay
movement; the recovered lines aligned without a global offset.

## .3DO - Object Placement (Level Layout)

Plain text, block grammar. Depends on: `.3DC` (Class-name resolution), references `.3DG` (`3DG-FILENAME` field).

Top-level tags: `LEVEL-NAME [name]`, `LEVEL-ID [id]`, `OBJECT-COUNT [n]`, `3DG-FILENAME [file]`.

`CREATE-OBJECT [ Class-name = X  Position = X.XXX, Y.XXX  TYPE <name> [ ... ] ]` - one block per object instance. `Class-name` references an `OBJECT-CLASS` in the zone's `.3DC`. `Position` = world X,Y floats. `TYPE` blocks are optional instance-specific overrides. T1U1 has two `TYPE Lift` instances and six `TYPE Patrol` instances covering path IDs 1 through 5.

`OBJECT-COUNT` matches the real number of `CREATE-OBJECT` blocks exactly.

Pickups (keys, ammo, etc.) are ordinary `CREATE-OBJECT` entries here, not `.3DI`.

## .3DP - AI Patrol Paths

Plain text, block grammar. Per level, references the same coordinate space as `.3DO`.

`CREATE-PATH [ Id = n  Count = n  Point = x, y  Point = x, y  ... ]` - ordered waypoint list. `Count` matches the number of `Point` lines. A level may have zero `CREATE-PATH` blocks. The field linking a path `Id` to its enemy spawner is not identified.

## .3DR - Demo/Replay Recording

Binary. One file in the entire game: `CAVE\DEMO.3DR` (645,664 bytes), pairs with `CAVE\DEMO.3DO`. Same coordinate space as `.3DO`.

```
header: 4 bytes  (uint32 record count)
records: repeating 20 bytes
  uint32  timestamp_ms   (elapsed ms since recording start, ~60Hz tick)
  uint32  input_flags    (input/action bitmask)
  float32 pos_x
  float32 pos_y
  uint32  anim_frame     (sprite animation frame index, 0-23)
```
32,283 records, 270.62s duration. `input_flags` bit meanings not decoded.

## Other Formats

| Extension | Notes |
|---|---|
| `.TGA` | Targa images - cutscene slides, story art |
| `.WAV` | Uncompressed PCM |
| `.TXT` | Localized text, EN/FR pairs |

