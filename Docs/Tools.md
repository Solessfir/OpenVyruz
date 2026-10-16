# Tools and checks

Each tool comes first on the command line, runs on its own and exits; anything else on the line is an error.

## Modding

`--extract <game folder>` writes the archives as modern files (evenly spaced PNG sprite sheets per animation, JSON, BMFont, WAV, UTF-8 text and Tiled maps) to an `Extracted` folder beside it. `--compress <Extracted>` packs such a tree back, edited sprites, sounds, fonts, texts, images, classes, Tiled levels and the demo included, into archives the original and the port read. `--verify-extract <game folder>` checks the round trip. See [Extract.md](Extract.md).

## Checks

Each check exits non-zero on any failure.

- `--verify-gaf` decodes every `.gaf` in `vz-data.rsc`, including animation frames, hotspots, mirror flags, names and embedded sound samples, the `.tga` story and ending images, the slides of `vz-slide.rsc` when it is there, and the `.wav` sounds.
- `--verify-levels` loads all 25 levels (`.3DX`, `.3DI`, `.3DO` with resolved `.3DC` class properties, `.3DP` patrol paths, `.3DG`), checks their object and shadow GAFs and animation indices, decodes their animated map GAFs, and parses every object's `Behaviour`.
- `--verify-game` runs the frame-loop, terrain-collision, route-search and player-movement self-checks and replays `CAVE\DEMO.3DR` through the ported player movement. It fails if fewer than 80% of steps match the recording exactly or fewer than 90% within half a pixel ([PlayerMovement.md](PlayerMovement.md)). It also runs the combat checks:
  - Every level's player weapons load, and the player and the turrets fire for a few seconds.
  - Destroyed objects drop their contents, every level's first turret leaves a wreck before it is gone, and nine in ten generators give birth within 20 seconds.
  - Each level puts the player in view of an armed cop, which must route to it and fire.
  - Every level's cops run for 30 seconds: at least half of the patrollers with a `.3DP` path must reach their next patrol point, and at least half of the random movers that found open ground must leave their cell.
  - Every level's sensing gates must close with the player away and open for the player, and the player must ride every usable lift to a level that loads.
  - A difficulty changed during a level must leave every value as a level started on it would.
- `--verify-intro` decodes every frame of `Intro.avi` beside the archive, checking each picture against its band checksums, and its MP3 sound ([Intro.md](Intro.md)).

## Development aids

`--dump-gaf <archive path>` lists an archive GAF's animations with their frame counts, flags and durations. `--start X,Y` puts the player at a world position as the level starts.

In a level, N starts the next of the 25 levels (the list goes to the console at startup) and wraps to the first after the last, without a carried player, for checking every level. A level without a player scrolls with the arrow keys instead.
