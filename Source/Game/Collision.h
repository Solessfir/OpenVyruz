#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct FLevel;

struct FCollisionPoint
{
    float X = 0.f;
    float Y = 0.f;
};

struct FCollisionCircle
{
    FCollisionPoint Center;
    float Radius = 0.f;
};

struct FStaticCollisionShape
{
    std::array<FCollisionPoint, 4> Points;
    size_t PointCount = 2;
    bool bWater = false;
};

struct FTerrainHit
{
    size_t ShapeIndex = 0;
    FCollisionPoint Point;
    bool bWater = false;
};

class FTerrainCollision
{
public:
    std::span<const FStaticCollisionShape> GetShapes() const { return Shapes; }

private:
    struct FEndpoint
    {
        float Position;
        size_t ShapeIndex;
        bool bMaximum;
    };

    friend std::expected<FTerrainCollision, std::string> BuildTerrainCollision(const FLevel& Level);
    friend std::optional<FTerrainHit> QueryTerrainCollision(const FTerrainCollision& Terrain, FCollisionCircle Previous, FCollisionCircle Candidate);

    std::vector<FStaticCollisionShape> Shapes;
    std::array<std::vector<FEndpoint>, 2> Endpoints;
};

FCollisionPoint ToCollisionSpace(FCollisionPoint World);
FCollisionPoint ToWorldSpace(FCollisionPoint Collision);
std::expected<FTerrainCollision, std::string> BuildTerrainCollision(const FLevel& Level);
std::optional<FCollisionPoint> IntersectTerrainShape(FCollisionCircle Previous, FCollisionCircle Candidate, const FStaticCollisionShape& Shape);
std::optional<FTerrainHit> QueryTerrainCollision(const FTerrainCollision& Terrain, FCollisionCircle Previous, FCollisionCircle Candidate);
std::expected<void, std::string> VerifyTerrainCollision();
