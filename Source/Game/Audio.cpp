#include "Game/Audio.h"

#include "Game/Animation.h"
#include "Game/Engine.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <format>

namespace
{
constexpr uint32_t ChannelBit(const uint32_t Channel)
{
    return 1u << (Channel - 1);
}

// The menus and the music play at these priorities (RunMenu, MenuStep, MusicStart (0042ec70)); SoundPlayOnChannel (00403f80) raises its own by 0x40 on request.
constexpr uint32_t MenuPriority = 0xbf;
constexpr uint32_t MusicPriority = 0xff;
constexpr uint32_t ChannelPriority = 0x7f;
constexpr uint32_t HighPriorityBoost = 0x40;

// The sound that the voice functions reach, as the original's globals do.
FAudio* VoiceAudio = nullptr;
uint32_t VoiceClock = 0;

// PgClockNow: the voice queue runs on real time.
uint64_t NowMilliseconds()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr std::array<std::string_view, size_t(EVoice::WantMore) + 1> VoiceNames = {
    "code", "key", "bonus", "malus", "gun", "grenade", "rockets", "flame", "saw", "homing", "lazer", "pulsar", "mine", "need-code", "need-key",
    "need-cash", "game-saved", "game-over", "high-speed", "low-speed", "help", "inventory", "map", "menu", "pause", "", "shield-low", "warning",
    "watch-out", "take-care", "locked", "wake-up", "waiting-for", "what-doing", "forget-mission", "ariba", "laugh-1", "looser", "sad-man",
    "well-done", "yeepee", "yeah", "come-on", "hey", "ooh", "yo", "give-me", "the-best", "want-more",
};
}

bool QueueVoice(const EVoice Voice, const uint32_t Delay, const uint32_t Duration, const uint32_t Hold)
{
    return VoiceAudio && VoiceAudio->AddVoice(Voice, Delay, Duration, Hold);
}

void ClearVoices()
{
    if (VoiceAudio)
    {
        VoiceAudio->DropVoices();
    }
}

void SetVoiceClock(const uint32_t Clock)
{
    VoiceClock = Clock;
}

void StepPlayerVoices(const uint32_t Clock, const bool bFiring)
{
    // DAT_00523810 and DAT_00523814: the last update without and with fire, or the last line they brought.
    // Like the originals they outlive the level.
    static uint32_t LastQuiet = 0;
    static uint32_t LastFire = 0;
    constexpr uint32_t PickMask = 7;
    if (bFiring)
    {
        constexpr uint32_t CheerAfter = 3000;
        LastFire = Clock;
        if (LastQuiet == 0 || Clock - LastQuiet < CheerAfter)
        {
            return;
        }

        constexpr std::array Cheers = { EVoice::Ariba, EVoice::Yeepee, EVoice::Yeah };
        constexpr uint32_t CheerDelay = 300;
        constexpr uint32_t CheerDuration = 2000;
        if (const uint32_t Pick = Clock & PickMask; Pick < Cheers.size())
        {
            QueueVoice(Cheers[Pick], CheerDelay, CheerDuration, 0);
        }

        LastQuiet = Clock;
        return;
    }

    constexpr uint32_t IdleAfter = 20000;
    LastQuiet = Clock;
    if (LastFire == 0 || Clock - LastFire < IdleAfter)
    {
        return;
    }

    constexpr std::array WakeUps = { EVoice::WakeUp, EVoice::WaitingFor, EVoice::WhatDoing, EVoice::ForgetMission };
    constexpr uint32_t WakeUpDuration = 3000;
    if (const uint32_t Pick = Clock & PickMask; Pick < WakeUps.size())
    {
        QueueVoice(WakeUps[Pick], 0, WakeUpDuration, 0);
    }

    LastFire = Clock;
}

uint32_t GetVoiceClock()
{
    return VoiceClock;
}

FAudio::FAudio(FResourceArchive& InArchive)
    : Archive(InArchive)
{
}

FAudio::~FAudio()
{
    if (VoiceAudio == this)
    {
        VoiceAudio = nullptr;
    }

    SetAnimationMixer(nullptr);
}

bool FAudio::Open()
{
    if (!Mixer.Open())
    {
        return false;
    }

    SetAnimationMixer(&Mixer);
    VoiceAudio = this;
    return true;
}

std::shared_ptr<const Sound::FSample> FAudio::Load(const std::string_view Name)
{
    std::string Key(Name);
    std::ranges::transform(Key, Key.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
    if (Key.empty())
    {
        return nullptr;
    }

    if (const auto Found = Bank.find(Key); Found != Bank.end())
    {
        return Found->second;
    }

    std::shared_ptr<const Sound::FSample> Sample;
    if (const std::optional<std::vector<uint8_t>> File = Archive.Read(std::format("common\\data\\sound\\{}", Key)))
    {
        if (std::optional<Sound::FSample> Decoded = Sound::DecodeWav(*File))
        {
            Sample = std::make_shared<const Sound::FSample>(std::move(*Decoded));
        }
    }

    Bank.emplace(Key, Sample);
    return Sample;
}

void FAudio::PlayOnChannel(const std::shared_ptr<const Sound::FSample>& Sample, const uint32_t Channel, const bool bLoop, const bool bHighPriority)
{
    if (!Sample)
    {
        return;
    }

    if (Mixer.IsPlaying(Channel))
    {
        Mixer.Stop(ChannelBit(Channel));
    }

    Mixer.Play(Sample, bLoop, ChannelPriority + (bHighPriority ? HighPriorityBoost : 0), ChannelBit(Channel));
}

void FAudio::Apply(const FAudioSettings& InSettings)
{
    Settings = InSettings;
    for (uint32_t Channel = 1; Channel <= Sound::FMixer::ChannelCount; ++Channel)
    {
        if (Channel != MusicChannel && Channel != VoiceChannel)
        {
            Mixer.SetVolume(Channel, Settings.bSoundFx ? Settings.SoundFxVolume : 0);
        }
    }

    if (Settings.bMusic)
    {
        Mixer.SetVolume(MusicChannel, Settings.MusicVolume);
    }

    if (Settings.bVoices)
    {
        Mixer.SetVolume(VoiceChannel, Settings.VoicesVolume);
    }
}

void FAudio::StartMusic()
{
    if (Track < 1 || Track > TrackCount)
    {
        constexpr std::array<std::string_view, TrackCount> Names = { "techno.wav", "darken.wav", "hardcore.wav" };
        for (int Index = 0; Index < TrackCount; ++Index)
        {
            Tracks[size_t(Index) + 1] = Load(Names[size_t(Index)]);
        }

        if (Settings.bMusic)
        {
            PreviousTrack = Track;
            Track = 1;
        }
    }

    if (Tracks[size_t(Track)])
    {
        Mixer.SetPan(MusicChannel, 0);
        Mixer.SetVolume(MusicChannel, Settings.MusicVolume);
        Mixer.Play(Tracks[size_t(Track)], true, MusicPriority, ChannelBit(MusicChannel));
    }
}

void FAudio::SwitchMusic(const int NewTrack)
{
    if (NewTrack == Track)
    {
        return;
    }

    Mixer.Stop(ChannelBit(MusicChannel));
    PreviousTrack = Track;
    Track = NewTrack;
    StartMusic();
}

void FAudio::RestoreMusic()
{
    if (Track >= 1 && Track <= TrackCount)
    {
        Mixer.Stop(ChannelBit(MusicChannel));
        Track = PreviousTrack;
        PreviousTrack = 0;
    }

    if (Track != 0)
    {
        StartMusic();
    }
}

void FAudio::PlayMenuAccept()
{
    Mixer.Play(Load("bip.wav"), false, MenuPriority, Sound::FMixer::AllChannels, std::nullopt, 0);
}

void FAudio::PlayMenuFlash()
{
    Mixer.Play(Load("electric.wav"), false, MenuPriority, Sound::FMixer::AllChannels, std::nullopt, -Sound::MaxPan);
}

void FAudio::PlayFinaleFlash()
{
    Mixer.Play(Load("electric.wav"), false, MenuPriority, Sound::FMixer::AllChannels, std::nullopt, 0);
}

void FAudio::UpdateMotor(const FPlayerState& Player, const float ThrustMax, const bool bBlockedMotor, const bool bThrusting)
{
    // The motor reads the sound-fx volume itself, so it plays even with sound-fx off.
    const int Full = Settings.SoundFxVolume;
    const int Idle = Full / 3;
    if (bBlockedMotor)
    {
        if (!Mixer.IsPlaying(MotorChannel) && bThrusting)
        {
            Mixer.SetVolume(MotorChannel, Full);
            PlayOnChannel(Load("start-engine.wav"), MotorChannel, false, true);
        }

        return;
    }

    if (!Mixer.IsPlaying(MotorChannel))
    {
        PlayOnChannel(Load("vz-motor.wav"), MotorChannel, true, true);
    }

    const int Volume = ThrustMax == 0.f ? Idle : TruncateLow(double(Full - Idle) * double(Player.Speed) / double(ThrustMax) + double(Idle));
    Mixer.SetVolume(MotorChannel, std::max(Volume, Idle));
}

void FAudio::StopLevelChannels()
{
    Mixer.Stop(ChannelBit(PlayerWeaponChannel) | ChannelBit(MotorChannel));
}

void FAudio::StopMotor()
{
    Mixer.Stop(ChannelBit(MotorChannel));
}

void FAudio::UpdatePlayerWeapon(const FWeaponClass& Weapon, const bool bFiring, const bool bStock, const float Rounds)
{
    constexpr uint32_t PlayerWeaponPriority = 0xbf;
    UpdateWeapon(Weapon, bWeaponStarting, bWeaponFiring, bFiring, bStock, Rounds, PlayerWeaponChannel, ChannelBit(PlayerWeaponChannel), PlayerWeaponPriority, true);
}

void FAudio::UpdateWeapon(const FWeaponClass& Weapon, bool& bStarting, bool& bSounding, const bool bFiring, const bool bStock, const float Rounds,
    const uint32_t Channel, const uint32_t Mask, const uint32_t Priority, const bool bMayLoop)
{
    const bool bAll = Mask == Sound::FMixer::AllChannels;
    const auto Playing = [&] { return Channel != 0 && Mixer.IsPlaying(Channel); };
    const bool bPlaying = Playing();
    // The original stops "channel 0" for a weapon on a range of channels, which stops nothing.
    const auto StopOwn = [&]
    {
        if (Channel != 0)
        {
            Mixer.Stop(ChannelBit(Channel));
        }
    };
    const auto Restart = [&](const std::string& Name, const bool bLoop)
    {
        if (!bAll && bPlaying)
        {
            StopOwn();
        }

        Mixer.Play(Load(Name), bLoop, Priority, Mask);
    };

    if (!bStock)
    {
        if (!bFiring)
        {
            if (bSounding && Weapon.bFireSoundLoops && (bAll || bPlaying))
            {
                Mixer.Stop(Mask);
            }

            return;
        }

        if (bStarting || bSounding)
        {
            bStarting = bSounding = false;
            if (!bAll && bPlaying)
            {
                StopOwn();
            }
        }

        if (!bAll && !Playing())
        {
            Mixer.Play(Load(Weapon.NoAmmoSound), false, Priority, Mask);
        }

        return;
    }

    if (!bFiring)
    {
        const bool bLoopEnds = !Weapon.FireSound.empty() && bSounding && Weapon.bFireSoundLoops;
        const bool bShotEnds = !Weapon.FireStartSound.empty() && bSounding && (bAll || !bPlaying);
        if (bLoopEnds || bShotEnds)
        {
            bStarting = bSounding = false;
            if (!bAll)
            {
                StopOwn();
            }

            Mixer.Play(Load(Weapon.FireEndSound), false, Priority, Mask);
        }

        return;
    }

    if (Weapon.FireStartSound.empty() || bSounding)
    {
        if (!Weapon.FireSound.empty() && (!bSounding || bAll || !bPlaying))
        {
            if (Rounds == 0.f)
            {
                Restart(Weapon.NoAmmoSound, false);
                return;
            }

            bStarting = false;
            bSounding = true;
            Restart(Weapon.FireSound, bMayLoop && Weapon.bFireSoundLoops);
        }

        return;
    }

    if (!bStarting)
    {
        if (Rounds != 0.f)
        {
            bSounding = false;
            bStarting = true;
        }

        Restart(Rounds == 0.f ? Weapon.NoAmmoSound : Weapon.FireStartSound, false);
        return;
    }

    if (bAll || !bPlaying)
    {
        bStarting = false;
        bSounding = !Weapon.FireSound.empty();
    }
}

void FAudio::PlayWeaponSwitch()
{
    bWeaponStarting = bWeaponFiring = false;
    PlayOnChannel(Load("weapon-switch.wav"), PlayerWeaponChannel, false, true);
}

void FAudio::PlayDeath()
{
    PlayOnChannel(Load("aaaa.wav"), PlayerWeaponChannel, false, true);
}

void FAudio::PlayPickup()
{
    constexpr uint32_t ItemChannel = 3;
    PlayOnChannel(Load("pickup-heavy.wav"), ItemChannel, false, true);
}

void FAudio::PlayMoney()
{
    constexpr uint32_t ItemChannel = 3;
    PlayOnChannel(Load("money.wav"), ItemChannel, false, true);
}

bool FAudio::AddVoice(const EVoice Voice, const uint32_t Delay, const uint32_t Duration, const uint32_t Hold)
{
    // VoiceLoad loads the lines only with voices on (EngineApplyDisplayAndCursor), in the voice-type language.
    const std::string_view Name = VoiceNames[size_t(Voice)];
    if (!Settings.bVoices || Name.empty() || std::ranges::contains(Voices, Voice, &FQueuedVoice::Voice) || !Load(std::format("english\\v-{}.wav", Name)))
    {
        return false;
    }

    const uint64_t Start = NowMilliseconds() + Delay;
    Voices.push_back({ .Voice = Voice, .Start = Start, .Duration = Duration, .End = Start + Duration + Hold });
    return true;
}

void FAudio::DropVoices()
{
    Voices.clear();
}

void FAudio::UpdateVoices()
{
    constexpr uint32_t VoicePriority = 0xbf;
    Mixer.SetVolume(VoiceChannel, Settings.VoicesVolume);
    const uint64_t Now = NowMilliseconds();
    std::erase_if(Voices, [&](FQueuedVoice& Queued)
    {
        if (!Queued.bStarted && Queued.Start <= Now)
        {
            Queued.bStarted = true;
            if (VoiceBusyUntil == 0 || VoiceBusyUntil <= Now)
            {
                VoiceBusyUntil = Now + Queued.Duration;
                if (Mixer.IsPlaying(VoiceChannel))
                {
                    Mixer.Stop(ChannelBit(VoiceChannel));
                }

                Mixer.Play(Load(std::format("english\\v-{}.wav", VoiceNames[size_t(Queued.Voice)])), false, VoicePriority, ChannelBit(VoiceChannel));
            }
        }

        return Queued.End <= Now;
    });
}

void FAudio::PlayShieldAlarm()
{
    constexpr uint32_t ItemChannel = 3;
    PlayOnChannel(Load("shield-alarm.wav"), ItemChannel, false, false);
}
