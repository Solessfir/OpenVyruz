# Path Search

Traced from `Vyruz.exe` 1.31.01. The route search at `00410460` is incremental obstacle-corner branching, rather than a conventional A* priority queue.

## Search state

`PathStartSearch` (`0040f6f0`) creates a two-point route from the actor's collision centre to the target's collision centre. Each advance visits only the routes that existed at the start of that call. Branches appended during an advance wait for the next call.

The original return values are retained:

| Value | Meaning |
|---|---|
| `0` | No surviving route |
| `1` | Routes were already complete |
| `-2` | Work remains |
| `-1` | At least one route reached the target during this advance |

The collision graph orders obstacle bounds independently along each axis. Equal keys put the last insertion first. Initial insertion visits grid rows and columns in reverse order; each cell's linked list puts newly inserted objects first. The shooter and target are disabled in the graph while querying their route.

The point and segment queries ignore kind-3 actors. Terrain lines and rectangles remain obstacles; water is ignored only when the caller sets the water-exclusion flag. The port therefore feeds terrain bounds in their existing insertion order to the search.

An obstructed segment branches through selected corners of the obstacle's bounding box, expanded by the actor's half-extents and another 8 collision units. Generated points are shared across branches and exact coordinate duplicates are suppressed. Completed routes are simplified by skipping to the furthest visible later point (`00411120`). Selection uses the original score at `0042d6c0`: the sum of squared distances from the first point to each later point, keeping the first route on equal scores.

## Turret caller

`SeekerUpdate` (`0042c5a0`) advances a pending search at 17 ms intervals. A fixed seeker aborts on `-2`; moving seekers retain their routes and continue searching. This distinction prevents a turret from locking a target that requires a detour.

After route selection, `SeekerValidateRoute` (`0042ca30`) requires a turret's route to contain exactly two points and its collision-space direction to be one of `0`, `6`, `12`, or `18`. Engaging and firing still use the seeker's range and direction checks.

## Patrols

`ParseBehaviour` (`00409490`) packs `Behaviour` words into object `+0x190`: the target bits are Seeker `1`, Tracker `2` and Player `3`; the movement bits are Random `4`, Fixed `8`, Patrol `0xc` and Straight `0x10`; Guardian adds `0x20`. A movement word must come first, and Seeker or Tracker must follow it.

`PatrolMoveUpdate` (`0042e400`) keeps a work attribute beside the `TYPE Patrol` attribute. Its timer refills by the patrol's `Scan-delay` both when a search starts and when it ends, and the route is re-planned whenever the timer runs out, even toward the same point. `FUN_0042e740` picks the goal. The first time, `FUN_0042e910` takes the `.3DP` path with the patrol's `Path-ID` (the last one on duplicates) and its nearest point by Manhattan distance to the truncated world position, preferring later points on ties. Afterwards the goal moves on only once the previous route is used up and the last range check reported arrival. An open path turns back at either end; a closed one wraps. A patroller without a matching path never starts.

A found route is followed segment by segment. `SeekerOnSegment` (`0042d820`) drops the segment once the actor leaves its bounds widened by twice its radius plus its speed. `SeekerEngage` (`0042caa0`) checks the distance to the segment end against 8 units for inner corners, or a 64-unit radius at the route end (arrival within 48), then turns the body one pose toward the segment per turn timeout (object `+0xec`) and sets command 200 unless the route has run out. `SeekerTurnBody` (`0042cfa0`) turns a body that cannot turn on the spot (class flag `0x20`, jeeps and tanks) only while its speed (object `+0x148`) is 1 or more (`DAT_0050950c`); with a class `Turn-timeout` (class `+0x7c`), its timeout becomes `(1 - speed / Speed-max) * Turn-timeout`, truncated, and the time already waited is cut to it, so it turns quicker the faster it goes. `FUN_0042cc60` adds one hundredth of the world-space offset back to the segment line, which `ObjectComputeDisplacement` adds in collision space after strafe and before friction.

## Moving seekers

`SeekerUpdate` (`0042c5a0`) runs before the movement behaviour and uses the `TYPE Seeker` block for Seeker actors and the `TYPE Tracker` block for Trackers (`SeekerFindAttribute`, `0042c510`). Every `Scan-Delay` it looks for the player within `Sensor-Range` and the vision cone around the body's facing (`SeekerFindTarget`, `0042da80`), whose half-width in directions the class library reads as `(Vision-Angle % 361) / 30` from the integer angle (keyword `0x8a`); the data uses 360 and 60 and routes to it. A seeker drops a search that needs a detour; a tracker keeps searching. While it follows a route to the target, object `+0x13c` holds bit 1 without bit 2 and the patrol pauses; `FUN_0042d7d0` cancels the patrol's route. At each `Scan-Delay` on the route, a target that has moved, by position on the last segment and by world cell before it, ends the route so the next scan re-plans.

On the last segment the range check (`SeekerDirectRange`, `0042ce40`) measures the distance to the target against `Min-distance`: within it the actor steers its segment end onto the target and aims, and within half of it backs off with command `0xd0`. Actors that cannot turn on the spot also need the target within one direction of their facing; that check goes through `FUN_00407630`, which rotates the collision-space delta as if it were in world space before taking its direction. `SeekerAimAndFire` (`0042d2d0`) pulls the trigger inside the weapon's `Fire-Range` once the body faces the target exactly, is not turning (`FUN_00436520`) and the weapon's `Delay` has passed; a firing seeker drops forward thrust. Seekers and trackers do not get the segment pull, since `ObjectComputeDisplacement` applies it only in patrol and random mode (object `+0x194`).

## Actor kinds

Cops and burners have 48 body poses, two per direction; jeeps, rollers, tanks and the other vehicles have 24 (`PlayerAdvancePose`, `00438390`, and the pose factor in `SeekerTurnBody`). `ObjectDraw` draws a burner like a cop, as a frame of its `rot` animation (25): the lean animations `rot01` and `rot00` that `FUN_004333f0` would pick while it turns are only allocated for the player. A roller draws like a jeep, one animation per pose. A `Two-pieces` class (tanks) has a turret top: while the target is in firing range the body holds still and `SeekerTurnTop` (`0042d150`) turns the top one pose per `Turret-turn-timeout` toward the target, and when the body turns the top turns with it. Aiming and firing then use the top's direction. TANK.GAF holds the 24 body poses as animations 1 to 24 and the top as animation 25, one frame per top pose, drawn after the body at the same depth. Tanks, walkers and spiders scale the segment pull by 0.01 (`DAT_005095b8`) and keep no velocity after a free move; walkers and spiders also push without dividing thrust by `Weight`. Patrol arrival radii are 4 units for spiders and 5 for walkers. A spider turns through 24 poses but stops only at the four axis directions, drives only when stopped there (`FUN_0042ccf0`), accepts a seeker route only as one straight axis leg (`FUN_0042ca30`), and gets no thrust from its seeker, so it moves only while patrolling. `ObjectDraw` lifts a walker's body by 32 pixels. Helis have 24 poses and fly over water (`Over-water`), so water edges do not stop them. HELI.GAF holds four tilt animations with one frame per pose; `HeliSelectBodyPose` (`004334e0`) moves a tilt counter one step per draw toward the band for the current speed (above 2, 1 and 0.5 units per step it climbs while at or below 24, 20 and 10, holding in the fastest band; slower it settles at 5) and draws animation 0 below 5, 1 below 10, 2 below 20 and 3 from 20. Things move too when their behaviour says so: the `Particle-of-Death` things of `cpu\t2p1` (`Behaviour = Patrol`, 48 poses) follow their patrol paths like cops.

`FixedIdleLookAround` (`00444a40`) runs for a seeker or tracker that can turn on the spot (`Can-turn-on-place`) and stands still with no turn or thrust command, such as a fixed burner waiting for the player. While it is at its target pose it draws `RandomRange(1, 150)` from the Misc stream, and a 75 picks a new target pose with `RandomRange(0, 23) + 1`, so only poses 1 to 24 even for a 48-pose body. It then turns one pose toward it per `Turn-timeout` (`PlayerTurn`). A two-piece body looks around with its top instead (`FUN_00444b80`); no two-piece class can turn on the spot, so the port leaves that out.

An aimed seeker in firing range that hovers or stands also counts as idle, so in the original it now and then turns one pose off its target and back; a Visible-when-firing heli such as the T3 dragon scouts blinks out each time. The port keeps such a seeker on target: it still makes the draws, so the Misc stream is unchanged, but drops the new target pose while the last engage found the target within firing distance.

Walking, turning poses and bobbing follow the object passes in [MainLoop.md](MainLoop.md#object-passes).

## Random movers and guardians

`RandomMoveUpdate` (`0042dd20`) follows the same plan, search and segment cycle as a patrol, with the `TYPE Random` block's `Delay` as its interval and `Range` as the route search limit. Its work attribute has no arrival radius, so it drives each route to the end and re-plans once it leaves the last segment. `FUN_0042e040` picks the destination: if the last route never reached its last segment, it retries that destination once; otherwise it draws up to 10 cells with `RandomRange` from the shared generator, X then Y, inside a window of `(Range + 32) / 64` columns and `(Range + 16) / 32` rows around the actor's cell (the whole map when `Range` is zero), and takes the centre of the first cell whose surrounding 5 x 5 block of terrain markers (session `+0x2038`, the `BuildTerrainMarkers` grid) all read `0xff`, ground closed on all four sides. `CellsHaveMarker` (`0042c130`) also rejects cells holding blocking objects. In narrow corridors no draw succeeds, so about half the shipped Random cops wait in place until their seeker finds the player. Every class library's `DEFAULT-PROPERTY` gives `Range = 300` and `Delay = 2000`.

A Guardian seeker (behaviour bit `0x20`) uses its `TYPE Guardian` block: `SeekerFindTarget` searches `Range` around the actor in every direction, and with no target in sight `FUN_0042d8f0` routes back to `Location`, which defaults to the placement position. That route has no target, so the next scan drops it and plans again.

A cop that starts exactly on its nearest patrol point gets a zero-length first route; `SeekerUpdateSegment` rejects its direction and the cop stays put until something else moves it. The port's route graph holds only terrain, so a route through placed objects such as pallets can leave a patroller pushing against them, where the original detours.
