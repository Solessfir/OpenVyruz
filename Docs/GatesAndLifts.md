# Gates, Lifts and Keys

Traced from `Vyruz.exe` 1.31.01 in Ghidra. Names are the ones applied in the Ghidra project.

## Kinds and attributes

`VzCreateTypeTable` (`00447430`) registers object kinds 0x10 to 0x20: Ammo, FX, Item, Lift, Gate, Thing, Cop, StaticGen, Vyruz, Walker, Burner, Jeep, Roller, Heli, Turret, Spider and Tank. An object's kind comes from its class `Sub-type` or the folder of its GAF.

`AttributeKindFromProperty` (`00409ae0`) turns a `TYPE` block into an attribute kind: `Lift` is kind 3, `Gate` kind 4 and `Key` kind 5. `LoadObjectClassLibrary` packs the gate and lift fields into one flag word (attribute `+0x10`):

| Bit | Gate | Lift |
|---|---|---|
| `0x1` | `Active` | `Active` |
| `0x2` | `Opened` | `Present` |
| `0x4` | `Closing` | `Going-Up` |
| `0x8` | `Stay-Opened` | `Stay-Up` |
| `0x10` | `Stay-Closed` | `Stay-Down` |
| `0x20` | `Sensor` | |
| `0x40` | `Key-always-needed` | `Key-always-needed` |
| `0x80` | `Destroy-key` | `Destroy-key` |
| `0x100` | `Needs-code` | `Needs-code` |

The words after it hold `Delay`, the running timer and `Id`. A lift also keeps `3do-file` (the level it leads to, or a starred marker such as `*End*`), `Scenery` and `Usage-Count`. A key holds `Id`, `Is-code`, `Code-part-count` and `Code-part-id`.

## Activation and animation

The object's animation `GAF-index` leads to open or present, and `GAF-index + 1` leads to closed or away. Each is played once and then held on its last frame, because the loop start is the last frame. `AnimInitializeGraphics` (`00417b30`) shows the last frame of the animation for the starting state and fills the timer with `Delay`. Its collision node is disabled while the gate is open or the lift present (object `+0x5c` bit 1, `ObjectSyncCollisionNode` at `0040ef20`). The decor classes have `Graphic-update-needed`, so `EngineUpdateObjects` advances the animation every step before the update runs, wherever the view is.

A new animation starts at its third frame. Each update first returns while the timer is full and the animation has not reached its last frame.

## Gates

`GateUpdate` (`00443620`):

- **Closed, with the opening animation started:** the gate becomes passable and open.
- **Closed otherwise:**
  - A sensing gate waits, flagged `Stay-Closed`, until something may pass within two cells.
  - A gate without a sensor does not wait.
  - The opening animation then starts.
- **Open:** after `Delay` it stays open while something may pass nearby. Otherwise it closes, unless a solid object overlaps it or it has `Stay-Opened`.

`DoorFindHolder` (`00443430`) scans the 5×5 cells around the gate for any object except ammo, FX, items, lifts, gates and things, so cops and turrets also open a gate. With a nonzero `Id`, only an object carrying that key counts. The cheat flag `DAT_00523808` waives the key, and invisible objects are left out.

`ObjectOverlapsSolid` (`00405df0`) compares box against box over the 3×3 cells around the object. It skips passable objects and lying items. Every body is a circle (there is only `Bounding-box-size`), so a closed gate one cell away, or a turret beside a gate, keeps a neighbouring gate open.

## Lifts

`ObjectUpdateLevelExit` (`00442bf0`) runs the same cycle:

- **Away, with the arrival animation started:** the lift becomes present. It is passable only once `Id` is zero or the key holder is near.
- **Away otherwise:** after `Delay` (unless `Stay-Down`) the arrival animation starts.
- **Present, after `Delay` and while `Usage-Count` is positive:**
  - With nothing solid on it and no `Stay-Up`, it leaves: impassable, departure animation.
  - Otherwise it ends the level if the player's box lies inside the lift's box and the player has access. A negative `Reward` is the fare, taken from the player's money.
  - The lift then counts a use and names the next level.

Touching a lift or gate without the key shows "Lift needs key", "Wait, lift will come" or "Gate needs key" (`EngineUpdateObjects`), and using one shows "Using Key" (`ShowUsingKeyMessage`, `00443500`).

## Level changes

`ObjectUpdateLevelExit` stores the lift's `3do-file` as the next level and sets the level-end flag. Without `Scenery` the level stays in the current zone. With it, the zone changes, which takes the fuller reload (session flags `0x400000` and `0x800000`). A starred name such as `*End of Tower 2*` ends a tower.

`LoopLevels` (`004467d0`) runs `VzPlayLevel`, then keeps the player object if it is still alive. `LevelReload` (`00420430`) loads the next level from scratch through `VzLoadLevel`, so a level left and entered again starts over. Before the next `VzPlayLevel`, `LoopLevels` replaces the new level's player object with the kept one:

- It moves to the new player's position.
- Its velocity is cleared.
- Everything else is kept: energy, shield, money, weapons, ammo, carried keys and codes, facing and thrust.

The player arrives on the next level's lift. That lift is present, so it carries the player back once its delay runs out.

## Keys and codes

`Key-N` and `Code` items have `Carry`. `ConsumeItem` (`00444660`) moves a carried item without an item action onto the player's list at object `+0x11c`, newest first. `ObjectCarriesKey` (`0043dcb0`) checks the newest item with the id. A code counts only when the parts carried with that id match the first part's `Code-part-count` and include part 1 (`ObjectFindCompleteCode`, `00439270`). `Destroy-key` makes `ObjectDestroyKeys` (`0043dd20`) remove every carried key or part with the id. Without `Key-always-needed` the door then forgets its id.

## Port

`CreateDoors` and `StepDoors` in `Source/Game/Engine` follow these updates after the actors' step. `--verify-game` checks them:

- With the player far away, every level's sensing gates are run until they close.
- Each closed gate is then opened by the player.
- The player rides each usable lift with its key, waiting on it until it leaves.

`RunLevel` in `Source/Main.cpp` plays one level and returns the next level's path, carrying the player object, state, inventory, selected weapon and game clock. `--verify-game` also loads the level each lift leads to.

Not ported yet:

- The end-of-tower sequence for starred names.
- Putting the carried player first in its grid cell when the activation phases are drawn.
- The messages, which wait for the HUD.
- Invisibility, the key cheat and code timeouts.
