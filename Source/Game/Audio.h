#pragma once

#include "Pg/Sound.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

class FResourceArchive;
struct FPlayerState;
struct FSoundEmitter;
struct FSoundTrack;
struct FWeaponClass;

// The audio options vzconf.txt sets (ConfigLoad at 0042f570): music (option flag 0x400), sound-fx (0x800) and voices (0x1000), and their volumes at session +0x27e4, +0x27e8 and +0x27ec.
// The defaults are the archive's own VZCONF.TXT.
struct FAudioSettings
{
    bool bMusic = true;
    bool bSoundFx = true;
    bool bVoices = true;
    int MusicVolume = Sound::MaxVolume;
    int SoundFxVolume = Sound::MaxVolume / 2;
    int VoicesVolume = Sound::MaxVolume;
    // voice-type (session +0x280c): 1 the English lines, 2 the other language's, which this build keeps under french.
    uint32_t VoiceType = 1;
};

// The voice lines of VoiceLoad (00411cb0), in the order of its table (session +0x208c), each Data\Sound\<language>\v-<name>.wav.
// Slot 25 is never loaded.
enum class EVoice : uint8_t
{
    Code,
    Key,
    Bonus,
    Malus,
    Gun,
    Grenade,
    Rockets,
    Flame,
    Saw,
    Homing,
    Lazer,
    Pulsar,
    Mine,
    NeedCode,
    NeedKey,
    NeedCash,
    GameSaved,
    GameOver,
    HighSpeed,
    LowSpeed,
    Help,
    Inventory,
    Map,
    Menu,
    Pause,
    Unused,
    ShieldLow,
    Warning,
    WatchOut,
    TakeCare,
    Locked,
    WakeUp,
    WaitingFor,
    WhatDoing,
    ForgetMission,
    Ariba,
    Laugh,
    Looser,
    SadMan,
    WellDone,
    Yeepee,
    Yeah,
    ComeOn,
    Hey,
    Ooh,
    Yo,
    GiveMe,
    TheBest,
    WantMore,
};

// VoiceQueue (00413200): queues a line to start after Delay ms, holding the voice channel for Duration ms and the queue entry Hold ms longer.
// A line already queued is not queued again, and without voices nothing is. Returns whether it was queued.
bool QueueVoice(EVoice Voice, uint32_t Delay, uint32_t Duration, uint32_t Hold);
// VoiceQueueClear (00411170): drops every queued line.
void ClearVoices();
// The session's game clock (+0x1020), whose low bits pick among several lines; the level loop sets it each step.
void SetVoiceClock(uint32_t Clock);
uint32_t GetVoiceClock();
// EngineUpdateObjects at 004406e0, on each update of the player, with the fire input that ReadPlayerInput (00438540) returns.
// Firing without a break of 3 seconds cheers three times in eight; 20 seconds without firing, after the first shot, brings one of four lines that wake the player up.
void StepPlayerVoices(uint32_t Clock, bool bFiring);

// How the game shares the mixer's channels: music on 1, the player's weapon and death on 2, the player's motor on 4 and voices on 5; animation sounds take any idle one.
class FAudio
{
public:
    static constexpr uint32_t MusicChannel = 1;
    static constexpr uint32_t PlayerWeaponChannel = 2;
    static constexpr uint32_t MotorChannel = 4;
    static constexpr uint32_t VoiceChannel = 5;

    explicit FAudio(FResourceArchive& InArchive);
    ~FAudio();
    FAudio(const FAudio&) = delete;
    FAudio& operator=(const FAudio&) = delete;

    // Opens the output in the format and hands the mixer to the animations; false leaves the game silent.
    bool Open(const Sound::FOutputFormat& Format);
    // The Audio menu's Quality, Frequency and Channels reopen the output (SoundOpen at 0045f300).
    void SetOutputFormat(const Sound::FOutputFormat& Format) { Mixer.Open(Format); }

    // SoundApplyFxVolume (0042f280) and EngineApplyDisplayAndCursor (0042f2d0): every channel but music and voices plays at the sound-fx volume, or silent with sound-fx off; music and voices take their own volumes while they are on.
    void Apply(const FAudioSettings& InSettings);

    // MusicStart (0042ec70): loads the three music loops through MusicLoad (0042eaf0), techno, darken and hardcore, and with music on loops the first.
    void StartMusic();
    // MusicSwitch (0042ed60): switches to another tune and remembers the one it replaced; the darkness and the extasy surprises play tunes 2 and 3.
    void SwitchMusic(int Track);
    // MusicRestore (0042ed00): goes back to the remembered tune.
    void RestoreMusic();
    // The Music menu's switch: on loops the first tune, off stops the music.
    void SetMusicOn(bool bOn);
    int GetMusicTrack() const { return Track; }

    // RunMenu at 00421050 plays bip.wav when it returns, centred, at priority 0xbf.
    void PlayMenuAccept();
    // MenuStep at 00421780 plays electric.wav, hard left, with the VZ fire flash.
    void PlayMenuFlash();
    // FUN_00445dd0 plays electric.wav, centred, at priority 0xbf with each of the finale's flashes.
    void PlayFinaleFlash();
    // LoadFadeSound (0041e560) loads fade.wav, which the menu's closing fade and the game's end play at priority 0xbf.
    void PlayFade();
    // PlayIntro (004033b0) leaves the intro's sound to MCI, which plays it at full volume while the music is stopped.
    void PlayMovieSound(std::shared_ptr<const Sound::FSample> Sample);
    void StopMovieSound();
    // ShowStoryPage (00447c80) reads the story aloud, v-intro-uk.wav on channel 5 at the voices volume and priority 0xbf, and stops it when the page closes.
    void PlayStoryVoice();
    void StopStoryVoice();
    // The whole game, sound included, stands still while it is inactive.
    void SetPaused(bool bPaused) { Mixer.SetPaused(bPaused); }
    // MenuMain (004271a0), MenuGame (004223e0) and the menu fade FUN_0041e5a0, which also fades each level in, set every channel's pan back to the middle (SoundSetPan at 0045ff00).
    // Without it a channel keeps the last pan given to a sound on it, such as the menu flash's hard left on channel 2, the player's weapon channel.
    void CenterPans();
    // The reverb follows the zone of the level playing (its path's folder: cave, hitek, underground or cpu); an empty path, as in the menus, has none.
    void SetLevelZone(std::string_view LevelPath);

    // PlayerUpdateMotorSound (00445c50), every frame of a level: while the player's motor works its loop plays on channel 4, louder with speed, from a third of the sound-fx volume up; a blocked motor tries start-engine.wav when the player thrusts.
    void UpdateMotor(const FPlayerState& Player, float ThrustMax, bool bBlockedMotor, bool bThrusting);
    // EngineRunLevel at 0043e5f0 stops the player's weapon and motor channels when the level ends, and the motor alone when the player dies.
    void StopLevelChannels();
    void StopMotor();

    // ObjectsUpdateWeaponSounds (0043df70) for the player, each frame, on channel 2 at priority 0xbf.
    // Firing, a weapon with Fire-start-sound plays it first and its Fire-sound once that ends; one without plays Fire-sound straight away, looping with Fire-sound-loop, else again whenever it ends.
    // Letting go plays Fire-end-sound.
    // An empty stock plays No-ammo-sound. bFiring is object flag 2, which a jammed weapon, a blocked motor and invisibility clear; bStock tells whether a stock is bound (object +0x248) and Rounds what it holds.
    void UpdatePlayerWeapon(const FWeaponClass& Weapon, bool bFiring, bool bStock, float Rounds);
    // Any other armed object, on its weapon class's Sample-channel-range at its Sample-channel-priority, each frame; bShot tells whether it fired since the last.
    // The original names no channel for these, so its playing checks see nothing and a sound starts on another channel each frame of firing, piling up at full volume wherever the object is.
    // The port gives the object one positional sound (SoundTrack) instead: a shot restarts it, a looping weapon's fire sound plays through and its end sound follows the last shot, and an empty stock plays No-ammo-sound.
    // bSounding is the object flag 0x20000.
    void UpdateObjectWeapon(const FWeaponClass& Weapon, bool& bSounding, FSoundTrack& SoundTrack, bool bShot, float Rounds, const FSoundEmitter& Emitter);
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

    // VoiceQueueUpdate (004132e0): sets the voice channel to the voices volume and starts each queued line once its time comes, through VoicePlay (004133a0), which plays it on channel 5 at priority 0xbf only once the line before has had its duration; a line that comes too early is lost.
    // Entries leave the queue once held long enough.
    void UpdateVoices();
    bool AddVoice(EVoice Voice, uint32_t Delay, uint32_t Duration, uint32_t Hold);
    void DropVoices();

private:
    // VoiceLoad (00411cb0) takes the lines from the voice type's folder.
    std::string_view VoiceFolder() const;
    // SoundBankLoad (00403a40): a sound by file name from Data\Sound, loaded once.
    std::shared_ptr<const Sound::FSample> Load(std::string_view Name);
    // SoundPlayOnChannel (00403f80): restarts a channel with the sample.
    void PlayOnChannel(const std::shared_ptr<const Sound::FSample>& Sample, uint32_t Channel, bool bLoop, bool bHighPriority);
    // ObjectsUpdateWeaponSounds' state machine; bStarting and bSounding are the object flags 0x10000 and 0x20000.
    void UpdateWeapon(const FWeaponClass& Weapon, bool& bStarting, bool& bSounding, bool bFiring, bool bStock, float Rounds, uint32_t Channel,
        uint32_t Mask, uint32_t Priority, bool bMayLoop);

    static constexpr int TrackCount = 3;

    FResourceArchive& Archive;
    Sound::FMixer Mixer;
    // Applies the reverb option to the zone in play.
    void ApplyReverb();

    FAudioSettings Settings;
    std::string Zone;
    std::unordered_map<std::string, std::shared_ptr<const Sound::FSample>> Bank;
    std::array<std::shared_ptr<const Sound::FSample>, TrackCount + 1> Tracks;
    // DAT_005237c8 and DAT_005237cc: the tune playing (1 to 3, zero for none) and the one it replaced.
    int Track = 0;
    int PreviousTrack = 0;
    struct FQueuedVoice
    {
        EVoice Voice = EVoice::Code;
        uint64_t Start = 0;
        uint32_t Duration = 0;
        uint64_t End = 0;
        bool bStarted = false;
    };
    // DAT_00523598 and DAT_005235a0: the queue and the time the voice channel stays taken.
    std::vector<FQueuedVoice> Voices;
    uint64_t VoiceBusyUntil = 0;
    // Object flags 0x10000 and 0x20000 of the player: its weapon's start sound or fire sound is under way.
    bool bWeaponStarting = false;
    bool bWeaponFiring = false;
};
