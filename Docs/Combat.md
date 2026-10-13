# Combat

Traced from `Vyruz.exe` 1.31.01 and ported in `Source/Game/Engine.cpp`. Names are the ones applied in the Ghidra project.

## Random generator

`RandomNext` (`00454080`) keeps a 32-bit state and a counter. It returns `counter * state` (low 32 bits), increments the counter and rotates the state left by the product's low five bits. `RandomRange` (`004540c0`) maps the product to `low + product * (high - low + 1) / 2^32` and `RandomBelow` (`00454130`) to `product * n / 2^32`, both truncated. `VzLoadLevel` reseeds it with state `0xFEABCEFE` and counter `0xABCDEFDA` (`RandomSeed`, `00454030`), so every level starts from the same sequence, after the level start draws described in [MainLoop.md](MainLoop.md#level-start).

## Weapons and firing

A weapon class carries a `TYPE Weapon` block (`weapon.3dc`); the player's `ADD Weapon` blocks list the weapons it carries and `ADD Ammo` the stocks. `LoadObjectClassLibrary` stores `initial-slope` and `slope-step` in radians (`degrees * pi * (1/180)`, float constants).

### Weapon slots

Session `+0x1028` holds the 1-based slot in hand, which starts at 1. `ObjectSetWeaponSlot` (`004375f0`) gives slot N the player's Nth `ADD Weapon` (object `+0x24c`) and clears that entry's last-shot time; a slot past them leaves the player unarmed. Each `ADD Weapon` keeps its own trigger flags and slope. `ObjectBindAmmo` (`00437540`) then binds the `ADD Ammo` stock of the weapon's ammo class (object `+0x248`), but only a stock that is not empty.

- `EngineReadActionKeys` (`0043b5c0`) reads `action-next-weapon` and `action-weapon-1` to `-9`, whose default keys (`InputSetDefaultKeys` at `0041d420`, from `DAT_00509298`) are Backspace and the digits.
- `PlayerNextWeaponSlot` (`004376a0`) tries the slots after the one in hand, wrapping after 9, until one has a stock of at least 1, and stays when none has.
- `PlayerSelectWeaponAction` (`0043bad0`) ignores the slot already in hand.
- `PlayerSelectWeapon` (`00437740`) takes the slot and plays its sound. With option flag 8 it lists `WEAPON <name>`, `FIRE RANGE`, `FIRE DELAY` at 400, 140 to 160, and when ammo is bound `AMMO <name>`, `DAMAGE`, `DAMAGE AREA` at 400, 180 to 200, upper case and typed in the panel's font 3 for 4 seconds. The damage and area are the ammo's `TYPE Ammo` values, or, when either is zero, the largest over its Ammo and Explosion blocks.
- `WeaponHasAmmo` (`00444450`) moves an empty weapon to the next slot when it fires.

While fire is held, `ReadPlayerInput` calls `WeaponRaiseSlope` (`0042c210`): the first press sets the slope to `initial-slope`, then each step adds `slope-step` until it passes zero and stays at zero. Releasing calls `WeaponResetSlope` (`0042c280`).

`ObjectUpdateWeapon` (`00444ca0`) runs at the end of the shooter's update. It fires when no shot has been taken yet or when `Delay` milliseconds of game clock have passed since the last one. `WeaponFire` (`00443bd0`) then creates one ammo object per fitted side:

- With `Weapons-gap`, the shots come from both sides (`Left-side`, `Right-side`), offset by half the gap across the facing. `Alternated-fire` flips the side on every shot.
- The muzzle sits `Start-distance` out along the facing in world space (table `0050d440`). Without one it is `RandomNext() % trunc(min(width, height) * 0.4 * 10)` tenths out, from the shooter's sprite bounds.
- The shot takes the shooter's facing, its ammo class `Thrust.Initial` as speed, and `Fire-altitude` (or the shooter's altitude) as altitude. Its velocity is `speed * facing` plus the shooter's velocity.
- A weapon with a negative `initial-slope` gives the shot the current slope: each step the shot's altitude changes by `sin(slope) * altitude`, and it bursts on the ground once the altitude drops below 1. Holding the trigger therefore lengthens the range.
- Each shot uses one round; the volley stops when the stock runs out.

## Shots in flight

Ammo objects with `Behaviour = Straight` thrust forward through `ObjectComputeDisplacement` like any object. A `damage-after` timer detonates the shot when it runs out. Objects other than the player are removed once they leave the level.

On contact `AmmoHit` (`0043d7d0`) moves the shot to the contact point and calls `AreaDamage` (`0043cb20`) around its last committed collision centre. `AmmoImpact` (`0043d350`) then pushes the struck object by `40 / Weight` along the shot's facing and damages it by `damage`, scaled by a matching `TYPE Absorption` coefficient (`AbsorptionScale`, `0043d780`).

`AreaDamage` scans the grid cells within the zone and damages every live object whose collision centre lies within `Damage-zone` units: the full damage at distance zero, otherwise `(zone - trunc(sqrt(trunc(d^2)))) / zone * damage`.

`ApplyDamage` (`0043ca30`) takes damage from the shield first; when the shield breaks, the rest goes to energy. An object whose energy reaches zero is destroyed in its next update. Cops, generators, walkers, burners, jeeps, rollers, helicopters, turrets, spiders and tanks pay their `Reward` to the player.

## Effects and explosions

FX classes (`fx.3dc`) play one animation from their GAF. `UpdateAnimation` (`0045e7e0`) holds each frame for its ANM duration times 14 ms: `VzRun` calls `AnimationSetFixedPeriod` (`0045e4c0`) at startup, so the display frame period (`g_FramePeriodMs`) is never used for animation. `DrawAnimationFrame` (`0045eab0`) also applies each ANM frame's event transform: flags `0x40`, `0x80` or `0x100` scale X, Y or both by the event value in percent, times the FX `stretch-ratio` (`Small-Explosion` frames are drawn at 50%). Event flag `4` (a colour tint), `1` (a sound) and `0x20` (rotation) are not ported. A one-shot animation (`loop = 0`) ends after its last frame and the FX object is removed; a looping one restarts from the animation's loop-start frame.

`AmmoImpact` spawns the ammo's `hit-fx` at the contact point when a shot strikes an object or a wall, and `Bullet-Ground-Impact` when a sloped shot sinks to the ground.

A destroyed object with a `TYPE Explosion` block hands it to a new `Boom-fx` object at its position (`ObjectDestroy`, `00437d40`). Before half of the effect's animation has played, the blast waits up to 500 ms; then `AreaDamage` goes off once around the effect with the block's `damage` and `damage-zone`. Blasts destroy neighbouring objects, so explosions chain.

## Drops

`ObjectDestroy` (`00437d40`) calls `ObjectDropContents` (`00437f80`), which creates the destroyed object's contents with `3do::CreateObject` (`00413800`):

- The class's `HAVE <class>` lines (class `+0xa4`), always.
- Its `RANDOM <class> <chance> <out-of>` lines (class `+0xa8`, the numbers at object `+0x2b0` and `+0x2b4`), each when `RandomRange(1, out-of)` is at most the chance. A demo recording or replay skips these.
- The objects it carries (object `+0x11c`).

The class loader prepends each entry, so a list runs from the last declared line back to the first, and a derived class's lines come before its parent's. `Override-Objects` and `Override-Random-Objects` empty the lists.

The first object lands on the wreck's truncated position. Each later one is placed from the previous offset `dx` and a row `dy`, both starting at 0, with the ring `r` starting at 32 and `left = -r` at the start of each list:

- `dx = 0`: the object goes to `left`.
- `dx < 0`: it goes to `-dx`, mirrored right.
- `dx > 0`: `r` and `-left` grow by 10 and the row changes: 0 becomes -10, -10 becomes 10, and 10 becomes 0 with the object at `left`; otherwise it goes to `dx = 0`.

The new objects are not activated, so they draw no bob phase.

## Generators

A StaticGen (kind 0x17, the `Generator-*` classes of tower 3) carries a `TYPE Generator` block, attribute kind 2: `Product` (a class, keyword 0x66) at `+0x10`, `Max-births` (negative for no limit) at `+0x14`, the births so far at `+0x18`, `Birth-delay` and `Birth-duration` in milliseconds at `+0x1c` and `+0x20`, a timer at `+0x24`, and flags at `+0x28`: 1 `Stressable`, 2 `Active` (a birth under way), 4 `Stable-population`.

`ObjectUpdateGenerator` (`004372a0`) runs at the end of the generator's update every step; generators skip the view check other objects have. While births remain and either time is set:

1. The timer counts the step down, twice as fast for a stressable generator whose energy is below its class maximum.
2. When the step reaches it, a generator that is neither under way nor open (object `+0x5c` bit 1) waits.
3. Otherwise `ObjectOverlapsSolid` (`00405df0`) tests the generator closed: anything solid on it holds the birth back.
4. An open generator closes: the timer becomes the birth duration and flag 2 is set.
5. A closed one creates its product on itself (`FUN_00433ff0`), links it back at product `+0x2b0` and `+0x2b4`, gives it weapon slot 1, counts the birth, opens and restarts the timer with the birth delay. A product that hunts the player adds to the enemy count.

`Active = 1` makes the first birth come at the first update. With `Stable-population`, `ObjectDropContents` takes a dead product off its generator's births, so it makes another.

## Wrecks

A class with `Two-pieces` (class `+0x20` bit 2: tanks and turrets) dies twice. On the first death `EngineUpdateObjects` calls `ObjectDestroy` with its keep flag: the explosions go off and the reward is paid, but nothing drops and the enemy count stays. The object becomes a wreck (object `+0x128` bit `0x20`): its energy and shield go back to the class's initial values, and its behaviour becomes plain Fixed (8), so it neither moves nor looks for the player. `ObjectDraw` leaves out a wreck's upper half, a tank's top or a turret's gun, and keeps the turret's base (the animation after its 24 gun poses) and the shadow. The second death destroys it for good. Gold boxes leave `Gold` (500 money), and many classes drop a `Surprise` power-up by chance.

## Turrets

Turrets (`Behaviour = Fixed Seeker`, kind Turret) draw a rotating top from the 24 animations at `GAF-index` and a fixed base from animation 25. `SeekerUpdate` (`0042c5a0`) runs their `TYPE Seeker` block every step:

- Unlocked, the scan timer counts down; when it runs out it is refilled by `Scan-Delay` and `SeekerFindTarget` (`0042da80`) looks for the target within `Sensor-Range` collision units and inside the vision cone around the base's facing. `PathStartSearch` (`0040f6f0`) then routes to it.
- Locked, the timer counts down while the turret engages. When it runs out and the target has moved since the lock, the lock drops and the next step scans again.
- The pending route search advances every 17 ms. Fixed seekers abort when the route needs another expansion; a turret accepts only a direct two-point route in one of the four collision-axis direction sectors. See [PathSearch.md](PathSearch.md).
- Engaging (`SeekerEngage`, `0042caa0`) needs that direct route within `Min-distance`. The top turns one pose per `Turret-turn-timeout` toward the line to the target (`SeekerTurnTop`, `0042d150`), using the previous step's direction.
- `SeekerAimAndFire` (`0042d2d0`) raises the weapon slope while the target is within the weapon's `Fire-Range`, and pulls the trigger when the weapon is ready and the top faces the target. The weapon update then fires as for the player.

The port runs the original incremental route search from collision centre to collision centre against the terrain graph. Turret shots pass through the turret that fired them and damage the player's shield and energy; blasts reach the player too.

## Not ported yet

- The shield reset value (a class field at `+0x6c`).
- Bouncing ammo (`on-hit = 0`: grenades, saws, pulsars), which the port bursts on first contact.
- The other seeker kinds (cops, walkers, jeeps and the rest), which follow their routes.
- The idle look-around of fixed objects (`FixedIdleLookAround`, `00444a40`), with its saved random state.
- Drawing turret bases, the player's shove and absorption, the HUD and the player's death.
- Automatic weapon switching when the stock is empty.
- Water splashes, what a surprise does when collected, the generator's opening animation, the `CREATE` trails and smoke (they draw from their own random stream), and sounds.
- Blasts hurting pickups.
- Drawing visible ammo.

`--verify-game` fires every level's player weapon for six seconds of game time while spinning, then puts the player in clear view of a turret for three seconds; it fails if no turret fires in those duels.
