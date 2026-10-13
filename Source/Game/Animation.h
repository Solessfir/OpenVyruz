#pragma once

#include "Pg/Gaf.h"

#include <cstdint>

// UpdateAnimation at 0045e7e0 holds each frame for its duration in ticks; VzRun fixes the tick at 14 ms through AnimationSetFixedPeriod
// (0045e4c0), so the display period never applies.
inline constexpr int32_t AnimationPeriodMilliseconds = 14;

// __ftol truncates into 64 bits and its callers keep the low half; NaN and out-of-range values give the integer indefinite, whose low half is zero.
constexpr int32_t TruncateLow(const double Value)
{
    constexpr double Limit = 9223372036854775808.0;
    return Value > -Limit && Value < Limit ? int32_t(uint32_t(uint64_t(int64_t(Value)))) : 0;
}

// An animation instance's frame and timer, as StartAnimation (0045e770) resets them and UpdateAnimation (0045e7e0) advances them.
struct FAnimationPlayback
{
    // 1-based.
    uint32_t Frame = 0;
    int32_t Timer = 0;
    bool bEnded = false;
};

// UpdateAnimation at 0045e7e0. Object animations neither run backwards nor play back and forth, so only looping is handled.
void UpdateAnimation(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, int32_t Milliseconds);
FAnimationPlayback StartAnimation(const Gaf::FAnimation& Animation);
// ObjectDraw at 00431ae0 shows a chosen frame by stepping back to the one before it and replaying the time left, which lands on it.
void ShowFrame(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, uint32_t Frame);
