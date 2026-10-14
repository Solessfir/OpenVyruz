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

`EnginePause` (`0043b8d0`, `action-pause`, P) closes an open map instead; otherwise it sets session `+0x20` bit `0x1000000`, centres `Data\Menu\pause.gaf`'s first animation over the screen in draw mode 7 (`FUN_00403b70`, `FUN_00403d70`), and polls until P goes down again, so the game clock does not advance. The port keeps drawing the frozen level under the picture.

`g_FramePeriodMs` comes from `PgMeasureRefreshRate` (`004504a0`): it times 20 vsync flips, keeps the samples between 3 and 33 ms, and stores `1000000 / (refresh * 1000)`, e.g. 16 ms at 60 Hz (1000000 if no rate could be measured). Recording or playing a demo (`.3DR`) uses a fixed 10 ms step cap instead.

The port therefore cannot use a fixed tick and stay faithful: it must reproduce the variable millisecond step capped at the display period (or 10 ms for demos).

### Inactive

`PgVideoElapsedMs` reads `PgClockNow` (`00469680`), a clock that `PgClockPause` (`00469730`) can stop. When the game loses the activation, the window procedure's `AppActivate` (`004814c0`) pauses that clock (and in full screen restores the display mode and minimizes the window). The message pump in `PgPoll` (`004813c0`) then waits in `GetMessage` until the game is active again. Everything stands still, menus and level alike, and play resumes with no jump in time.

The port does the same in its event poll: while the window is minimized or has lost the focus it waits for window events, with the clock paused and the mixer silent, so it draws nothing and takes no CPU. The swapchain presents in FIFO mode, so frames are paced by vertical sync on both systems. On Wayland a window on a hidden workspace loses the focus, so it never waits in a present that the compositor would not release.

The original took the mouse through DirectInput, so the cursor never left the game. The port confines the cursor to the window while it is active (GLFW's captured mode: `ClipCursor` on Windows, a pointer confinement on Wayland, a pointer grab on X11) and lets go while it is minimized or unfocused. It applies the confinement again each time the game becomes active, since Windows can drop it on some focus changes. Where the game draws its own cursor, the system cursor shows a blank image.

## Level start

`EngineRunLevel` reseeds the random generator before a demo is recorded or played back (`DemoStartRecording`, `00428fc0`, and `DemoStartPlayback`), then `EngineActivateObjects` (`0043c700`) visits the grid from its last row and column back to the first, each cell listing its newest object first. `ObjectActivate` (`0043c150`) draws `RandomRange(0, 256)` once per object for its bob phase (`2 pi / draw`; a zero draw gives an infinite phase, and that object never leaves its average altitude) and shows the bob's first altitude. In normal play `LevelStartMapAnimations` (`0041f0b0`) has drawn before that, after `VzLoadLevel`'s reseed: one `RandomRange(1, frames)` start frame per `.3DX` entry, in file order, whose GAF lies two folders below an `Anim` directory (`Level3dxResolveEntry`'s kind 1 of `Anim`, `Bitmap`, `Object`), for the animation the entry names (an exact match, `FUN_0046c710`); an entry whose name the GAF lacks is dropped without a draw. The port makes both sets of draws.

`LevelStartMapAnimations` then calls `FUN_00433930`, which walks the `.3DI` cells column by column, each column from the top, and each cell's four slots in order. Every slot whose descriptor is a loaded `Anim` one gets a decor object (`FUN_00433aa0`, category 4, its kind the descriptor's terrain type) at the cell's top left corner, added to the cell's object list. `ObjectInitialize` (`00419580`) starts its own copy of the animation from the first frame, so the descriptors' random start frames only spend draws, stamps its sounds onto every channel at priority `0x3f`, and draws `RandomBelow(10000)` from the shared generator into `+0x114`. `EngineVisitViewObjects` (`00433b30`) steps a decor object in its update pass only once the clock (session `+0x1024`, set with the game clock) has passed that value, so each map animation starts within ten seconds, and draws its frame in its draw pass in sprite layer 2. Its depth is its row, plus one row for walls and obstacles and two rows for roofs (ground and water get none). Neither pass touches decor objects with low details (session `+0x27e0` bit `0x40`).

Before the game loop `EngineRunLevel` draws the level and the panel once and calls `FUN_00445580`. When `<level>-<language>.txt` exists next to the level (`-%s.txt`: `underground\t1u1`, `t2u1`, `t3u1` and `cpu\t3p2` have one), `FUN_0043de90` reads it and `FUN_004282a0` shows it over the frozen level:

- **Backdrop**: the panel's `black_square` (slot 22) tiled over the screen twice, from 32, 16, each row and column stopping a tile short of the edge. The panel is drawn over it.
- **Text**: v-bonus font 3 from `64 * width / 640`, `80 * height / 480`, lines `25 * height / 480` apart. It types one character per frame. On a tab the cursor first moves to the next multiple of 64, then the tab prints with the font's own tab rule. Each printed character also asks for `bip.wav`, on the channel mask `1 << ((char)(1 << 16) - 2)`, which is bit 30 and names no channel, so it stays silent.
- **Ending**: fire while it types ends the page at once; once typed, fire or any key does.

Then, for every level, the level's name (session `+0x1034`) rises from the bottom centre (`width / 2 - 100`, `height`) at one pixel per update for 5 seconds, typed, in v-bonus font 5 and colour `0xffe010`.

## Player death

The player dies like any object once its energy is spent: `EngineUpdateObjects` calls `ObjectDestroy` with its keep flag, so the player's `TYPE Explosion` blocks go off (the Vyruz's blasts reach 256 pixels) and the object stays, and sets its timer at object `+0xcc` to 10 seconds. While the timer runs the player's updates are skipped; when it runs out the game update returns 0. `EngineRunLevel` then ends the game with `EngineGameOver` (`00445a40`), which shows `tmp\game-over.gaf` centred for up to 5 seconds (any key or mouse button ends it, `ShowImageWait` at `00404950`), saves the score table `vz.sco` (`SaveScores`, `0043e470`) and clears the level name, so the menu follows. There are no lives.

While the timer runs, `EngineRunLevel` (`0043e5f0`) plays out the death:

- **No target**: `SeekerFindTarget` (`0042da80`) takes only objects whose `+0xcc` timer is below 1, and a seeker drops a target that moved, so the enemies stop hunting.
- **The skull**: `FUN_004457f0` puts `Data\Extra\failed.gaf` at the screen's centre as two animation messages that last until the game ends: animation 2, a dark outline in draw mode 8, under animation 1, the skull.
- **The motor** stops (`SoundStop(8)`).
- **Red**: with Gamma FX on and a gamma ramp available, the gamma offsets (session `+0x25a0`) become 80, 0, 0. While the player is dead they are applied as they are and do not decay. See [Gamma flashes](#gamma-flashes).
- **The drift**: after each update the dead player's position, which the view follows, moves at 0.5 pixels per update, then at 3 per 14 ms step (`3 / (14 / step)`), in a growing square: right, up, left, down. It turns, without moving that update, whenever the position leaves the level or the truncated X or Y is a multiple of the turn interval, which starts at 32 and grows by 30 with each turn.

## Gamma flashes

With Gamma FX (option flag `0x80`) and a gamma ramp, `EngineRunLevel` hands the gamma offsets (session `+0x25a0`, red, green, blue) to the ramp on every frame that has one set, while no lift is taking the player and the finale has not started (`GammaSetRamp`, `0045ffc0`, clamped to ±255). A positive value lifts a channel toward white by `(255 - c) * value >> 8` and a negative one scales it by `(value + 255) >> 8` (`FUN_00460160`). For the living player the ramp is the user's own gamma (session `+0x27d4`) plus the offsets, which then step one toward zero, and once all three reach zero the ramp is the user's gamma again. During Darken (session `+0x20d0` bit `0x20`) the frame leaves both the ramp and the offsets alone. For the dead player the ramp is the offsets alone, without the user's gamma, and they stay. Flashes push all three offsets:

| Event | Push | Clamp |
|---|---|---|
| A blast of 300 or more on the living player (`AreaDamage`) | 60, or 30 below 1000 | ±100 |
| The trap surprise | 100 | ±150 |
| The destroy-around surprise | 200 | ±150 |

The ramp itself holds absolute values: `ConfigLoad` and every menu (`RunMenu`, `FUN_00420990`) set it to the user's gamma (`gamma = red, green, blue` in `vzconf.txt`), so it applies to every screen, and without Gamma FX a level leaves it there. Darken, with Gamma FX, keeps the ramp it finds and sets the user's gamma as it starts (`SurpriseStart`, `0041c390`), and puts the kept ramp back as it ends (`SurpriseEnd`). The port's renderer draws the ramp over every finished frame (`FRenderer::SetGamma`): a screen blend for positive values and an inverse-source darkening for negative ones. Its swapchain is UNORM, so the blend works on display values as the ramp did.

`FUN_0041e5a0` fades in five steps. With Gamma FX the ramp of all three channels is `(step - 4) * 255 / 4` on its own, without the user's gamma, 50 ms apart (`FUN_00469780`); without, it blacks out the rows at or past the step in every four, 25 ms apart. Fading out runs the steps from 4 down to 0, fading in from 0 up to 4, and a fade with its sound flag plays `fade.wav` (`LoadFadeSound`, `0041e560`) at priority `0xbf` as it starts. `MenuGame` uses it however the game menu closes: it fades the menu's background bitmap (`DAT_00523628`) out, with the sound unless the game is ending; only a resumed game then fades the level back in, with the game standing still, and the ramp goes back to the user's gamma (`FUN_00420990`). `EnginePause` uses it only when `pause.gaf` cannot be loaded, and `EngineGameOver` only sets the user's gamma back. The game's end plays `fade.wav` before the ending pages.

The finale after `Final-PC` falls has its own flashes and fade ([Combat.md](Combat.md#the-finale)).

## Object passes

`EngineVisitViewObjects` (`00433b30`) visits the objects of the grid cells around the view: from one column left of it and two rows above it, 13 columns and 20 rows, clamped to the grid. Objects elsewhere are neither updated nor drawn. It runs twice:

- After every sub-step, as the update pass: each object's bob plays (`ObjectSelectBobMode`, `00433f40`, and `ObjectPlayBob`, `0043c090`), then `ObjectDraw` (`00431ae0`) updates its animations without drawing.
- Once per frame, as the draw pass. `ObjectDraw` advances animations again by the last sub-step before drawing, except for classes with `Graphic-update-needed`, which only the game update advances.

Placed objects other than actors, doors and turrets play their looping animation the same way: the FX, thing, ammo and decor libraries set `Graphic-update-needed`, so their objects (`Water-Wave` among them) step every update wherever they are; items step in both passes while in view.

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

The player (kind `0x18`) also draws three layers from its shadow GAF (`V-VZ.GAF`, set up by `AnimInitializeGraphics` at `00417b30`), each at frame = pose and at the body's place and depth, and none while it fades in or out:

- The reactor glow, animation 3 under the body and animation 4 over it, while the reactor timer (object `+0x258`) runs and the speed is positive, in additive mode 7 with a grey diffuse of `trunc(speed * 255 / Thrust.Max)`. `EngineUpdateObjects` sets the timer to 1000 ms on every step with forward thrust and runs it down by the step otherwise. The Video Details option Reactor FX (session `+0x27e0` bit 4) turns it off.
- The front light, animation 5, over the body in its own mode, unless the motor is blocked (object flag `0x2000000`) or Ground Smoke details are off (`+0x27e0` bit `0x40`).

These frames carry event flag 4: `DrawAnimationFrame` sets the diffuse colour to the event value, scaled by the colour the draw passes (`ColorCombine`, `0044f310`), so the layers fade by pose. The light shows only for the poses that face the viewer.
