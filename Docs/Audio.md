# Audio

The original plays everything through its own channel mixer (`pgaudio.cpp`) on top of DirectSound. The port keeps that mixer's rules in `Source/Pg/Sound` and plays the result through miniaudio, whose MP3 decoder also reads the intro's sound; `Source/Game/Audio` holds the game's use of it.

## Samples

- **WAV files**: 126 RIFF WAVE files in `common\data\sound` (`SoundLoadWav`, `0045dbe0`; `SoundParseWav`, `0045da80`): 8-bit unsigned or 16-bit signed PCM, mono or stereo, at 22050 or 44100 Hz. `SoundBankLoad` (`00403a40`) loads a sound once by name and keeps it in a bank, retrying under `..\Common\`. Voices sit in `english\` and `french\`.
- **GAF samples**: 27 GAFs carry an `SMP_` entry whose slot 1 holds `SND_` records ([DataFormats.md](DataFormats.md)). Slot 0 holds one 76-byte play descriptor per sample, zero on disk; the game fills them in (see [Animation sounds](#animation-sounds)).

## Mixer

`VzOpenSound` (`004477e0`) opens the device at 22050 Hz, 16-bit stereo (`SoundOpen`, `0045f300`, format `0x2a0000`) with 16 channels (`SoundSetChannelCount`, `0045f0b0`). Each channel plays one sample at a time and keeps a priority, a volume from 0 to `0xffff` and a pan from `-0x7fff` (left) to `0x7fff` (right).

- **Play** (`SoundPlay`, `0045f7e0`) takes a sample, a loop flag, a priority and a channel mask. It uses the first idle channel of the mask. When all are busy it takes the one with the lowest priority not above the new one, or, between equal priorities, the one with the least time left (channel `+0x24`, kept by `SoundPoll`, `0045f6b0`). Without such a channel nothing plays. A caller can set the volume and pan for this one play (`DAT_0052768c`, `DAT_00527680`); otherwise the channel keeps the ones it had.
- **Volume and pan** (`SoundSetVolume`, `0045fe00`; `SoundSetPan`, `0045ff00`) stay with the channel for later samples. `SoundStop` (`0045fa00`) stops a mask of channels and `SoundChannelPlaying` (`0045fb70`) tells whether one plays.
- **The device layer** (`SoundDeviceOpen`, `0047e9c0`; `SoundDeviceUpdate`, `0047ee10`) turns a volume into a DirectSound attenuation through a table of `volume >> 8` steps: step 0 is silent (-10000), step 255 is full, and step `n` in between is `trunc(873 * ln((n + 1) / 256))` hundredths of a decibel, close to linear amplitude. The pan attenuates the far side by the table entry `255 - (|pan| >> 7)`.

The port mixes the channels itself in miniaudio's callback at the format's rate and channel count, resampling each sample linearly, with the same table; miniaudio converts the mix for the system. As with a DirectSound primary buffer in that format, an 8-bit format truncates the mix to the top byte of each 16-bit value, and a mono one ignores the pan and averages a stereo sample's sides.

### Positional sounds

`AnimationPlaySound` (`0045e6e0`) makes a sound positional when its descriptor's channel mask (`+0x1c`) is set, and plays it on that mask at the descriptor's priority (`+0x24`). `SoundSetListener` (`0045edb0`) puts the listener at the player's position and altitude on each update of the player.

- **Volume** (`SoundDistanceVolume`, `0045e4e0`): the descriptor's volume (`+0x20`) while the emitter lies within its inner box (`+0x28`) of the listener on every axis, and nothing beyond its outer box (`+0x34`). In between the original scales by the distance between two radii (`+0x40`, `+0x44`), measured from `|listener - position| - position` on each axis rather than the difference. Nothing sets those radii, so the division by zero gives no volume there either: a positional sound is heard only inside the inner box.
- **Pan** (`SoundScreenPan`, `0045e630`): the emitter's horizontal offset from the listener over half the screen width (320), full at the edge.
- **Following**: while the sound plays, each `UpdateAnimation` call on its animation sets the channel's volume and pan again, and stops the channel once the volume drops to zero. It recognises its sound by the channel's serial (`+0x3c`), so a channel taken by another sound is let go.

## Channels

| Channel | Use | Volume |
|---|---|---|
| 1 | Music | `music-volume` while `music` is on |
| 2 | The player's weapon and death | `sound-fx-volume` |
| 4 | The player's motor | Set by the motor itself |
| 5 | Voices | `voices-volume` while `voices` is on |
| Others | Animation sounds, menus and other effects | `sound-fx-volume` |

`EngineApplyDisplayAndCursor` (`0042f2d0`) and `SoundApplyFxVolume` (`0042f280`) set these from `vzconf.txt`: every channel but 1 and 5 gets the sound-fx volume, or zero with `sound-fx = 0`. `ConfigLoad` clamps the three volumes to `0..0xffff`. The port defaults to the archive's `VZCONF.TXT`: everything on, music and voices at `65535`, sound effects at `32767`.

## Music

The music is three WAV loops, not the CD (`MusicLoad`, `0042eaf0`): 1 `techno.wav`, 2 `darken.wav` and 3 `hardcore.wav`.

- **Start**: `MusicStart` (`0042ec70`) loads them and, with music on, loops tune 1 on channel 1 at priority `0xff`, centred, at the music volume. `VzRun` starts it after the story page and the intro, before the menus, and it plays through the menus and the levels.
- **Switch and restore**: `MusicSwitch` (`0042ed60`) changes the tune and remembers the old one; `MusicRestore` (`0042ed00`) brings it back.
- **Surprises**: `SurpriseStart` switches to tune 2 for the darkness surprise and tune 3 for the extasy surprise, and `SurpriseEnd` restores.
- **Level end**: `LoopLevels` goes back to tune 1 after each level.

## Animation sounds

`UpdateAnimation` (`0045e7e0`) asks `AnimationFrameSound` (`0045ea70`) for a sound each time it enters a frame. A frame with event flag 1 names its GAF's sample by event value, from 1. A zero value or a missing sample plays nothing, which four frames in `panel.gaf`, `v-panel.gaf` and `circular-fx.gaf` have. `--verify-gaf` decodes every WAV and GAF sample and counts the events: 55 play.

`AnimInitializeGraphics` (`00417b30`) stamps each object's GAF descriptors through `AnimStampSoundChannels` (`00417a20`): the class's `Sample-channel-range` (keyword `0x3b`, `first, last`, class `+0xe8`) as the channel mask, its `Sample-channel-priority` (keyword `0x3c`, class `+0xec`) as the priority, channel 2's volume (the sound-fx volume) as the volume, and the boxes `DAT_0050d420` (400 on each axis) and `DAT_0050d430` (800). Each `.3DC` file sets these in its `DEFAULT-PROPERTY` block: FX on channels 6 to 8 at priority 130, ammo on 9 to 12 at 128, weapons on 9 to 16 at 129, common decor on 13 and 14, everything else on 1 to 16 at 127. `ObjectDraw` hands the animation instance the object's position and altitude, so object sounds are positional. The panel's animations are never stamped, so their sounds play on any idle channel at priority `0x7f` with the channel's own volume and pan.

In the port, FX effects (explosions, impacts) and gates and lifts play their sounds from where they are. A fresh animation instance has no position until `ObjectDraw` gives it one, so the frame a door shows when the level loads sounds from the origin, which keeps it silent. Not checked yet: the original advances `Graphic-update-needed` classes (gates, lifts, FX) in the object update, outside `ObjectDraw`'s position copy; whether their instance position then lags behind the object.

## Menus

`MenuLoadArt` (`00420560`) loads `bip.wav` and `electric.wav`. `RunMenu` (`00421050`) plays `bip.wav` when it returns, centred, at priority `0xbf` on any channel. `MenuStep` (`00421780`) plays `electric.wav` hard left with each VZ fire flash. The hover sound (`DAT_00523770`) is never loaded, so moving between items is silent.

## Motor

`PlayerUpdateMotorSound` (`00445c50`) runs each frame while the player has energy and no lift is taking it away (`DAT_00523820`). With a working motor it loops `vz-motor.wav` on channel 4 (`SoundPlayOnChannel`, `00403f80`, at priority `0xbf`). It sets the channel's volume to `trunc((fx - fx / 3) * speed / Thrust.Max + fx / 3)`, at least `fx / 3`, where `fx` is the sound-fx volume (read directly, so the motor plays even with sound-fx off). While the motor is blocked, it plays `start-engine.wav` once at the full sound-fx volume when the player pushes forward and channel 4 is free. `EngineRunLevel` stops channels 2 and 4 when the level ends.

## Weapons

The `TYPE Weapon` block names its sounds (`LoadObjectClassLibrary`, keywords `0x98` to `0x9c`): `Fire-sound` (`+0x34`), `Fire-start-sound` (`+0x38`), `Fire-end-sound` (`+0x3c`), `No-ammo-sound` (`+0x40`), and `Fire-sound-loop` (flag `0x10` at `+0x30`). `ObjectsUpdateWeaponSounds` (`0043df70`) runs each frame over the armed objects, keeping two object flags: `0x10000` while the start sound plays and `0x20000` while the fire sound does. The player uses channel 2 at priority `0xbf`; others use their weapon's ADD Weapon channel, or the weapon class's `Sample-channel-range` and `Sample-channel-priority`. Its trigger is object flag 2, which a jammed weapon, a blocked motor and invisibility clear.

- **Firing**: a weapon with a start sound plays it first, then marks the fire sound once it ends; the fire sound then plays, looping for the player when `Fire-sound-loop` is set and otherwise again whenever it ends. A weapon without a start sound plays its fire sound straight away. A bound stock (object `+0x248`) that has run dry plays `No-ammo-sound` instead.
- **Letting go**: after a looping fire sound, or after any fire sound of a weapon with a start sound once the channel is free, it stops the channel and plays `Fire-end-sound`.
- **No stock bound**: firing ends both states and plays `No-ammo-sound` whenever the channel is free; letting go stops a looping fire sound.

`PlayerSelectWeapon` (`00437740`) ends both states and plays `weapon-switch.wav` on channel 2.

The pass covers every object of the level (session `+0x2010`) except ammo, FX, items, lifts, gates and things. Other objects never loop their fire sound. With no ADD Weapon channel they play on the weapon class's `Sample-channel-range` (weapons: 9 to 16) at its `Sample-channel-priority` (129). Their "channel" is then 0, so every playing check sees nothing and every stop stops nothing: a fire sound starts again on each frame of firing. The port takes a turret's or actor's trigger as the shots its weapon took since the last frame.

## Other sounds

`LoadSessionSounds` (`0043bc30`) loads the session's own sounds. `SoundPlayOnChannel` (`00403f80`) restarts a channel with one, at priority `0x7f` or `0xbf`:

| Sound | Channel | When |
|---|---|---|
| `pickup-heavy.wav` | 3 | Every item `ConsumeItem` (`00444660`) takes |
| `weapon-switch.wav` | 2 | Every weapon switch |
| `aaaa.wav` | 2 | The player's death (`EngineUpdateObjects`) |
| `shield-alarm.wav` | 3 | `ShieldAlarmUpdate` (`0043e370`), each frame with the panel in mode 0 or 1: once a second while the shield is at 1% or less. With HUD Green Info it also keeps "DANGER : SHIELD IS LOW" up at 100, 100 for two seconds; the other modes show the bars' own warning |
| `money.wav` | 3 | The open map's drain crossing a multiple of 5000 cash (`PanelUpdate`), and a saving plot's payment (`SavingPlotUpdate`) |
| `fade.wav` | any | The gamma fades (not ported) |

## Voices

`VoiceLoad` (`00411cb0`) loads 48 lines from `Data\Sound\<language>\v-*.wav` into a table (session `+0x208c`) when voices are on: code, key, bonus, malus, the nine weapon slots (gun, grenade, rockets, flame, saw, homing, lazer, pulsar, mine), need-code, need-key, need-cash, game-saved, game-over, high-speed, low-speed, help, inventory, map, menu, pause, an unused slot, shield-low, warning, watch-out, take-care, locked, wake-up, waiting-for, what-doing, forget-mission, ariba, laugh-1, looser, sad-man, well-done, yeepee, yeah, come-on, hey, ooh, yo, give-me, the-best and want-more. The port plays the English set.

`VoiceQueue` (`00413200`) queues a line with a delay, a duration and a hold; a line already queued is not queued again. `VoiceQueueUpdate` (`004132e0`) runs each frame (and inside the pause and before the game menu): it sets channel 5 to the voices volume and starts each line whose time has come through `VoicePlay` (`004133a0`), which plays it on channel 5 at priority `0xbf` only once the previous line's duration has passed. A line that comes too early is lost. An entry leaves the queue after its delay, duration and hold. `VoiceQueueClear` (`00411170`) empties it. The queue runs on real time (`PgClockNow`); the random choices take the low bits of the game clock (session `+0x1020`).

| Line | Delay, duration, hold (ms) | When |
|---|---|---|
| Gun to Mine | 750, 500, 0 | `PlayerSelectWeapon`: the slot taken, 1 to 9 |
| Pause, Map | 0, 1000, 5000 | `EngineReadActionKeys`: P and M |
| Menu | 0, 1000, 2000 | Escape while alive, played at once |
| Need-cash | 0, 1000, 5000 | The map refused (`MapNeedsCash`) or a saving plot refused |
| Game-saved | 500, 1000, 3000 | A saving plot saved |
| Need-key, Need-code | 0, 1000, 5000 | A gate refuses |
| Key, Code | 0, 1000, 0 | `ConsumeItem`: a key, a code part or a whole code |
| Bonus / Malus | 0, 2000 / 1000, 0 | `ConsumeItem`: a surprise by its name (`SurpriseIsBonus`, `0041bcf0`), none for Nop |
| Ariba or Yeepee | 3000, 2000, 0 | `SurpriseStart`: the extasy surprise, by clock `& 3` |
| Laugh, Yeepee, Yeah, Hey, Yo, Give-me | 0, 1000, 0 | Picking up money, by clock `& 15` (six in sixteen) |
| Yeah, Hey, Yo, Give-me, The-best, Want-more, Laugh | 1000, 1500, 0 | `VoiceKillCheer` (`0043d070`): the player's shot kills, by clock `& 15` |
| Ooh, Looser, Sad-man | 500, 2000, 5000 | `AreaDamage`: a blast of 300 or more on the living player, by clock `& 7` |
| Warning, Watch-out, Take-care, Locked | 0, 1000, 10000 | `PanelUpdate` with HUD Green Info while the player is locked, by clock `& 3` |
| Shield-low | 0, 1000, 15000 | `PanelUpdate` with HUD Green Info, the bars showing a shield at 1% or less, while alive |
| Ariba, Yeepee, Yeah | 300, 2000, 0 | `EngineUpdateObjects`: firing for 3 seconds without a break, by clock `& 7` |
| Wake-up, Waiting-for, What-doing, Forget-mission | 0, 3000, 0 | `EngineUpdateObjects`: 20 seconds without firing, by clock `& 7` |
| Well-done | 0, 1000, 10000 | `ObjectUpdateLevelExit`, after emptying the queue |
| Game-over | 0, 1000, 0 | `EngineGameOver`, after emptying the queue |

The fire input is what `ReadPlayerInput` (`00438540`) returns. The two timers behind the last two rows (`DAT_00523810`, `DAT_00523814`) start at zero and outlive the level, so the wake-up lines start only after the player's first shot.

## Not ported yet

- **ADD Weapon channels**: an ADD Weapon entry's own channel (`+0x18`) is not read; every object uses its weapon class's range.
- **Other animated objects**: items, things, FX and the map animations play their animation sounds from where they are.
