# Surprises

Traced from `Vyruz.exe` 1.31.01 in Ghidra (`VzEd\surprise.cpp`).

A surprise is an item whose `TYPE Item` has a `Surprise-type` (item attribute `+0x80`): `random`, or a `bonus-` or `malus-` keyword. `FUN_0041ba80` maps the keywords; any other word gives no surprise. `Surprise` items mostly come from crates and wrecks (`RANDOM Surprise`), and some levels place `BONUS-` items.

## Collecting

The pickup branch of `EngineUpdateObjects` leaves a surprise lying while the player already carries one. Otherwise `ConsumeItem` hands it to `FUN_0041cf40`:

1. Every carried surprise of the type in effect (object `+0xd4`) ends and is dropped.
2. The item joins the player's carried objects (object `+0x11c`, newest first).
3. A random type is drawn by `FUN_0041ba00`: `RandomRange(0, 135)` against the cumulative weights of `DAT_0050d5d0`, which `FUN_0041b830` sums when the level loads. The weights for types 0 to 20 are 0, 0, 10, 10, 5, 10, 5, 10, 0, 10, 1, 5, 5, 10, 10, 5, 5, 5, 10, 10, 10.
4. The delay before it starts is 500 ms (`FUN_0041bfb0`) and the time it lasts comes from `FUN_0041bfe0`.
5. Its text (`FUN_0041bd20`) appears at the view's centre minus 132 in red `0xff2010`, and its icon (`FUN_0041d180`, an animation of `surprise.gaf`) at width - 80, height - 54, pulsing its alpha by 4 every 10 ms. Both last the duration plus the delay (3 seconds for a one-millisecond surprise), at least 5 seconds.

| Type | Keyword | Lasts | Effect |
|---|---|---|---|
| 2 | malus-reverse-commands | 10 s | Left and right, and the two strafes, swap (object flag `0x40000000`). |
| 3 | malus-trap | 1 ms | An Explosion FX (there is no "Shock Wave" class); energy and shield times 0.75; `RandomRange(-16, 16)` twice for the screen shake. |
| 4 | malus-weapon-jam | 10 s | The weapon in hand does not fire (session `+0x220c`). |
| 5 | malus-darken | 20 s | A dark zone around the player. |
| 6 | malus-extasy | 10 s | Acceleration and deceleration doubled, screen effects. |
| 7 | malus-low-speed | 20 s | Acceleration and deceleration times 0.25. |
| 8 | malus-pinball | 20 s | Elasticity 1.5. |
| 9 | malus-banana | 15 s | Friction 0.999 both ways and grip 1; the end restores the friction only. |
| 10 | malus-blocked-motor | 5 s | No command and no firing (object flag `0x2000000`). |
| 11 | bonus-destroy-around | 1 ms | `FUN_0043d980`: every destructible object within 6 cells loses its energy; `RandomRange(-32, 32)` twice for the screen shake. |
| 12 | bonus-touch-and-destroy | 8 s | Object flag `0x400000` and attached FX. |
| 13 | bonus-auto-shield-up | 15 s | The shield-up step (object `+0xe4`) plus 10. |
| 14 | bonus-invisible | 16 s | The invisible drone, below. |
| 15 | bonus-circular-fire | 10 s | Session flag 8. |
| 16 | bonus-time-warp | 12 s | Session `+0x21f4` = 3, trails. |
| 17 | bonus-reload-weapons | 15 s | Every ammo stock full; the end gives back the counts it found. |
| 18 | bonus-invulnerability | 10 s | Object flag `0x800000`: `ApplyDamage` takes nothing. |
| 19 | bonus-triple-fire | 20 s | Session flag 4. |
| 20 | bonus-nop | 1 ms | Nothing. |

## Each step

`FUN_0041d290` runs at the start of the player's update. For each carried surprise: while the delay lasts it counts down; then the duration counts down and the first such step starts it (`surprise::FUN_0041c390`), which does nothing while another surprise is in effect; when the duration is over the surprise ends (`surprise::FUN_0041ca90`, only for the type in effect), and its icon and the item go. With no surprise counting its duration, the type in effect returns to none.

## The invisible drone

Invisibility sets object flag 8 and leaves a passable copy of the player where it was (session `+0x2604`, its position at `+0x26ac`):

- `SeekerFindTarget` skips flagged objects, so no seeker or turret finds the player, and `DoorFindHolder` does not count it for gates and lifts.
- `ApplyDamage` spares the invisible player.
- A lift does not take it, and touching a closed lift or gate says "Cannot take lift while invisible !" or "Cannot open gates while invisible !" 250 pixels left of it.
- Only ammo with `Usable-when-invisible` fires; other weapons say "NOTE : ONLY MINES CAN BE USED WHILE INVISIBLE" at 100, 110 in the panel font with option flag 8.
- `ObjectDraw` fades the player out by 0.02 of full alpha a frame (object `+0x170`, which counts from 49 down) to a quarter, and back in afterwards.
- `FUN_00445380` counts session `+0x26a0` down from 16 seconds; in the last second the drone flies straight back to its body without speed, and the end puts it there.

## Screen effects

- **Darken** loads `Data\Object\FX\darkzone.gaf` and sets session `+0x20d0` bit `0x20`. `EngineRunLevel` then draws the level only inside a 376x309 window around the player (8 pixels left and 16 up of centring it, inset by 2), leaves the rest black, and stretches the GAF's frame, a 128x103 quarter in draw mode 8 (inverse source colour, white toward the corner), over the window's four quarters, mirrored outward, which leaves an oval of light. The player and the `Is-light` objects are drawn again over the dark. With Gamma FX it also sets the gamma ramp to session `+0x27d4`, which the port leaves out.
- **Extasy** sets object flag `0x1000000`, which `ReadPlayerInput` (`00438540`) reads as full forward thrust whatever the keys say.
- **Screen shake**: a trap draws `RandomRange(-16, 16)` and Destroy-around `RandomRange(-32, 32)` twice into session `+0x2200` and `+0x2204`. Each frame `EngineRunLevel` moves each one step toward zero, flips its sign, and adds it to the view offset after the clamp to the level.
- **Gamma flashes**: see [MainLoop.md](MainLoop.md#gamma-flashes).

## Port

`Source/Game/Engine` keeps the carried surprises in the player's inventory. Not ported yet: Darken's gamma ramp, the attached FX of Touch-and-destroy, Invulnerability and Banana, Time-warp's trails, what Touch-and-destroy, Triple-fire and Circular-fire do, the shield-up step, and the `Reactivate-after` of surprise generators.
