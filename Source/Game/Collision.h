#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

// AnimInitializeBounds at 0041a5d0 gives objects this radius unless the class sets Bounding-Box-Size.
inline constexpr float DefaultCollisionRadius = 20.f;

struct FCollisionCircle
{
    FCollisionPoint Center;
    float Radius = DefaultCollisionRadius;
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
    // CellsHaveMarker (0042c130) with value 0xff: every marker cell within Margin of (X, Y), clipped to the grid, is enclosed ground on all four sides.
    bool IsOpenGround(int X, int Y, int Margin) const;
    // CellsHaveMarker (0042c130) with value 0x100 and no margin, as AmmoImpact asks: the marker cell is open water.
    bool IsWater(int X, int Y) const;
    int GetMarkerWidth() const { return int(MarkerWidth); }
    int GetMarkerHeight() const { return int(MarkerHeight); }

private:
    struct FEndpoint
    {
        float Position;
        size_t ShapeIndex;
        bool bMaximum;
    };

    friend std::expected<FTerrainCollision, std::string> BuildTerrainCollision(const FLevel& Level);
    friend std::optional<FTerrainHit> QueryTerrainCollision(const FTerrainCollision& Terrain, FCollisionCircle Previous, FCollisionCircle Candidate, bool bIgnoreWater);

    std::vector<FStaticCollisionShape> Shapes;
    std::array<std::vector<FEndpoint>, 2> Endpoints;
    // The BuildTerrainMarkers grid, kept at session +0x2038 for movers and impacts.
    std::vector<uint32_t> Markers;
    size_t MarkerWidth = 0;
    size_t MarkerHeight = 0;
};

FCollisionPoint ToCollisionSpace(FCollisionPoint World);
FCollisionPoint ToWorldSpace(FCollisionPoint Collision);
std::expected<FTerrainCollision, std::string> BuildTerrainCollision(const FLevel& Level);
std::optional<FCollisionPoint> IntersectTerrainShape(FCollisionCircle Previous, FCollisionCircle Candidate, const FStaticCollisionShape& Shape);
// Over-water objects (helis) pass water edges.
std::optional<FTerrainHit> QueryTerrainCollision(const FTerrainCollision& Terrain, FCollisionCircle Previous, FCollisionCircle Candidate, bool bIgnoreWater = false);
// True when the straight line from From to To crosses no solid terrain edge; water does not block it.
bool IsLineClear(const FTerrainCollision& Terrain, FCollisionPoint From, FCollisionPoint To);
// Circle against circle at 004069d0: the moving circle's candidate centre touches a resting circle.
std::optional<FCollisionPoint> IntersectCircles(FCollisionCircle Moving, FCollisionCircle Resting);
// Contact selection at 0041b770: true when Next replaces Previous as the contact nearest Center.
bool PrefersContact(FCollisionPoint Previous, FCollisionPoint Next, FCollisionPoint Center);
std::expected<void, std::string> VerifyTerrainCollision();
