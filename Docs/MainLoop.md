# Main Loop

Traced from `Vyruz.exe` 1.31.01 in Ghidra. Names are the ones applied in the Ghidra project.

## Startup (`VzRun` at `00446e20`)

1. Config and command line checks, `VzResolveRscPath`; window title `Vyruz 1.31.01`.
2. Platform init (video, timer, mouse), `VzOpenRsc`; "Cannot load resource file" if the archive is missing.
3. `VzCreateTypeTable` (`00447430`) registers the semantic type names (terrain types 1-15, then object behaviours such as `Vyruz`, `Turret`, `Spider`).
4. Session object created, `.svz.log`, preferences ("pref file is missing" aborts), temp directory setup.
5. If a level name is given on the command line: `VzLoadLevel` (`0041f7c0`), "Cannot load level" on failure.
6. `VzRunSession` (`00446780`) -> `LoopLevels` (`004467d0`): per level, `VzPlayLevel` (`0041e9e0`) loads the `.3DG` and runs `EngineRunLevel`; level transitions loop until the session ends.
7. Shutdown in reverse order.

## Level loading

`VzLoadLevel` lowercases the path, resolves the zone data folder (`<zone>\Data`), then `LevelLoadFiles` (`00429710`):

1. `.3DX` (`LevelLoad3dx`, `00429d80`): animated map GAFs, then the tile descriptor list (index 0 is a null placeholder).
2. `.3DI`: world extents, `LevelAllocGrid` (`00429390`) allocates a column-major `width x height` grid of 12-byte cells, records are classified by `Level3dxResolveEntry` (`00429460`) into one of four slots. Out-of-range tile IDs only log "Reference to missing image filename" once.
3. `.3DP`, then `.3DO` (both through `3do::FUN_00413d80`); a difficulty variant `<zone>\<name>-<suffix>.3do` is used when one applies.

The `.3DG` is loaded later by `LevelLoad3dg` (`0041eae0`) through the generic `GafLoad` (`00453960`). Its path is the `.3DO` `3DG-FILENAME` when set, otherwise `<name>.3dg`. The decoded level `.3DG` is cached in `g_CachedLevel3dg` across restarts of the same level.

## Frame loop (`EngineRunLevel` at `0043e5f0`)

There is no fixed tick. Each frame:

1. `PgPoll(0x1fbff)` runs the platform pollers (input, joystick, sound) and returns the milliseconds since the previous frame (`PgVideoElapsedMs`, `timeGetTime` based).
2. The elapsed time is consumed in sub-steps of `min(remaining, g_FramePeriodMs)`. The step length is stored at session `+0x101c` and drives all movement; session `+0x2744` gets the float `14.0 / step`.
3. Each sub-step: debug/cheat keys, demo record/playback, pause menu, then the game update `FUN_004406e0` (returns -1 restart, 0 level end, 1 continue), messages, scroll and object updates.
4. After the sub-steps, once per frame: camera and scroll position (clamped to the map), screen shake, map/scroll draw (`FUN_00469820`), objects (`FUN_00433b30`), HUD and overlays, cursor, flip.

`g_FramePeriodMs` comes from `PgMeasureRefreshRate` (`004504a0`): it times 20 vsync flips, keeps the samples between 3 and 33 ms, and stores `1000000 / (refresh * 1000)`, e.g. 16 ms at 60 Hz (1000000 if no rate could be measured). Recording or playing a demo (`.3DR`) uses a fixed 10 ms step cap instead.

The port therefore cannot use a fixed tick and stay faithful: it must reproduce the variable millisecond step capped at the display period (or 10 ms for demos).

## Level start

`EngineRunLevel` reseeds the random generator before a demo is recorded or played back (`DemoStartRecording`, `00428fc0`, and `FUN_00429110`), then `EngineActivateObjects` (`0043c700`) visits the grid from its last row and column back to the first, each cell listing its newest object first. `ObjectActivate` (`0043c150`) draws `RandomRange(0, 256)` once per object for its bob phase (`2 pi / draw`; a zero draw gives an infinite phase, and that object never leaves its average altitude) and shows the bob's first altitude. In normal play `LevelStartMapAnimations` (`0041f0b0`) has drawn before that, after `VzLoadLevel`'s reseed: one `RandomRange(1, frames)` start frame per `.3DX` entry whose GAF sits in an `Anim` directory. The port makes the activation draws but not the map animation draws, so normal play's sequence is still off by those.

## Object passes

`EngineVisitViewObjects` (`00433b30`) visits the objects of the grid cells around the view: from one column left of it and two rows above it, 13 columns and 20 rows, clamped to the grid. Objects elsewhere are neither updated nor drawn. It runs twice:

- After every sub-step, as the update pass: each object's bob plays (`ObjectSelectBobMode`, `00433f40`, and `ObjectPlayBob`, `0043c090`), then `ObjectDraw` (`00431ae0`) updates its animations without drawing.
- Once per frame, as the draw pass. `ObjectDraw` advances animations again by the last sub-step before drawing, except for classes with `Graphic-update-needed`, which only the game update advances.

The bob comes from `Bounce-period`, `Bounce-amplitude` and `Bounce-average-altitude` (the object's altitude when zero): `altitude = trunc(sin(t / period + phase) * amplitude) + average`, with `t` in milliseconds. A static object plays it on with `Vertical-bounce-when-static`, and otherwise winds `t` back while `t / period` is above `-pi / 2`. A moving object with `Vertical-bounce-when-moving` plays it while `t / period` is below `pi / 2` and then holds. The bob changes the altitude that the object's shots start from. Cops (amplitude 2, period 200), helis (20 and 1000) and pickups (5 and 500) bob.

`ObjectDraw` shows most actor poses as a frame of a fixed animation:

| Kind | Body | Shadow |
|---|---|---|
| Cop | animation 25 (`rot`), frame = pose of 48 | frame = pose |
| Jeep | animation `GAF-index + pose - 1` | frame = pose |
| Tank | animation `pose` | frame = pose |
| Heli | animation `GAF-index + tilt`, frame = pose | frame = pose |
| Walker | walk `GAF-index + pose - 1` | first frame |
| Spider | walk `GAF-index + (pose - 1) / 6` on a diagonal while driving forward, else animation 5 (`rot`), frame = pose | `Shadow-index + (pose - 1) / 6` |

A walker advances its walk only while driving forward or in the pass where its pose changed; a new pose continues from the frame the previous pose's walk had reached. A spider's walk restarts whenever it shows its turning pose. The heli tilt counter moves in both passes. Every actor animation loops (ANM flags 7).
