#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>

inline constexpr uint32_t DemoStepMilliseconds = 10;
inline constexpr uint32_t MillisecondsPerSecond = 1000;
inline constexpr uint32_t MillihertzPerHertz = 1000;

// PgMeasureRefreshRate at 004504a0 uses integer division, including its no-rate fallback.
constexpr uint32_t FramePeriodMilliseconds(const uint32_t RefreshMillihertz)
{
    constexpr uint32_t PeriodNumerator = MillisecondsPerSecond * MillihertzPerHertz;
    return RefreshMillihertz == 0 ? PeriodNumerator : PeriodNumerator / RefreshMillihertz;
}

// PgVideoElapsedMs at 00450d10 returns one display period on its first poll and clamps negative clock deltas.
class FFrameClock
{
public:
    constexpr explicit FFrameClock(const uint32_t FramePeriod) : FramePeriod(FramePeriod) {}

    constexpr uint32_t Poll(const uint32_t NowMilliseconds)
    {
        const uint32_t Previous = LastMilliseconds;
        LastMilliseconds = NowMilliseconds;
        return Previous == 0 ? FramePeriod : uint32_t(std::max(0, std::bit_cast<int32_t>(NowMilliseconds - Previous)));
    }

private:
    uint32_t FramePeriod;
    uint32_t LastMilliseconds = 0;
};

// EngineRunLevel at 0043e5f0 consumes the remainder in full, with a 10 ms cap for recording and playback.
constexpr uint32_t NextGameStep(uint32_t& RemainingMilliseconds, const uint32_t FramePeriod, const bool bDemo)
{
    const uint32_t Step = std::min(RemainingMilliseconds, bDemo ? DemoStepMilliseconds : FramePeriod);
    RemainingMilliseconds -= Step;
    return Step;
}

bool VerifyFrameLoop();
