#pragma once

#include "Pg/Gaf.h"

#include <cstdint>
#include <memory>
#include <string_view>

namespace Sound
{
class FMixer;
}

// UpdateAnimation at 0045e7e0 holds each frame for its duration in ticks; VzRun fixes the tick at 14 ms through AnimationSetFixedPeriod (0045e4c0), so the display period never applies.
inline constexpr int32_t AnimationPeriodMilliseconds = 14;

// __ftol truncates into 64 bits and its callers keep the low half; NaN and out-of-range values give the integer indefinite, whose low half is zero.
constexpr int32_t TruncateLow(const double Value)
{
    constexpr double Limit = 9223372036854775808.0;
    return Value > -Limit && Value < Limit ? int32_t(uint32_t(uint64_t(int64_t(Value)))) : 0;
}

// An object that plays its animation's sounds where it stands.
// AnimInitializeGraphics (00417b30) stamps, through AnimStampSoundChannels (00417a20), the class's Sample-channel-range and Sample-channel-priority on its GAF's sound descriptors, and ObjectDraw hands UpdateAnimation the object's position and altitude, which makes the sounds positional (AnimationPlaySound at 0045e6e0).
struct FSoundEmitter
{
    float X = 0.f;
    float Y = 0.f;
    float Altitude = 0.f;
    // Zero plays the sound anywhere at priority 0x7f, as for the panel's animations.
    uint32_t ChannelMask = 0;
    uint32_t Priority = 0;
};

// The channel an animation's positional sound plays on and the sound's serial there (animation instance +0x30 and +0x44).
struct FSoundTrack
{
    uint32_t Channel = 0;
    uint32_t Serial = 0;
};

// An animation instance's frame and timer, as StartAnimation (0045e770) resets them and UpdateAnimation (0045e7e0) advances them.
struct FAnimationPlayback
{
    // 1-based.
    uint32_t Frame = 0;
    int32_t Timer = 0;
    bool bEnded = false;
    FSoundTrack Sound;
};

// UpdateAnimation at 0045e7e0. Object animations neither run backwards nor play back and forth, so only looping is handled.
void UpdateAnimation(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, int32_t Milliseconds, const FSoundEmitter* Emitter = nullptr);
FAnimationPlayback StartAnimation(const Gaf::FAnimation& Animation, const FSoundEmitter* Emitter = nullptr);
// ObjectDraw at 00431ae0 shows a chosen frame by stepping back to the one before it and replaying the time left, which lands on it.
void ShowFrame(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, uint32_t Frame, const FSoundEmitter* Emitter = nullptr);
// UpdateAnimation's sound part, for animations stepped elsewhere: a frame with event flag 1 starts its GAF's sample, and a positional sound follows its emitter while it plays.
void PlayFrameSound(const Gaf::FAnimation& Animation, const Gaf::FAnimationFrame& Frame, FSoundTrack& Track, const FSoundEmitter* Emitter);
void FollowFrameSound(FSoundTrack& Track, const FSoundEmitter* Emitter);
// The mixer that frames with a sound event play on; none leaves animations silent, as the verify modes run them.
void SetAnimationMixer(Sound::FMixer* Mixer);
// The port's sound for an object's weapon: positional like a frame's sound, and while the Track's last sound still plays it takes that channel, so a new shot cuts the last one short instead of piling onto another channel.
void PlayTrackedSound(std::shared_ptr<const Sound::FSample> Sample, FSoundTrack& Track, const FSoundEmitter& Emitter);
bool IsTrackPlaying(const FSoundTrack& Track);
// SoundSetListener (0045edb0): EngineUpdateObjects puts the listener at the player's position and altitude on each update.
void SetSoundListener(float X, float Y, float Altitude);
// The class's Sample-channel-range ("first, last", 1-based, FUN_00407b70) as a channel mask and its Sample-channel-priority.
FSoundEmitter MakeSoundEmitter(std::string_view ChannelRange, std::string_view Priority, float X, float Y, float Altitude);
