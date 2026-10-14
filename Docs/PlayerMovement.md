# Player Movement

Traced from `Vyruz.exe` 1.31.01 and ported in `Source/Game/Engine.cpp`. Names are the ones applied in the Ghidra project.

## Order within a game step

`EngineUpdateObjects` (`004406e0`) runs once per step (see [MainLoop.md](MainLoop.md)). For the player it:

1. Reads input into three commands (`ReadPlayerInput`, `00438540`; `ReplayPlayerInput`, `00438800` for demos): turn (`0xCB` left, `0xCD` right), thrust (`200` forward, `0xD0` backward) and strafe (`-2` or `+2`). Live input combines turning and strafing; demo input drops the turn while strafing.
2. Turns (`PlayerTurn`, `00443990`). A timer counts down by the step length and, whenever it drops below 1, is refilled by `Turn-timeout` until it is non-negative again. Each refill advances the body pose one step toward its target (`PlayerAdvancePose`, `00438390`, 48 poses the short way round); a new direction is only taken once the pose has caught up. Turning left moves to the next node of the 24-direction ring (counter-clockwise on screen), turning right to the previous one.
3. If there is no thrust, no strafe and no velocity, the player idles: the speed decays toward zero by `Decceleration-step`.
4. Otherwise it computes the displacement (`ObjectComputeDisplacement`, `00438b20`) and moves.

## Displacement

All of this happens in collision space (see [DataFormats.md](DataFormats.md#original-static-terrain-collision)) with x87 double intermediates rounded on float stores. Velocity is kept in units per 14 ms; `timeScale = 14 / step`.

- With `Use-inertia`, the displacement starts from `velocity * Grip`.
- Thrust ramps the speed: forward adds `Acceleration-step` up to `Thrust.Max`, backward subtracts `3 * Decceleration-step` down to `Thrust.Min`, no thrust decays toward zero.
- While the previous speed magnitude is below `Speed-max / 10`, thrust pushes by `speed * direction / Weight`.
- Strafing adds `strafe * direction` for the facing six steps back (90 degrees).
- Each axis is multiplied by `Friction-factor`; if neither axis exceeds `Friction-threshold / timeScale` the displacement is zeroed.
- The result is divided by `timeScale`.

Direction `d` uses table entry `(d + 3) % 24` at `0050d508`: `0.1 * (sin, cos)` of `(entry - 3) * 15` degrees.

## Collision and bounce

The new collision centre is converted back to the world, clamped to `[1, extent - 2]` for the player, and the circle (`Bounding-Box-Size`, default 20) is swept through `MovementCollision` (`004105d0`). Without a hit the velocity becomes `timeScale * displacement`. On a hit the world position is restored and `ObjectBounce` (`004367d0`) decides:

- If the direction to the contact point and the motion direction are seven or more of the 24 directions apart, the move is accepted: the collision centre advances and the world position catches up on the next step.
- Otherwise the velocity is reflected about the centre-to-contact direction and scaled by `Elasticity`. A blocked player that is thrusting uses `speed * facing` as its motion.

## Mouse

`ConfigLoad` (`0042f570`) sets the input device (session `+0x165c`) from `vzconf.txt`: 1 for `use-keyboard`, 2 for `use-joystick`, 3 for `use-mouse`, which also turns the absolute command (option flag 1) on. Only the mouse loads `Data\Menu\cursor.gaf` (`EngineApplyDisplayAndCursor`, `0042f2d0`, session `+0x25b8`); every other device hides the cursor. EngineRunLevel plays the sight by the last step and draws it at the mouse after the rest of the frame.

The port plays with every device at once instead. It reads the keyboard's table, whose actions take keys and mouse buttons alike (see [Menus.md](Menus.md)). Moving the mouse hands the steering to the sight, which then turns the player as the original's mouse device does; pressing a turn key (Left or Right, not as part of a strafe), or with the absolute direction any movement key, hands it back to the keys, absolute or relative as set. A demo steers by its records. The sight always loads, shows while the mouse steers, and fades out over half a second once the mouse has been still for three seconds; returning from the game menu does not count as moving the mouse.

The mouse's default bindings (`DAT_00509368`) fire with the left button (0x103), thrust with the right one (0x104), back off with Down, and strafe with Left and Right; turning has no binding. `InputReadDirection` (`0041d7e0`) instead takes the vector from the player's screen position (its truncated world position plus the scroll) to the mouse, turns it into a direction through collision axes (`DirectionFromScreen`, `00407630`), and lets `DirectionTurnCommand` (`00436600`) walk the player's direction ring: the target within eleven next nodes turns left, exactly twelve away reads as backward and does not turn, and otherwise it turns right. While the player moves, a target within one direction of its facing gives no turn. `InputActionPressed` drops the turn on a side that strafes.

## Absolute keyboard

With the keyboard and the absolute command (Control's `Direction : ABSOLUTE`), `InputReadDirection` makes a screen vector of the movement keys (Left and Right for x, Forward and Backward for y), turns it into a direction the same way, and `DirectionTurnCommand` (with its backward flag) gives the command: the player's own direction (or none) thrusts forward, twelve nodes away backs off, and otherwise the player only turns toward it, so a new direction first stops it. The port (`AbsoluteKeyInput`) deliberately thrusts while it turns: forward while the target lies within a quarter turn of the facing, backward beyond, and neither at exactly a quarter turn, so the ship keeps moving the target's way; the target straight behind still backs off without turning. Without the absolute command the actions drive as they are, Left and Right turning.

## Verification against DEMO.3DR

`--verify-game` replays `CAVE\DEMO.3DR`. The recording came from a development build: its records are taken with the 10 ms demo step cap, its timestamps are clock readings at each step start, and its `Friction-threshold` was 0.001 (the line commented out in the shipped `OBJECT.3DC`). Rebuilding each step's state from the recording, 26454 of 32282 steps match within 0.001 px and 30156 within 0.5 px. The remainder come from object collisions, which are not modelled yet, and from step lengths that the 1 ms timestamps can only place to within a millisecond.

## Object collisions

`MovementCollision` (`004105d0`) sweeps one endpoint list that holds both terrain shapes and object bounds. Objects collide as circles with radius `Bounding-Box-Size`; a size of zero leaves them out. Circle against circle (`004069d0`) subtracts 2 from the centre distance before comparing it with the summed radii.

An object hit is resolved by `ObjectPush` (`00436dd0`). Moving away from the contact accepts the move, as for terrain. A target with zero `Friction-factor` is immovable and reflects the mover's velocity component along the contact normal, scaled by `Elasticity`. Otherwise both exchange that component weighted by `Weight / 10`, and the target is flagged as pushed: its velocity then carries it with friction until its speed drops below 1.

The port treats static scenery with a bounding box as solid. Pickups, gates, lifts, keys, teleports and actors are left out until their behaviours exist, and the demo replay stays terrain-only. Running the level's objects alongside it (doors, keys, the player's recorded fire, cops, turrets and shots) made 20 more steps exact and 843 fewer, because the recording keeps only the player, so the port's world drifts from the original's and objects stand where the recorded path went. Of the 5811 steps that miss, about 3860 match exactly with other step lengths, which the 1 ms timestamps cannot pin down; of the rest, about 1010 are off by less than 0.2 pixels on runs of 6, 7 and 10 ms steps, which also points at the split, and about 200 are jumps of around 2 pixels from rest across the facing, the shove of an enemy shot (`40 / Weight`), which the replay does not fire.

## Object kinds and pickups

`CreateObject` (`00413800`) gives each object a kind: the class `Sub-type` when set, otherwise the name of the folder holding its `GAF-name`, looked up in the type table (`OBJECT\ITEM\items.GAF` is an Item, `OBJECT\VYRUZ\VZ.GAF` the player). Kinds `0x10`-`0x20` are Ammo, FX, Item, Lift, Gate, Thing, Cop, StaticGen, Vyruz, Walker, Burner, Jeep, Roller, Heli, Turret, Spider and Tank. Crates and pickups are both Items.

When the player touches an object with an item attribute, `EngineUpdateObjects` lets it pass and applies the item: `Add-NRJ` while energy is below 95% of its maximum, `Add-Money` always, `Add-Shield` while shield is below its maximum, and ammo through `FUN_00444540`. If anything applied, `FUN_00444660` consumes the item.

The player picks up an Item whose `TYPE Item` sets `Only-for-player` (keyword `0xdb`, item attribute `+0x10` bit 1); grilles, stains, smoke, saving plots and the empty standby carry item blocks without it. `PlayerAddAmmo` (`00444540`) adds an item's ammo only when one of the player's weapons has a `TYPE Weapon` block for it: the first stock of that ammo that is not full fills by the item's amount up to its maximum, and an unbound weapon in hand binds it. Under Reload Weapons it looks in the saved copy at session `+0x2208`, which holds the stocks but no weapon, so nothing is added. `ConsumeItem` (`00444660`) leaves a hidden copy of an item with `Reactivate-after` that shows again later; only `Surprise-Generator` has one and no level places it, so the port leaves it out. `--verify-game` reports how many pickups the recorded demo path collects.
