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

## Verification against DEMO.3DR

`--verify-game` replays `CAVE\DEMO.3DR`. The recording came from a development build: its records are taken with the 10 ms demo step cap, its timestamps are clock readings at each step start, and its `Friction-threshold` was 0.001 (the line commented out in the shipped `OBJECT.3DC`). Rebuilding each step's state from the recording, 26454 of 32282 steps match within 0.001 px and 30156 within 0.5 px. The remainder come from object collisions, which are not modelled yet, and from step lengths that the 1 ms timestamps can only place to within a millisecond.
