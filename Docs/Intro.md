# The intro

`Intro.avi` sits beside `vz-data.rsc` in the game folder (`%s\intro.avi` at `0050e0a8`). It is 103 seconds of Indeo Video 5 at 320x180 and 30 frames per second, with 22050 Hz stereo MP3 sound. The original hands it to Windows through MCI; the port reads the container and decodes the picture itself, from this description.

## When it plays

`PlayIntro` (`004033b0`) plays it in three places:

- `VzRun` (`00446e20`) at startup, after `ConfigLoad` and before the main menu.
- `VzMenuLoop` (`00447820`), case 10, unless session flag `0x2000` is set.
- The Extras menu (`MenuExtras`, `00427820`), item 2, while `IntroPresent` (`00403370`) finds the file. The item's help reads `Intro animation file is not present.` otherwise, or `Install Indeo 5.11 or higher to see the intro.` when `IndeoInstalled` (`00403030`) finds no Indeo codec.

It stops the music (`MusicRestore`) for the video and starts it again afterwards if a tune was playing. The video plays until it ends or a key is pressed: the wait loop polls input (`PgPoll(0x1ff7f)`) and leaves when `DAT_00526da8`, the last key, is set.

Two paths play it. The default opens the `avivideo` MCI device (`AviOpen`, `0047c6f0`) as a child window of the game's window. It asks MCI for the source size and fits the video into the 640x480 screen with its aspect kept and centred, then moves the window there (`AviMoveWindow`, `0047cbc0`). The right edge is computed as `left + width - 5` where the bottom is `top + height - 1`, so the window is 4 pixels narrower than the fit: the intro shows at (0, 60), 636x360. With option flags `0x100` or `0x200` (session `+0x27e0`) in full screen, `PlayIntroFullScreen` (`00403610`) instead switches the display mode for the movie and plays it through a second library (`c2vavilib`); a crash there leaves flag `0x200` saved in `vzconf.txt` so the next start falls back to MCI.

## The container

A RIFF `AVI ` file. `hdrl` holds `avih` (33333 microseconds per frame, 3092 frames, 320x180) and two `strl` lists: the video stream (`vids`, handler `IV50`, a 40-byte `BITMAPINFOHEADER` with compression `IV50` and 24 bits per pixel) and the sound stream (`auds`, format `0x55`, MP3 at 22050 Hz, 2 channels, 128 kbit/s). `movi` holds the chunks in time order: `00dc` for each video frame and `01wb` for pieces of the MP3 stream. `idx1` follows.

Chunks 1 and 2 of the video are empty: a frame of size zero repeats the one before. Every other frame is a complete picture.

## Indeo Video 5

Intel's codec, `Ir50_32.dll` 5.11, ships beside the game. Only what `Intro.avi` uses is described here, which is the codec's simplest profile: every frame is an intra picture, each plane is one band and one tile, and no macroblock carries a quantiser change or a motion vector. A decoder for other files would need the inter pictures, motion compensation, wavelet bands and tiles this one rejects.

### Bits

Frames are read as a little-endian bit stream: bits come from each byte starting at its lowest bit, and a field of n bits takes its first bit as its lowest. Alignment skips to the next byte.

### Picture header

| Bits | Field |
|---|---|
| 5 | Start code, `0x1f` |
| 3 | Picture type: 0 intra; 1 to 4 are the inter types, which the intro never uses |
| 8 | Picture number |
| | Intra pictures: the GOP header |
| 8 | Picture flags (`0xd5` in every intro frame) |
| 24 | With flag `0x01`: picture size in bytes |
| 16 | With flag `0x10`: checksum |
| | With flag `0x20`: an extension, bytes of length then data until a zero length |
| | With flag `0x40`: the macroblock codebook (see Codebooks); without, the default |
| 3 | Unknown |
| | Align |

### GOP header

| Bits | Field |
|---|---|
| 8 | GOP flags (`0x81` in the intro) |
| 16 | With flag `0x01`: header size |
| 32 | With flag `0x20`: lock word of a protected clip |
| 2 | With flag `0x40`: tile size, `64 << n` |
| 2 | Luma wavelet levels: the plane has `3n + 1` bands |
| 1 | Chroma wavelet level, the same way |
| 4 | Picture size index; 15 means two 13-bit fields follow, height then width |
| | For each band of luma, then each of chroma: |
| 1 | Half-pixel motion |
| 1 | Macroblock size: 0 is twice the block size, 1 the block size |
| 1 | Block size: 0 is 8, 1 is 4 |
| 1 | Extended transform information (must be 0) |
| 2 | End marker (must be 0) |
| | With flag `0x08`: 3 zero bits, then a transparency bit and with it a 24-bit fill colour |
| | Align |
| 23 | Unknown |
| 1 | Extension: 16-bit words follow while bit 15 is set |
| | Align |

Flag `0x02` marks a YV12 picture, which the intro is not. The intro uses index 15 (180 by 320), one band per plane, no tiles and no transparency. Luma has 8x8 blocks in 16x16 macroblocks; chroma has 4x4 blocks in 4x4 macroblocks.

### Planes

The picture is YVU9: a luma plane at full size and two chroma planes at a quarter of each dimension, rounded up (80x45 here). The bit stream sends luma, then V, then U. Each plane is stored as signed 16-bit samples around zero; output adds 128 and clamps to 0..255. Macroblocks cover the plane in raster order and may run past its edge (luma rounds up to 192 rows, chroma to 48), so the planes are decoded into buffers rounded up to the macroblock size.

### Band header

| Bits | Field |
|---|---|
| 8 | Band flags |
| | Flag `0x01`: the band is empty and nothing else follows |
| 24 | With picture flag `0x80`: band size in bytes |
| 8 | With flag `0x10`: number of corrections, then two bytes for each |
| 3 | With flag `0x40`: run/value map, else map 8 |
| | With flag `0x80`: the block codebook; without, the default |
| 1 | Checksum present, then 16 bits of checksum |
| 5 | Global quantiser |
| | With flag `0x20`: align, then an extension as in the picture header |
| | Align |

Flag `0x02` inherits the luma band's macroblock types and vectors, `0x04` says macroblocks carry quantiser changes and `0x08` inherits them. The intro's bands use flags `0x80`, `0x90`, `0xc0`, `0xd0`, `0x92`, `0xc2` and `0xd2`, so quantiser changes never appear and only chroma inherits, which in an intra picture only means every macroblock is intra.

Each correction is a pair of symbol indices whose entries in the band's run/value map are swapped for this band only, end-of-block and escape symbols included.

The checksum is the 16-bit sum of the band's decoded samples over the plane's width and height.

### Tile

| Bits | Field |
|---|---|
| 1 | Empty tile (never, in an intra picture) |
| 1 | Size present, then 8 bits of size, and when those are 255 a further 24 bits of size |
| | Align |
| | Macroblock information, then align |
| | Block data, then align |

The size counts bytes from the tile's first bit to the end of its block data.

### Macroblock information

For each macroblock: 1 bit that must be 0 in an intra picture, then one coded-block bit for each block, the lowest for the top left, then top right, bottom left and bottom right. With quantiser changes on, a change would follow. Intra macroblocks carry nothing else.

### Codebooks

A codebook is described by up to 16 rows, each with a bit count. Row `r` holds `2^bits[r]` codes: `r` one bits, a zero bit unless it is the last row, then `bits[r]` bits of index, its highest bit first. Symbols are numbered through the rows in order and stop at 256.

A coded codebook is 3 bits of selection: 0 to 6 pick one of the predefined codebooks, 7 describes one, 4 bits of row count then 4 bits for each row. The default is predefined codebook 7. `Ir50_32.dll` keeps the eight macroblock codebooks at `1008bb50` and the eight block codebooks at `1008bbf0`, each a 32-bit row count and 16 bytes of bit counts:

| | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| Macroblock | `0 4 5 4 4 4 6 6` | `0 2 2 3 3 3 3 5 3 2 2 2` | `0 2 3 4 3 3 3 3 4 3 2 2` | `0 3 4 4 3 3 3 3 3 2 2 2` | `0 4 4 3 3 3 3 2 3 3 2 1 1` | `0 4 4 4 4 3 3 3 2` | `0 4 4 4 4 3 3 2 2 2` | `0 4 4 4 3 3 2 3 2 2 2 2` |
| Block | `1 2 3 4 4 7 5 5 4 1` | `2 3 4 4 4 7 5 4 3 3 2` | `2 4 5 5 5 5 6 4 4 3 1 1` | `3 3 4 4 5 6 6 4 4 3 2 1 1` | `3 4 4 5 5 5 6 5 4 2 2` | `3 4 5 5 5 5 6 4 3 3 2 1 1` | `3 4 5 5 5 6 5 4 3 3 2 1 1` | `3 4 4 5 5 5 6 5 5` |

The intro codes macroblock codebook 0 in every picture, which its pictures never read, and uses all eight block codebooks and described ones.

### Run/value maps

A block symbol stands for a run of positions to skip and a value. Nine maps translate them; the DLL keeps them at `1008de68`, `0x590` bytes apart, in a compact form that `RunValueMapExpand` (`10036630`) expands:

| Offset | Size | Field |
|---|---|---|
| `+0x00` | 4 | Number of runs, R |
| `+0x05` | R | For runs 1 to R, the largest value with that run |
| `+0x48` | 4 | Number of symbols, 256 |
| `+0x4c` | 256 | For each symbol, its entry in the expanded list |

The expanded list starts with the end of block (entry 0) and the escape (entry 1), then for each run from 1 up, its values from the most negative to `-1`, then from `1` to the largest. With map 0 that is run 1 with values -40 to 40 at entries 2 to 81, run 2 with -14 to 14 from entry 82, and so on.

### Block data

Each block of a coded block bit reads symbols until the end of block:

- An escape is followed by three more symbols from the same codebook: the run minus one, then the low 6 bits and the high bits of a value `v` in which the lowest bit is the sign: an odd `v` is `(v + 1) / 2`, an even one `-v / 2`.
- Any other symbol gives its run and value from the band's map.

The position advances by the run, starting from just before the first, and takes the coefficient at that point in the scan: zigzag for 8x8 blocks (`1009d5a8`) and `0 1 4 8 5 2 3 6 9 12 13 10 7 11 14 15` for 4x4 (`1009d668`). Going past the block's last coefficient is an error.

The value is dequantised by the coefficient's step `q`. The macroblock's quantiser, the band's global quantiser clamped to 0..23, picks a scale `s` from the band's scale table, and `q = (b * s) >> 8` for the coefficient's base `b` (`DequantTablesBuild`, `100013f0`). The DLL clamps `q` to 1..255; a step of 0 or 1 leaves the value alone, and a larger one makes it `v * q + sign(v) * ((q ^ 1) - 1) / 2`. Intra blocks use these tables: of the DLL's five 8x8 sets, luma takes the one for a plane of a single band, and chroma takes the 4x4 one.

| | Address | |
|---|---|---|
| 8x8 base | `10091238` | 64 bytes |
| 8x8 scale | `10091558` | 24 32-bit |
| 4x4 base | `100914b8` | 16 bytes |
| 4x4 scale | `10091918` | 24 32-bit |

Every block is intra and so predicts its DC coefficient from the block before it in the band: the band keeps a running DC, which starts at 0 for each tile, adds each coded block's DC coefficient and becomes it. A block without a coded bit is flat: every sample is `(dc + 1) >> 1` of the running DC.

### Inverse transform

Both block sizes use a two-dimensional inverse slant transform, columns first, then rows; the row pass halves its results, rounding up (`(x + 1) >> 1`). Each pass is a one-dimensional transform of eight (or four) inputs `x0` to `x7`, built from butterflies `B(a, b) = (a + b, a - b)` and two kinds of rotation:

- `R(a, b) = (a + ((a + 2b + 2) >> 2), ((2a - b + 2) >> 2) - b)`
- `P(a, b) = (b + ((4a - b + 4) >> 3), a + ((-a - 4b + 4) >> 3))`

Eight points:

1. `(t4, t5) = P(x1, x3)`
2. `(t1, t5) = B(x0, t5)`, `(t2, t6) = B(x4, x5)`, `(t7, t3) = B(x7, x6)`, `(t4, t8) = B(t4, x2)`
3. `(t1, t2) = B(t1, t2)`, `(t4, t3) = R(t4, t3)`, `(t5, t6) = B(t5, t6)`, `(t8, t7) = R(t8, t7)`
4. `(t1, t4) = B(t1, t4)`, `(t2, t3) = B(t2, t3)`, `(t5, t8) = B(t5, t8)`, `(t6, t7) = B(t6, t7)`
5. The outputs are `t1` to `t8` in order.

Four points:

1. `(t1, t2) = B(x0, x2)`, `(t4, t3) = R(x1, x3)`
2. `(t1, t4) = B(t1, t4)`, `(t2, t3) = B(t2, t3)`
3. The outputs are `t1` to `t4` in order.

## In the port

`Source/Pg/Avi` reads the container, `Source/Pg/Indeo5` decodes the pictures and miniaudio decodes the sound, which plays on the music channel at full volume. The port starts the intro at startup when the main menu shows, before the idle demo, and from Extras, and draws it where MCI put it. `--verify-intro` decodes every frame of `Intro.avi` and checks each band against its checksum. The decoded frames were also compared with FFmpeg's, frame by frame, while writing it.
