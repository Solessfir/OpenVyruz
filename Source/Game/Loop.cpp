#include "Game/Loop.h"

#include <array>

namespace
{
constexpr bool CheckFrameLoop()
{
    if (FramePeriodMilliseconds(60000) != 16 || FramePeriodMilliseconds(144000) != 6 || FramePeriodMilliseconds(0) != 1000000)
    {
        return false;
    }

    FFrameClock Clock(16);
    if (Clock.Poll(100) != 16 || Clock.Poll(100) != 0 || Clock.Poll(135) != 35 || Clock.Poll(120) != 0 || Clock.Poll(130) != 10)
    {
        return false;
    }

    for (const bool bDemo : { false, true })
    {
        uint32_t Remaining = 35;
        const std::array<uint32_t, 4> Expected = bDemo ? std::array<uint32_t, 4>{ 10, 10, 10, 5 } : std::array<uint32_t, 4>{ 16, 16, 3, 0 };
        for (const uint32_t Step : Expected)
        {
            if (NextGameStep(Remaining, 16, bDemo) != Step)
            {
                return false;
            }
        }

        if (Remaining != 0)
        {
            return false;
        }
    }

    return true;
}
static_assert(CheckFrameLoop());
}

bool VerifyFrameLoop()
{
    return CheckFrameLoop();
}
