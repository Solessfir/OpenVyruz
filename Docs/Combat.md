# Combat

Traced from `Vyruz.exe` 1.31.01 and ported in `Source/Game/Engine.cpp`. Names are the ones applied in the Ghidra project.

## Random generator

`RandomNext` (`00454080`) keeps a 32-bit state and a counter. It returns `counter * state` (low 32 bits), increments the counter and rotates the state left by the product's low five bits. `RandomRange` (`004540c0`) maps the product to `low + product * (high - low + 1) / 2^32` and `RandomBelow` (`00454130`) to `product * n / 2^32`, both truncated. `VzLoadLevel` reseeds it with state `0xFEABCEFE` and counter `0xABCDEFDA` (`RandomSeed`, `00454030`), so every level starts from the same sequence.

## Weapons and firing

A weapon class carries a `TYPE Weapon` block (`weapon.3dc`); the player's `ADD Weapon` blocks list the weapons it carries and `ADD Ammo` the stocks. `LoadObjectClassLibrary` stores `initial-slope` and `slope-step` in radians (`degrees * pi * (1/180)`, float constants).

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

FX classes (`fx.3dc`) play one animation from their GAF. `UpdateAnimation` (`0045e7e0`) holds each frame for its ANM duration times the display frame period (`g_FramePeriodMs`), so effect timing follows the refresh rate. A one-shot animation (`loop = 0`) ends after its last frame and the FX object is removed; a looping one restarts from the animation's loop-start frame.

`AmmoImpact` spawns the ammo's `hit-fx` at the contact point when a shot strikes an object or a wall, and `Bullet-Ground-Impact` when a sloped shot sinks to the ground.

A destroyed object with a `TYPE Explosion` block hands it to a new `Boom-fx` object at its position (`ObjectDestroy`, `00437d40`). Before half of the effect's animation has played, the blast waits up to 500 ms; then `AreaDamage` goes off once around the effect with the block's `damage` and `damage-zone`. Blasts destroy neighbouring objects, so explosions chain.

## Not ported yet

- The shield reset value (a class field at `+0x6c`).
- Bouncing ammo (`on-hit = 0`: grenades, saws, pulsars), which the port bursts on first contact.
- Shots hitting their owner.
- Automatic weapon switching when the stock is empty.
- Water splashes, two-piece wrecks, surprise drops and sounds.
- Blasts hurting the player and pickups.
- Drawing visible ammo.

`--verify-game` fires every level's player weapon for six seconds of game time while spinning and reports the shots and destroyed objects.
