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
2. The elapsed time is consumed in sub-steps of `min(remaining, g_FramePeriodMs)`. The step length is stored at session `+0x101c` and drives all movement; session `+0x2744` gets `int(14.0 / step)`.
3. Each sub-step: debug/cheat keys, demo record/playback, pause menu, then the game update `FUN_004406e0` (returns -1 restart, 0 level end, 1 continue), messages, scroll and object updates.
4. After the sub-steps, once per frame: camera and scroll position (clamped to the map), screen shake, map/scroll draw (`FUN_00469820`), objects (`FUN_00433b30`), HUD and overlays, cursor, flip.

`g_FramePeriodMs` comes from `PgMeasureRefreshRate` (`004504a0`): it times 20 vsync flips, keeps the samples between 3 and 33 ms, and stores `1000000 / (refresh * 1000)`, e.g. 16 ms at 60 Hz (1000000 if no rate could be measured). Recording or playing a demo (`.3DR`) uses a fixed 10 ms step cap instead.

The port therefore cannot use a fixed tick and stay faithful: it must reproduce the variable millisecond step capped at the display period (or 10 ms for demos).
