#pragma once

#include <cstdint>

// The pg library's shared random generator (RandomNext at 00454080, RandomRange, RandomBelow): a rotating state multiplied by a step counter.
class FRandom
{
public:
    // VzLoadLevel at 0041f7c0 reseeds (RandomSeed at 00454030) with these values before every level.
    static constexpr uint32_t LevelSeedState = 0xFEABCEFE;
    static constexpr uint32_t LevelSeedCounter = 0xABCDEFDA;

    // RandomNext returns the product before rotating the state by it.
    constexpr uint32_t Next()
    {
        const uint32_t Product = Counter * State;
        ++Counter;
        State = (State >> ((0u - Product) & RotateMask)) | (State << (Product & RotateMask));
        return Product;
    }

    // RandomRange at 004540c0: Low + Product * (High - Low + 1) / 2^32, truncated the way __ftol does.
    constexpr int Range(const int Low, const int High)
    {
        const int64_t Scaled = int64_t(High - Low + 1) * int64_t(Next());
        return Low - int(-Scaled / ProductScale);
    }

    // RandomBelow at 00454130.
    constexpr int Below(const int Count)
    {
        return int(int64_t(Count) * int64_t(Next()) / ProductScale);
    }

private:
    static constexpr uint32_t RotateMask = 31;
    static constexpr int64_t ProductScale = int64_t{ 1 } << 32;

    uint32_t State = LevelSeedState;
    uint32_t Counter = LevelSeedCounter;
};

namespace RandomCheck
{
consteval bool CheckRandom()
{
    FRandom Random;
    for (int Index = 0; Index < 1000; ++Index)
    {
        const int Value = Random.Range(-5, 7);
        if (Value < -5 || Value > 7 || Random.Below(10) >= 10)
        {
            return false;
        }
    }

    return true;
}
static_assert(CheckRandom());
}
