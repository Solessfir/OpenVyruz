#include "Game/Audio.h"

#include "Game/Animation.h"
#include "Game/Engine.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <cctype>
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
}

FAudio::FAudio(FResourceArchive& InArchive)
    : Archive(InArchive)
{
}

bool FAudio::Open()
{
    if (!Mixer.Open())
    {
        return false;
    }

    SetAnimationMixer(&Mixer);
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

void FAudio::UpdatePlayerWeapon(const FWeaponClass& Weapon, const bool bFiring, const bool bStock, const float Rounds)
{
    constexpr uint32_t WeaponPriority = 0xbf;
    const uint32_t Mask = ChannelBit(PlayerWeaponChannel);
    const bool bPlaying = Mixer.IsPlaying(PlayerWeaponChannel);
    const auto Restart = [&](const std::string& Name, const bool bLoop)
    {
        if (bPlaying)
        {
            Mixer.Stop(Mask);
        }

        Mixer.Play(Load(Name), bLoop, WeaponPriority, Mask);
    };

    if (!bStock)
    {
        if (!bFiring)
        {
            if (bWeaponFiring && Weapon.bFireSoundLoops && bPlaying)
            {
                Mixer.Stop(Mask);
            }

            return;
        }

        if (bWeaponStarting || bWeaponFiring)
        {
            bWeaponStarting = bWeaponFiring = false;
            if (bPlaying)
            {
                Mixer.Stop(Mask);
            }
        }

        if (!Mixer.IsPlaying(PlayerWeaponChannel))
        {
            Mixer.Play(Load(Weapon.NoAmmoSound), false, WeaponPriority, Mask);
        }

        return;
    }

    if (!bFiring)
    {
        const bool bLoopEnds = !Weapon.FireSound.empty() && bWeaponFiring && Weapon.bFireSoundLoops;
        const bool bShotEnds = !Weapon.FireStartSound.empty() && bWeaponFiring && !bPlaying;
        if (bLoopEnds || bShotEnds)
        {
            bWeaponStarting = bWeaponFiring = false;
            Mixer.Stop(Mask);
            Mixer.Play(Load(Weapon.FireEndSound), false, WeaponPriority, Mask);
        }

        return;
    }

    if (Weapon.FireStartSound.empty() || bWeaponFiring)
    {
        if (!Weapon.FireSound.empty() && (!bWeaponFiring || !bPlaying))
        {
            if (Rounds == 0.f)
            {
                Restart(Weapon.NoAmmoSound, false);
                return;
            }

            bWeaponStarting = false;
            bWeaponFiring = true;
            Restart(Weapon.FireSound, Weapon.bFireSoundLoops);
        }

        return;
    }

    if (!bWeaponStarting)
    {
        if (Rounds != 0.f)
        {
            bWeaponFiring = false;
            bWeaponStarting = true;
        }

        Restart(Rounds == 0.f ? Weapon.NoAmmoSound : Weapon.FireStartSound, false);
        return;
    }

    if (!bPlaying)
    {
        bWeaponStarting = false;
        bWeaponFiring = !Weapon.FireSound.empty();
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

void FAudio::PlayShieldAlarm()
{
    constexpr uint32_t ItemChannel = 3;
    PlayOnChannel(Load("shield-alarm.wav"), ItemChannel, false, false);
}
