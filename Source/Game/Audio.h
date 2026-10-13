#pragma once

#include "Pg/Sound.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

class FResourceArchive;
struct FPlayerState;
struct FWeaponClass;

// The audio options vzconf.txt sets (ConfigLoad at 0042f570): music (option flag 0x400), sound-fx (0x800) and voices (0x1000), and their volumes
// at session +0x27e4, +0x27e8 and +0x27ec. The defaults are the archive's own VZCONF.TXT.
struct FAudioSettings
{
    bool bMusic = true;
    bool bSoundFx = true;
    bool bVoices = true;
    int MusicVolume = Sound::MaxVolume;
    int SoundFxVolume = Sound::MaxVolume / 2;
    int VoicesVolume = Sound::MaxVolume;
};

// How the game shares the mixer's channels: music on 1, the player's weapon and death on 2, the player's motor on 4 and voices on 5; animation
// sounds take any idle one.
class FAudio
{
public:
    static constexpr uint32_t MusicChannel = 1;
    static constexpr uint32_t PlayerWeaponChannel = 2;
    static constexpr uint32_t MotorChannel = 4;
    static constexpr uint32_t VoiceChannel = 5;

    explicit FAudio(FResourceArchive& InArchive);

    // Opens the output and hands the mixer to the animations; false leaves the game silent.
    bool Open();

    // SoundApplyFxVolume (0042f280) and EngineApplyDisplayAndCursor (0042f2d0): every channel but music and voices plays at the sound-fx volume, or silent with
    // sound-fx off; music and voices take their own volumes while they are on.
    void Apply(const FAudioSettings& InSettings);

    // MusicStart (0042ec70): loads the three music loops through MusicLoad (0042eaf0), techno, darken and hardcore, and with music on loops the first.
    void StartMusic();
    // MusicSwitch (0042ed60): switches to another tune and remembers the one it replaced; the darkness and the extasy surprises play tunes 2 and 3.
    void SwitchMusic(int Track);
    // MusicRestore (0042ed00): goes back to the remembered tune.
    void RestoreMusic();
    int GetMusicTrack() const { return Track; }

    // RunMenu at 00421050 plays bip.wav when it returns, centred, at priority 0xbf.
    void PlayMenuAccept();
    // MenuStep at 00421780 plays electric.wav, hard left, with the VZ fire flash.
    void PlayMenuFlash();

    // PlayerUpdateMotorSound (00445c50), every frame of a level: while the player's motor works its loop plays on channel 4, louder with speed, from a third of the
    // sound-fx volume up; a blocked motor tries start-engine.wav when the player thrusts.
    void UpdateMotor(const FPlayerState& Player, float ThrustMax, bool bBlockedMotor, bool bThrusting);
    // EngineRunLevel at 0043e5f0 stops the player's weapon and motor channels when the level ends.
    void StopLevelChannels();

    // ObjectsUpdateWeaponSounds (0043df70) for the player, each frame, on channel 2 at priority 0xbf. Firing, a weapon with Fire-start-sound
    // plays it first and its Fire-sound once that ends; one without plays Fire-sound straight away, looping with Fire-sound-loop, else again
    // whenever it ends. Letting go plays Fire-end-sound. An empty stock plays No-ammo-sound. bFiring is object flag 2, which a jammed weapon,
    // a blocked motor and invisibility clear; bStock tells whether a stock is bound (object +0x248) and Rounds what it holds.
    void UpdatePlayerWeapon(const FWeaponClass& Weapon, bool bFiring, bool bStock, float Rounds);
    // PlayerSelectWeapon (00437740): weapon-switch.wav on channel 2, which also ends the weapon's sound states.
    void PlayWeaponSwitch();
    // The player's death cry, aaaa.wav, on channel 2 (EngineUpdateObjects at 004406e0).
    void PlayDeath();
    // ConsumeItem (00444660): pickup-heavy.wav on channel 3 for every item taken.
    void PlayPickup();
    // money.wav on channel 3: the map's drain crossing a multiple of 5000 (PanelUpdate) and a saving plot's payment (SavingPlotUpdate).
    void PlayMoney();
    // ShieldAlarmUpdate (0043e370): shield-alarm.wav on channel 3.
    void PlayShieldAlarm();

private:
    // SoundBankLoad (00403a40): a sound by file name from Data\Sound, loaded once.
    std::shared_ptr<const Sound::FSample> Load(std::string_view Name);
    // SoundPlayOnChannel (00403f80): restarts a channel with the sample.
    void PlayOnChannel(const std::shared_ptr<const Sound::FSample>& Sample, uint32_t Channel, bool bLoop, bool bHighPriority);

    static constexpr int TrackCount = 3;

    FResourceArchive& Archive;
    Sound::FMixer Mixer;
    FAudioSettings Settings;
    std::unordered_map<std::string, std::shared_ptr<const Sound::FSample>> Bank;
    std::array<std::shared_ptr<const Sound::FSample>, TrackCount + 1> Tracks;
    // DAT_005237c8 and DAT_005237cc: the tune playing (1 to 3, zero for none) and the one it replaced.
    int Track = 0;
    int PreviousTrack = 0;
    // Object flags 0x10000 and 0x20000 of the player: its weapon's start sound or fire sound is under way.
    bool bWeaponStarting = false;
    bool bWeaponFiring = false;
};
