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

Moving enemies and route following are separate from the route-search implementation.
