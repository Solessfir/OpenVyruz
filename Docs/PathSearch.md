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

A found route is followed segment by segment. `SeekerOnSegment` (`0042d820`) drops the segment once the actor leaves its bounds widened by twice its radius plus its speed. `SeekerEngage` (`0042caa0`) checks the distance to the segment end against 8 units for inner corners, or a 64-unit radius at the route end (arrival within 48), then turns the body one pose toward the segment per `Turn-timeout` and sets command 200 unless the route has run out. `FUN_0042cc60` adds one hundredth of the world-space offset back to the segment line, which `ObjectComputeDisplacement` adds in collision space after strafe and before friction.

## Moving seekers

`SeekerUpdate` (`0042c5a0`) runs before the movement behaviour and uses the `TYPE Seeker` block for Seeker actors and the `TYPE Tracker` block for Trackers (`SeekerFindAttribute`, `0042c510`). Every `Scan-Delay` it looks for the player within `Sensor-Range` and the vision cone around the body's facing (`SeekerFindTarget`, `0042da80`) and routes to it. A seeker drops a search that needs a detour; a tracker keeps searching. While it follows a route to the target, object `+0x13c` holds bit 1 without bit 2 and the patrol pauses; `FUN_0042d7d0` cancels the patrol's route. At each `Scan-Delay` on the route, a target that has moved, by position on the last segment and by world cell before it, ends the route so the next scan re-plans.

On the last segment the range check (`SeekerDirectRange`, `0042ce40`) measures the distance to the target against `Min-distance`: within it the actor steers its segment end onto the target and aims, and within half of it backs off with command `0xd0`. Actors that cannot turn on the spot also need the target within one direction of their facing; that check goes through `FUN_00407630`, which rotates the collision-space delta as if it were in world space before taking its direction. `SeekerAimAndFire` (`0042d2d0`) pulls the trigger inside the weapon's `Fire-Range` once the body faces the target exactly, is not turning (`FUN_00436520`) and the weapon's `Delay` has passed; a firing seeker drops forward thrust. Seekers and trackers do not get the segment pull, since `ObjectComputeDisplacement` applies it only in patrol and random mode (object `+0x194`).

A cop that starts exactly on its nearest patrol point gets a zero-length first route; `SeekerUpdateSegment` rejects its direction and the cop stays put until something else moves it. The port's route graph holds only terrain, so a route through placed objects such as pallets can leave a patroller pushing against them, where the original detours.
