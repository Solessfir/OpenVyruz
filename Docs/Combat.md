# Combat

Traced from `Vyruz.exe` 1.31.01 and ported in `Source/Game/Engine.cpp`. Names are the ones applied in the Ghidra project.

## Random generator

`RandomNext` (`00454080`) keeps a 32-bit state and a counter. It returns `counter * state` (low 32 bits), increments the counter and rotates the state left by the product's low five bits. `RandomRange` (`004540c0`) maps the product to `low + product * (high - low + 1) / 2^32` and `RandomBelow` (`00454130`) to `product * n / 2^32`, both truncated. `VzLoadLevel` reseeds it with state `0xFEABCEFE` and counter `0xABCDEFDA` (`RandomSeed`, `00454030`), so every level starts from the same sequence, after the level start draws described in [MainLoop.md](MainLoop.md#level-start).

## Weapons and firing

A weapon class carries a `TYPE Weapon` block (`weapon.3dc`); the player's `ADD Weapon` blocks list the weapons it carries and `ADD Ammo` the stocks. `LoadObjectClassLibrary` stores `initial-slope` and `slope-step` in radians (`degrees * pi * (1/180)`, float constants).

### Weapon slots

Session `+0x1028` holds the 1-based slot in hand, which starts at 1. `ObjectSetWeaponSlot` (`004375f0`) gives slot N the player's Nth `ADD Weapon` (object `+0x24c`) and clears that entry's last-shot time; a slot past them leaves the player unarmed. Each `ADD Weapon` keeps its own trigger flags and slope. `ObjectBindAmmo` (`00437540`) then binds the `ADD Ammo` stock of the weapon's ammo class (object `+0x248`), but only a stock that is not empty.

- `AmmoHit` counts the player's shots that burst on a hunter or an item as hits and the others as misses, for the scores ([Menus.md](Menus.md#scores)).
- `EngineReadActionKeys` (`0043b5c0`) reads `action-next-weapon` and `action-weapon-1` to `-9`, whose default keys (`InputSetDefaultKeys` at `0041d420`, from `DAT_00509298`) are Backspace and the digits.
- `PlayerNextWeaponSlot` (`004376a0`) tries the slots after the one in hand, wrapping after 9, until one has a stock of at least 1, and stays when none has.
- `PlayerSelectWeaponAction` (`0043bad0`) ignores the slot already in hand.
- `PlayerSelectWeapon` (`00437740`) takes the slot and plays its sound. With option flag 8 it lists `WEAPON <name>`, `FIRE RANGE`, `FIRE DELAY` at 400, 140 to 160, and when ammo is bound `AMMO <name>`, `DAMAGE`, `DAMAGE AREA` at 400, 180 to 200, upper case and typed in the panel's font 3 for 4 seconds. The damage and area are the ammo's `TYPE Ammo` values, or, when either is zero, the largest over its Ammo and Explosion blocks.
- `WeaponHasAmmo` (`00444450`) moves an empty weapon to the next slot with ammo when it fires, and again right after the shot that empties it, naming the new slot (`PlayerSelectWeapon`). It uses the player's slot whichever object fires, but only the player ever runs dry.

While fire is held, `ReadPlayerInput` calls `WeaponRaiseSlope` (`0042c210`): the first press sets the slope to `initial-slope`, then each step adds `slope-step` until it passes zero and stays at zero. Releasing calls `WeaponResetSlope` (`0042c280`).

`ObjectUpdateWeapon` (`00444ca0`) runs at the end of the shooter's update. It fires when no shot has been taken yet or when `Delay` milliseconds of game clock have passed since the last one. `WeaponFire` (`00443bd0`) then creates one ammo object per fitted side:

- With `Weapons-gap`, the shots come from both sides (`Left-side`, `Right-side`), offset by half the gap across the facing. `Alternated-fire` flips the side on every shot.
- The muzzle sits `Start-distance` out along the facing in world space (table `0050d440`). Without one it is `RandomNext() % trunc(min(width, height) * 0.4 * 10)` tenths out, from the shooter's sprite bounds.
- The shot takes the shooter's facing, its ammo class `Thrust.Initial` as speed, and `Fire-altitude` (or the shooter's altitude) as altitude. Its velocity is `speed * facing` plus the shooter's velocity.
- A weapon with a negative `initial-slope` gives the shot the current slope: each step the shot's altitude changes by `sin(slope) * altitude`, and it bursts on the ground once the altitude drops below 1. Holding the trigger therefore lengthens the range.
- Each shot uses one round; the volley stops when the stock runs out.

## Shots in flight

Ammo objects with `Behaviour = Straight` thrust forward through `ObjectComputeDisplacement` like any object. A `damage-after` timer detonates the shot when it runs out. Objects other than the player are removed once they leave the level. However it goes, a detonating shot is destroyed with `ObjectDestroy`, so its class's `TYPE Explosion` blocks go off like any object's: that is where grenades (2000 within 128), missiles (4500 within 96), stingers (3000 within 64) and saws do their damage, their `TYPE Ammo` damage being zero. Only a contact (`AmmoHit`) counts a player's shot as a hit or a miss; a shot that runs out or sinks to the ground counts as neither.

On contact `AmmoHit` (`0043d7d0`) moves the shot to the contact point and calls `AreaDamage` (`0043cb20`) around its last committed collision centre. `AmmoImpact` (`0043d350`) then pushes the struck object by `40 / Weight` along the shot's facing and damages it by `damage`, scaled by a matching `TYPE Absorption` coefficient (`AbsorptionScale`, `0043d780`). The player is shoved and absorbs the same way, with its class's `Weight` and absorption blocks.

The `TYPE Ammo` attribute's flags decide what a contact does: `On-hit` (flag 1) bursts the shot on anything, `On-object-hit` (flag 2) only on objects. A shot with `On-object-hit` but not `On-hit` (grenades, saws, pulsars) that meets the terrain is put back and bounces (`ObjectBounce`, `004367d0`), then turns to its new heading. A hard knock, more than 2 units of travel that step, takes a tenth of its speed times its weight from its own energy (unless it is `Indestructible`), and a shot emptied that way bursts where it is. `--verify-game` throws grenades all round from each level's start and requires some to bounce and fly on.

A weapon class may hold several `TYPE Weapon` blocks, one per ammo it takes (the pulsar gun takes `Pulsar-Ammo`, `Pulsar-II-Ammo` and `Bouncing-Pulsar-Ammo`). `ObjectSetWeaponSlot` (`004375f0`) keeps the first block for the weapon's range, delay and sounds, and `ObjectBindAmmo` (`00437540`) binds the first of the shooter's stocks, in their order, that is not empty and holds an ammo any block takes; the shots are that ammo. It binds again when a weapon is selected, when its stock runs dry (`WeaponHasAmmo`, `00444450`), when ammo is picked up and when Reload Weapons fills every stock. The port binds the same way, and an empty weapon keeps the first stock a block takes so it still knows what it lacks.

`ObjectDraw` draws a shot as its ammo class: the shadow on the ground and the body at its altitude, in the pose of its direction (ammo classes have 24), at the frame its animation has reached. A class with `GAF-index` 0 or `Invisible` (the bullets, the pulsar's body) shows no body. `ObjectUpdateCreates` (`00437b00`) has each of the class's `CREATE` blocks make an object at the shot's position (plus `Offset`) and altitude once the shot has lived `Delay` milliseconds, then every `Repeat-timeout`, one per update. These are `Fixed` things with a `Life-Time`: `Pulsar-II-Trail` every 10 ms makes the pulsar's comet, `Little-Smoke-Trail` (an `Is-detail`) every 40 ms a rocket's smoke. The port keeps them as trails that play their class's first animation and go when their life is over. FX run their `CREATE` blocks the same way, by their own age.

## Debris

A `CREATE` block with `Random-dir` (keyword `0xec`, flag 2) or `Set-speed` (`0xed`, flag 4) launches its product (`FUN_00413740`): `Random-dir` faces it `RandomRange(0, 23)` drawn from the Misc stream, which `FUN_00428e20` and `FUN_00428eb0` swap in for the draw (session `+0x259c`), so the shared generator is left alone; its velocity is its thrust along that direction, the class's initial thrust or, with `Set-speed`, the block's `Speed` (`0xea`), which its Speed-max takes too. `Distance` (`0xeb`) is read but nothing uses it. Only the explosion FX `Big-Boom` (24 blocks), `Vyruz-Explosion`, `Vyruz-Explosion-2` and `Final-Explosion` have such blocks, throwing the particles, sparks and fire of `thing.3dc`: `Launched` (keyword `0x2a`, class flag `0x1000`, object flag `0x8000`, which drops movement commands), `Use-inertia`, a `Life-Time` of 1 or 2 seconds, and their own smoke `CREATE`. Their `Bounding-Box-Size` is zero (`thing.3dc`'s default), so object `+0xb4` bit 1 skips every collision: they coast through walls, objects and the player alike. The port flies them as trails with their class's movement (thing.3dc's default `Weight` of zero only matters for a push, which they never make), drops them once they leave the level, and `--verify-game` checks that a Big-Boom throws them in several directions with smoke.

## Mines

Ammo classes with `Behaviour = Fixed` are mines: `Mine-Infinite`, placed some 300 times over 17 levels and kept in sessions, and `Mine-Ammo`, which the mine weapon drops. A mine is an object like any other: it collides, takes damage, and blasts set it off, so minefields chain.

- **Contact**: `EngineUpdateObjects` calls `AmmoHit` when a mover that is not ammo runs into an ammo object. The mine moves to the contact point, `AmmoImpact` pushes the mover by `40 / Weight` along the mine's facing (and damages it by the mine's `TYPE Ammo` damage, zero for both classes), and `ObjectDestroy` sets off its `TYPE Explosion` (2000 within 124, `Mine-Shock-Wave`). The mover's update ends there. A player's mine counts as a hit when a hunter sets it off and a miss otherwise.
- **A moving mine**: ammo with neither `On-hit` nor `On-object-hit` that meets the terrain goes off (`AmmoHit` with its destroy flag). Against an object `AmmoHit` moves it and pushes the object without destroying it, then the object's shield plus energy comes off the mine's shield and, once the shield holds or is gone, off its energy too; a mine with energy left carries on through the object, one without dies in its next update. A mine only moves with a velocity it carries (`Use-inertia` or object flag `0x100`), which no shot or blast gives it, so this hardly happens in play.
- **Dropping**: `WeaponFire` (`00443bd0`) creates the mine facing the shooter's way. With the `TYPE Ammo` `active-after` (attribute `+0x1c`, 5000 for `Mine-Ammo`) it calls `ObjectHide` (`004391d0`) with a clear flag: the mine shows but its collision node is off, and its off timer (object `+0xcc`) runs that long.
- **Timers**: while the off timer runs (or is negative), the object's whole update is skipped, its `damage-after` with it. When it runs out `ObjectShow` (`00439140`) shows the object and turns its collision on, and with a `Random-on-timeout` `Max` (class `+0xb4`) the on timer (`+0xc8`) is drawn between `Min` and `Max`. When the on timer drops below zero `ObjectHide` hides it, keeping it solid only with `Keep-bb-when-invisible` (class `+0x20` bit 1, set for all ammo), and the off timer is drawn from `Random-off-timeout`; that step's update ends. Each draw reseeds the generator with the object's own seed (object `+0x2a8` and `+0x2ac`), which `CreateObject` sets to the level seed and nothing changes, then restores the shared one: every draw gives the same value, so a dropped mine arms after 5 s, then shows for about 2.4 s and hides for about 7.5 s over and over, and goes off by itself once its 30 s `damage-after` has counted down in the shown spells, about two minutes after the drop.
- `ObjectInitializeClassProperties` starts both timers at their blocks' `Initial`, zero for both mine classes, so placed mines never blink.

The port keeps a mine as an object body with its timers; a dropped mine joins the level at the end of the step it was fired in. Not ported: a mine's own `TYPE Ammo` damage and blast and `Ignore-owner`, unused by both classes. Pushed objects run into the player like into any object, bounce off it and knock it; a pushed mine that meets the player pushes it by 40 / its `Weight` along the mine's facing and goes off, the player's shield and energy outlasting any mine's. `--verify-game` drives the player into one of `UNDERGROUND\T2U1`'s mines, shoves another until it meets something, shoves it again at the player, and follows a dropped one until it goes off.

## Hit glow

`AreaDamage` sets object flag `0x200` on every object it damages, unless Invulnerability's flag `0x800000` is set. `ObjectDraw` reads it for the player alone: it notes the game clock at session `+0x2588` and clears the flag, then adds to the body a red specular of `(500 - ms since the hit) >> 2`, 125 fading to nothing over half a second. While the player's `Shield-up-step` (object `+0xe4`) is not zero, as during Auto Shield Up, it also adds a green specular of `|trunc(sin(clock * 2pi * 0.001) * 50)|`, a pulse once a second. The light layers (reactor glow, front light) reset their colour and take neither.

## Invisible objects

A class with `Invisible` (keyword `0x26`, class `+0x1c` bit `0x200`) starts with object flag 8 (`ObjectInitialize`, `00419580`). `ObjectDraw` leaves such an object out, unless its class has `Visible-when-firing` (keyword `0x4f`, bit `0x400`): then it fades with its counter (object `+0x170`) one step per frame toward hidden while flag 8 is set and back while it is clear. While it fades or stays hidden, only its body is drawn, by alpha: its shadow, lights and overlays are left out. Every invisible class in the campaign (`Walker-Invisible`, the T3 roller scouts, the invisible turrets) has `Visible-when-firing`. `SeekerEngage` (`0042caa0`) clears flag 8 while `SeekerAimAndFire` has the object aimed at the player in range and sets it again otherwise, so these enemies show only while they shoot. `WeaponFire` refuses an invisible shooter's ammo unless it is `Usable-when-invisible`. A Two-pieces wreck becomes visible.

## Shields and difficulty

`EngineUpdateObjects` adds each cop's, the player's, walker's, burner's, jeep's, roller's, heli's, turret's, spider's and tank's `Shield-up-step` (object `+0xe4`) to its shield on every update, up to its class's `Shield.Max`. The player's class has a step of 0, so only Auto Shield Up (10 more while it lasts) restores its shield.

The exe keeps four difficulty levels of ten values each (`DAT_0050d328`): player shield and energy maxima, and the ratios for the enemies' scan delay, scan range, shield-up step and fire delay, the player's shield-up step, the random mine count and the shield and energy item values. `LoadObjectClassLibrary` reads `COMMON\DIFFICULTY.3DC` over them (keywords `0x102` to `0x10c`: each `difficulty-level` line picks the level the following values go to, and the file ends on Normal), and `ConfigLoad` takes `difficulty-level` from `vzconf.txt` (`SetDifficulty`, `004032c0`) and copies that level to session `+0x26b8`. As each level starts, `EngineActivateObjects` hands it to `FUN_0043c4a0` for the player and every hunter:

- The player: a positive shield maximum replaces the class's, the energy maximum does unless it is exactly 1, and the shield-up step is multiplied by its ratio.
- Each seeker or tracker: its `Scan-Delay` and `Sensor-Range` are multiplied and truncated.
- Each hunter: its shield-up step is multiplied, and its first weapon gets its own copy of the class with `Delay` multiplied and truncated.

A ratio of exactly 1 leaves its value alone. Nothing reads the mine count or item value ratios.

## Fast kills

When a blast from the player's shot finishes off a hunter, `AreaDamage` opens a 500 ms window (session `+0x2748` into `+0x274c`) with a count of one at `+0x2750`, or adds one while the window is open. `EngineRunLevel` counts the window down each step; once it is shorter than the step and the player lives, a count of two or more pays `((count - 2) * 5 + 10) * 1000` into the player's money and earnings, posts "Well done <name> !!!" and "Fast Kill Bonus +<amount> $" at 100, `height / 2 - 102` in v-bonus font 5, red `0xff2010`, for 8 seconds, fading every 10 ms, and queues one of Well Done, Yeepee, Come On, Ariba, Laugh, The Best, Want More and Hey by the game clock's low three bits, half a second later.

## The finale

`Final-object` (keyword `0x1d`, class `+0x20` bit `0x20`) is set only on `Final-PC` in `cpu\t3p2`, which also has `Player-direct-fire-only`: `AmmoImpact` hurts it only with the player's own shots (`FUN_0043c990`), and `AreaDamage` only with blasts whose source belongs to the player. `ApplyDamage` returns 1 when it takes a Final-object's energy to zero, and each caller (`AmmoImpact`, `AreaDamage`, the knocks, `ConsumeItem`) then calls `FUN_0043db70`:

- `DestroyAround` (`0043d980`) from the map's centre over every cell takes every destructible object's energy and draws its shake, which is then set back to zero.
- The level-leaving timer `DAT_00523820` starts at 4500 ms, channel 4 stops, and the queued lines give way to Yeepee.

While the timer runs, `EngineRunLevel` reads no action keys, and `FUN_00445dd0` counts it down by each step. With Gamma FX, until 2.5 seconds are left, it plays `electric.wav` at priority `0xbf` and turns the gamma ramp white every 250 ms (`DAT_00523818`), showing the user's gamma in between; for the last 2.5 seconds `FUN_00446060` sets each channel of the ramp, on its own, to `trunc(510 * ln(e - t * (e - 1) * 0.0004)) - 255` for `t` milliseconds into the fade, white to black. When it runs out, `ShowNetworkFailure` (`00446120`) sets the user's gamma again (`GammaSet`, `0045ffc0`) and types "... GENERAL NETWORK FAILURE ...", a blank line, "... DISCONNECTED ...", a blank line and "... SYSTEM HALTED ..." at 100, from 100 and 20 apart, one a second, in the panel's font and green (`FUN_004315b0`), for up to 10 seconds or until a key, and the game ends as after the starred lift (session `+0x24` set to `*`). The port starts the finale in the destruction pass after the death rather than at the damage itself.

## Knocks

`EngineUpdateObjects` (`004406e0`) hurts movers that knock into things. When a mover that travelled more than 2 units this step meets an object, both take half the mover's speed times its weight; against the terrain the mover alone takes a tenth of it. The player never hurts itself this way. A `Player-direct-fire-only` class (keyword `0x36`, flag `0x40` at class `+0x20`) is hurt by knocks only when the player is the other party. `Ignore-same-class-objects` lets a class pass through objects of its own class. This is what makes a `Particle-of-Death` deadly: it has 1 energy, so its first knock destroys it, and its explosion does 5000 damage over 50 pixels. In the port actors also run into the player, which takes their knocks; the original pushes the player like any object, the port only bounces the actor off it.

`AreaDamage` scans the grid cells within the zone and damages every live object whose collision centre lies within `Damage-zone` units: the full damage at distance zero, otherwise `(zone - trunc(sqrt(trunc(d^2)))) / zone * damage`.

It takes every object with energy, whatever its bounding box, but never a pickup: `IsPlayerPickup` (`0043c9f0`) spares an item whose `TYPE Item` sets `Only-for-player`, the flag `EngineUpdateObjects` picks it up by. Of the box-less objects only Processors Chambers' 78 weld seams (`soudure`) are not `Indestructible`: 250 shield, 1 energy, and a `TYPE Explosion` of 500 damage over 50 units with `Small-Explosion`, so a blast along a seam sets the next ones off in a chain, as Destroy-around and the finale do. The port gives such objects a body that nothing collides with (object `+0xb4` bit 1), and `--verify-game` sets one seam off and checks that others follow.

It leaves out an invulnerable player. For the player, the damage that reaches it, not the blast's, decides the rest: 200 or more shakes the screen (`StartScreenShake`, `0043d100`, into session `+0x2200`) by `RandomRange(-n, n)` twice from the Misc stream, `n = trunc(damage) / 100 + 8` up to 32, unless the finale has started; 300 or more flashes the screen, 60 or 30 below 1000, and makes a living player cry out.

`ApplyDamage` (`0043ca30`) takes damage from the shield first; when the shield breaks, the rest goes to energy and the shield takes back the class's `Shield [Min]` (class `+0x6c`; the block's `Initial`, `Min` and `Max` fill `+0x68` to `+0x70`). Every class's minimum is zero but four of `underground\item.3dc`'s, whose shield equals its maximum, so it refills whole each time it breaks. An object whose energy reaches zero is destroyed in its next update. Cops, generators, walkers, burners, jeeps, rollers, helicopters, turrets, spiders and tanks pay their `Reward` to the player.

## Effects and explosions

FX classes (`fx.3dc`) play one animation from their GAF. `UpdateAnimation` (`0045e7e0`) holds each frame for its ANM duration times 14 ms: `VzRun` calls `AnimationSetFixedPeriod` (`0045e4c0`) at startup, so the display frame period (`g_FramePeriodMs`) is never used for animation. `DrawAnimationFrame` (`0045eab0`) also applies each ANM frame's event transform: flags `0x40`, `0x80` or `0x100` scale X, Y or both by the event value in percent, times the FX `stretch-ratio` (`Small-Explosion` frames are drawn at 50%). Event flag `4` sets the diffuse colour to the event value, scaled by the colour the draw passes. Flags `8` (specular), `0x10` (alpha), `1` (a sound) and `0x20` (rotation) are not ported. A one-shot animation (`loop = 0`) ends after its last frame and the FX object is removed; a looping one restarts from the animation's loop-start frame.

`AmmoImpact` spawns the ammo's `hit-fx` at the contact point when a shot strikes an object or a wall, and `Bullet-Ground-Impact` when a sloped shot sinks to the ground.

A destroyed object with a `TYPE Explosion` block hands it to a new `Boom-fx` object at its position (`ObjectDestroy`, `00437d40`). Before half of the effect's animation has played, the blast waits up to 500 ms; then `AreaDamage` goes off once around the effect with the block's `damage` and `damage-zone`. Blasts destroy neighbouring objects, so explosions chain.

## Orbiting FX

An FX class's `TYPE FX` (attribute kind `0xf`) can circle a host: `count`, `circular`, `angular-speed`, `ray`, `phase`, `vertical-amplitude`, `vertical-pulsation`, `vertical-phase`, `check-event`, and `on-timeout` and `off-timeout` (attribute `+0x34`, `+0x38`). `FUN_0041c180` attaches them: for each class named, from the last, it makes `count` FX objects at the host, the copies `2π / count` of phase apart from the last back, at the class's altitude plus the host's, tied to the host (`+0x44`) and its serial (`+0x48`). `CreateObject` (`00413800`) does it for each `ADD FX` of a class: `Final-PC` carries `Final-PC-FX-1` to 5, and many tower-three enemies `Circular-blue-stars`. Touch-and-destroy, Invulnerability and Banana attach theirs to the player (`FUN_0041c340`, see [Surprises.md](Surprises.md)).

The FX part of `ObjectUpdateWeapon` (`00444ca0`, which updates every attribute of an object) runs in the full object update:

- **Timers**: while the on timer runs, `FUN_004452c0` switches the check-event on the host (Invulnerability sets object flag `0x800000`); when it runs out the off timer starts and switches it off; when that runs out the on timer starts again and the object shows.
- **Host check** (`FUN_0041c0b0`): `bonus-invulnerability` (`0xc9`) holds while the host has flag `0x800000`, `bonus-touch-and-destroy` (`0xcd`) while it has `0x400000`, `malus-banana` (`0xcf`) while Banana is its surprise, and `do-fx` (`0x10d`) always. When the host is gone or the event no longer holds, the object is destroyed, or, when its class has an on-timeout, hidden until its next on time.
- **Orbit**: a circular one stands at `ray` from the host at the angle `clock * angular-speed + phase` (the clock as a float), its offset (sin - cos, cos + sin) through the collision-to-world scales, and with a vertical amplitude its altitude is `sin(clock * vertical-pulsation + vertical-phase) * amplitude` plus the host's. A place outside the level keeps the last one. Its `CREATE` blocks leave trails like a shot's.

So the Final-PC's fifteen orbiters keep it invulnerable (`ApplyDamage` takes nothing) for 10 seconds, then hide and leave it open for 2, over and over. `--verify-game` checks that cycle in `CPU\T3P2` and that Touch-and-destroy puts fifteen stars round the player and takes them away. The port keeps orbiters apart from the objects, in `FCombat::Orbiters`, and draws them like FX. Not ported: `only-when-visible` (FX flag 4), for which no reader was found.

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

`Active = 1` makes the first birth come at the first update. As the level starts, `EngineActivateObject` (`0043c150`) sets up every placed generator whatever its `Active`: with something solid on it (`ObjectOverlapsSolid`) the generator opens (object `+0x5c` bit 1) and its flag 2 clears, so it waits; otherwise flag 2 is set and it stands closed, so its first birth comes at the first update. With `Stable-population`, `ObjectDropContents` takes a dead product off its generator's births, so it makes another.

`AnimInitializeGraphics` gives a generator its body's first frame, held (object `+0x1b4`), and its `Shadow-gaf`'s animation 1 (object `+0x1b8`), the working light of `V-GENERATOR.GAF` (16 frames), beside the shadow its `Shadow-index` (2) names. `ObjectDraw` (kind 0x17) plays the light and draws it at the body's place, its depth 0x168 (90 rows) deeper, while the generator stands closed; open (object `+0x5c` bit 1, passable) it hides the light and sets it back to its first frame.

## Wrecks

A class with `Two-pieces` (class `+0x20` bit 2: tanks and turrets) dies twice. On the first death `EngineUpdateObjects` calls `ObjectDestroy` with its keep flag: the explosions go off and the reward is paid, but nothing drops and the enemy count stays. The object becomes a wreck (object `+0x128` bit `0x20`, Only-base, which other classes may start with): its energy and shield go back to the class's initial values, its shield step drops to zero, and its behaviour becomes plain Fixed (8), so it neither moves nor looks for the player. `ObjectDraw` leaves out a wreck's upper half, a tank's top or a turret's gun, and keeps the turret's base (the animation after its 24 gun poses) and the shadow. The second death destroys it for good. Gold boxes leave `Gold` (500 money), and many classes drop a `Surprise` power-up by chance.

## Turrets

Turrets (`Behaviour = Fixed Seeker`, kind Turret) draw a rotating top from the 24 animations at `GAF-index` and a fixed base from animation 25. `SeekerUpdate` (`0042c5a0`) runs their `TYPE Seeker` block every step:

- Unlocked, the scan timer counts down; when it runs out it is refilled by `Scan-Delay` and `SeekerFindTarget` (`0042da80`) looks for the target within `Sensor-Range` collision units and inside the vision cone around the base's facing. `PathStartSearch` (`0040f6f0`) then routes to it.
- Locked, the timer counts down while the turret engages. When it runs out and the target has moved since the lock, the lock drops and the next step scans again.
- The pending route search advances every 17 ms. Fixed seekers abort when the route needs another expansion; a turret accepts only a direct two-point route in one of the four collision-axis direction sectors. See [PathSearch.md](PathSearch.md).
- Engaging (`SeekerEngage`, `0042caa0`) needs that direct route within `Min-distance`. The top turns one pose per `Turret-turn-timeout` toward the line to the target (`SeekerTurnTop`, `0042d150`), using the previous step's direction.
- `SeekerAimAndFire` (`0042d2d0`) raises the weapon slope while the target is within the weapon's `Fire-Range`, and pulls the trigger when the weapon is ready and the top faces the target. The weapon update then fires as for the player.

The port runs the original incremental route search from collision centre to collision centre against the terrain graph. Turret shots pass through the turret that fired them and damage the player's shield and energy; blasts reach the player too.

## Impacts

`AmmoImpact` (`0043d350`) puts the ammo's `Hit-fx` where a shot struck. A shot that sank to the ground without striking anything leaves a `Water-Splash` where its cell's terrain marker reads `0x100`, open water (`FUN_0042c130` with no margin), and a `Bullet-Ground-Impact` elsewhere. Either goes at the spot clamped into the level, only near the view (`FUN_00436690`: within a screen of it each way), and a class with `Uniq-at-xy` (keyword `0x47`, class flag `0x1000000`: Flame-Smoke, Little-Smoke and three trails) only where none of its own stands at exactly that spot (`FUN_0043dc40`). The port measures the near view from the player rather than the clamped camera.

## Bounce sparks

A hard knock (more than 2 units of travel with speed and weight) calls `FUN_0043d140` before its damage. It acts for a class with `Spark-on-hit-point` (keyword `0x3f`, class flag `0x20000000`), and for the player under Pinball, whose sparks are always fx.3dc's `Flipper-Bong-Up-Left` and the other three. A class's four sides are `Bounce-fx-up-left`, `-up-right`, `-down-left` and `-down-right` (class `+0xd0` to `+0xdc`), all four set by `Bounce-fx`. The mover's position against the struck point picks the side: up-left of it, the down-right spark, and so on. The spark appears at the struck point 8 pixels higher, in whole pixels, only while the mover is within a screen of the view each way (`FUN_00436690`) and no object of its class stands in that map cell. Only `Grenade-Ammo` (`Small-Metal-Bounce-Spark`) and `Saw-Ammo-Empty` (`Saw-Bounce-Spark`) name one; the things with the flag have none. `--verify-game` checks that bouncing grenades throw sparks.

## Not ported yet

- The idle look-around of two-piece bodies, which turn their top (`FUN_00444b80`): no two-piece class can turn on the spot, so it never runs.

`--verify-game` fires every level's player weapon for six seconds of game time while spinning, then puts the player in clear view of a turret for three seconds; it fails if no turret fires in those duels.
