# Audio

The original plays everything through its own channel mixer (`pgaudio.cpp`) on top of DirectSound. The port keeps that mixer's rules in `Source/Pg/Sound` and plays the result through miniaudio; `Source/Game/Audio` holds the game's use of it.

## Samples

- **WAV files**: 126 RIFF WAVE files in `common\data\sound` (`SoundLoadWav`, `0045dbe0`; `SoundParseWav`, `0045da80`): 8-bit unsigned or 16-bit signed PCM, mono or stereo, at 22050 or 44100 Hz. `SoundBankLoad` (`00403a40`) loads a sound once by name and keeps it in a bank, retrying under `..\Common\`. Voices sit in `english\` and `french\`.
- **GAF samples**: 27 GAFs carry an `SMP_` entry whose slot 1 holds `SND_` records ([DataFormats.md](DataFormats.md)). Slot 0 holds one 76-byte descriptor per sample, and every one of them is zero in the data, so no GAF sound is positional (see below).

## Mixer

`VzOpenSound` (`004477e0`) opens the device at 22050 Hz, 16-bit stereo (`SoundOpen`, `0045f300`, format `0x2a0000`) with 16 channels (`SoundSetChannelCount`, `0045f0b0`). Each channel plays one sample at a time and keeps a priority, a volume from 0 to `0xffff` and a pan from `-0x7fff` (left) to `0x7fff` (right).

- **Play** (`SoundPlay`, `0045f7e0`) takes a sample, a loop flag, a priority and a channel mask. It uses the first idle channel of the mask. When all are busy it takes the one with the lowest priority not above the new one, or, between equal priorities, the one with the least time left (channel `+0x24`, kept by `SoundPoll`, `0045f6b0`). Without such a channel nothing plays. A caller can set the volume and pan for this one play (`DAT_0052768c`, `DAT_00527680`); otherwise the channel keeps the ones it had.
- **Volume and pan** (`SoundSetVolume`, `0045fe00`; `SoundSetPan`, `0045ff00`) stay with the channel for later samples. `SoundStop` (`0045fa00`) stops a mask of channels and `SoundChannelPlaying` (`0045fb70`) tells whether one plays.
- **The device layer** (`SoundDeviceOpen`, `0047e9c0`; `SoundDeviceUpdate`, `0047ee10`) turns a volume into a DirectSound attenuation through a table of `volume >> 8` steps: step 0 is silent (-10000), step 255 is full, and step `n` in between is `trunc(873 * ln((n + 1) / 256))` hundredths of a decibel, close to linear amplitude. The pan attenuates the far side by the table entry `255 - (|pan| >> 7)`.

The port mixes the channels itself in miniaudio's callback, resampling each sample linearly to the device rate, with the same table.

### Positional sounds

`AnimationPlaySound` (`0045e6e0`) makes a sound positional when its descriptor's channel mask (`+0x1c`) is set. Its volume then comes from `SoundDistanceVolume` (`0045e4e0`) against the listener, which `SoundSetListener` (`0045edb0`) sets to the player's position and altitude each update. Inside the descriptor's inner box (`+0x28`) the volume is the descriptor's own (`+0x20`); beyond its outer box (`+0x34`) it is zero. In between, the original scales it by the distance between two radii (`+0x40`, `+0x44`), but it measures that distance from `|listener - position| - position` on each axis rather than from the difference. Its pan comes from `SoundScreenPan` (`0045e630`), the horizontal offset over half the screen width. No data uses this path, so the port does not implement it.

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

- **Start**: `MusicStart` (`0042ec70`) loads them and, with music on, loops tune 1 on channel 1 at priority `0xff`, centred, at the music volume. `VzRun` starts it before the menus, and it plays through the menus and the levels.
- **Switch and restore**: `MusicSwitch` (`0042ed60`) changes the tune and remembers the old one; `MusicRestore` (`0042ed00`) brings it back.
- **Surprises**: `SurpriseStart` switches to tune 2 for the darkness surprise and tune 3 for the extasy surprise, and `SurpriseEnd` restores.
- **Level end**: `LoopLevels` goes back to tune 1 after each level.

## Animation sounds

`UpdateAnimation` (`0045e7e0`) asks `AnimationFrameSound` (`0045ea70`) for a sound each time it enters a frame. A frame with event flag 1 names its GAF's sample by event value, from 1; a zero value or a missing sample plays nothing. Four frames have one (`panel.gaf`, `v-panel.gaf` and `circular-fx.gaf`). Since every descriptor is zero, the sound takes any idle channel at priority `0x7f` and keeps that channel's volume and pan. This covers explosions, gates, lifts, items, mines and the other FX: 55 sound events. `--verify-gaf` decodes every WAV and GAF sample and counts the events.

## Menus

`MenuLoadArt` (`00420560`) loads `bip.wav` and `electric.wav`. `RunMenu` (`00421050`) plays `bip.wav` when it returns, centred, at priority `0xbf` on any channel. `MenuStep` (`00421780`) plays `electric.wav` hard left with each VZ fire flash. The hover sound (`DAT_00523770`) is never loaded, so moving between items is silent.

## Motor

`PlayerUpdateMotorSound` (`00445c50`) runs each frame while the player has energy and no lift is taking it away (`DAT_00523820`). With a working motor it loops `vz-motor.wav` on channel 4 (`SoundPlayOnChannel`, `00403f80`, at priority `0xbf`). It sets the channel's volume to `trunc((fx - fx / 3) * speed / Thrust.Max + fx / 3)`, at least `fx / 3`, where `fx` is the sound-fx volume (read directly, so the motor plays even with sound-fx off). While the motor is blocked, it plays `start-engine.wav` once at the full sound-fx volume when the player pushes forward and channel 4 is free. `EngineRunLevel` stops channels 2 and 4 when the level ends.

## Not ported yet

- **Weapon sounds**: `ObjectsUpdateWeaponSounds` (`0043df70`) and the weapon blocks' `fire-sound`, `fire-sound-loop`, `fire-start-sound`, `fire-end-sound` and `no-ammo-sound`. The player uses channel 2 at priority `0xbf`; others use their weapon's channel and priority.
- **Voices**: 48 lines in `english\` and `french\`, on channel 5 through `VoicePlay` (`004133a0`) and the queue `VoiceQueueUpdate` (`004132e0`).
- **Other sounds**: the player's death (channel 2), `shield-alarm.wav`, `money.wav`, `pickup-heavy.wav`, `weapon-switch.wav`, and `fade.wav` with the gamma fades.
- **Options**: the audio menus.
