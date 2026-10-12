#pragma once

#include "Game/Collision.h"

#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct FPathObstacle
{
    FCollisionPoint Min;
    FCollisionPoint Max;
    bool bDisabled = false;
    bool bWater = false;
    bool bActor = false;
};

enum class EPathStatus : int
{
    Searching = -2,
    Found = -1,
    Failed = 0,
    Finished = 1,
};

struct FPathRoute
{
    std::vector<FCollisionPoint> Points;
    size_t Current = 0;
};

class FPathSearch
{
public:
    bool Start(FCollisionPoint From, FCollisionPoint To);
    EPathStatus Advance(std::span<const FPathObstacle> Obstacles, FCollisionPoint HalfExtent, float MaxDistance = 0.f, bool bIgnoreWater = false);
    std::span<const FPathRoute> GetRoutes() const { return Routes; }
    std::optional<FPathRoute> SelectFinishedRoute(std::span<const FPathObstacle> Obstacles, FCollisionPoint HalfExtent, bool bIgnoreWater = false) const;

private:
    std::vector<FPathRoute> Routes;
    std::vector<FCollisionPoint> GeneratedPoints;
};

std::expected<void, std::string> VerifyPathSearch();
