#include "Game/Collision.h"

#include "Game/Level.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace
{
// Collision space is the isometric world rotated 45 degrees and rescaled so a 64x32 cell becomes a square.
// The float literals are the ones the exe uses: half a cell diagonal (16 * sqrt(5)), 2 / sqrt(5) and 1 / sqrt(5).
constexpr float CellDiagonal = 35.77709f;
constexpr float WorldFromCollisionX = 0.8944272f;
constexpr float WorldFromCollisionY = 0.4472136f;
constexpr float Sqrt5 = 2.2360680f;
static_assert(CellDiagonal - BlockHeight / 2 * Sqrt5 < 1e-4f && BlockHeight / 2 * Sqrt5 - CellDiagonal < 1e-4f);
static_assert(WorldFromCollisionX * Sqrt5 - 2.f < 1e-6f && 2.f - WorldFromCollisionX * Sqrt5 < 1e-6f);
static_assert(WorldFromCollisionY * 2.f == WorldFromCollisionX);
constexpr float InverseBlockWidth = 1.f / BlockWidth;
constexpr float InverseBlockHeight = 1.f / BlockHeight;
constexpr float CollisionInset = 0.5f;

// Directional edge markers written around a terrain cell by BuildTerrainMarkers; each is a 4-bit pattern rotated within one byte.
constexpr uint32_t MarkerUpperLeft = 0x1e;
constexpr uint32_t MarkerUpperRight = 0x78;
constexpr uint32_t MarkerLowerLeft = 0x87;
constexpr uint32_t MarkerLowerRight = 0xe1;
constexpr uint32_t MarkerEdgeMask = 0xff;
// Water is flagged above the edge byte so it never forms an edge itself.
constexpr uint32_t WaterMarker = 0x100;
constexpr size_t NoShape = std::numeric_limits<size_t>::max();

constexpr bool IsEdgeMarker(const uint32_t Marker)
{
    return Marker == MarkerUpperLeft || Marker == MarkerUpperRight || Marker == MarkerLowerLeft || Marker == MarkerLowerRight;
}

struct FBounds
{
    FCollisionPoint Min;
    FCollisionPoint Max;
};

FBounds Bounds(const FStaticCollisionShape& Shape)
{
    FBounds Result{ .Min = Shape.Points[0], .Max = Shape.Points[0] };
    for (size_t Index = 1; Index < Shape.PointCount; ++Index)
    {
        Result.Min.X = std::min(Result.Min.X, Shape.Points[Index].X);
        Result.Min.Y = std::min(Result.Min.Y, Shape.Points[Index].Y);
        Result.Max.X = std::max(Result.Max.X, Shape.Points[Index].X);
        Result.Max.Y = std::max(Result.Max.Y, Shape.Points[Index].Y);
    }

    return Result;
}

FBounds Bounds(FCollisionCircle Circle)
{
    return {
        .Min = { .X = Circle.Center.X - Circle.Radius, .Y = Circle.Center.Y - Circle.Radius },
        .Max = { .X = Circle.Center.X + Circle.Radius, .Y = Circle.Center.Y + Circle.Radius },
    };
}

bool Between(float Value, float A, float B)
{
    return (A <= Value && Value <= B) || (B <= Value && Value <= A);
}

bool PointInCircle(FCollisionCircle Circle, FCollisionPoint Point)
{
    const float X = Circle.Center.X - Point.X;
    const float Y = Circle.Center.Y - Point.Y;
    return std::sqrt(X * X + Y * Y) <= Circle.Radius;
}

// Contact selection at 0041b770 accepts the later contact on equal distances.
bool KeepPreviousContact(FCollisionPoint Previous, FCollisionPoint Next, FCollisionPoint Center)
{
    const float PX = std::abs(Previous.X - Center.X);
    const float PY = std::abs(Previous.Y - Center.Y);
    const float NX = std::abs(Next.X - Center.X);
    const float NY = std::abs(Next.Y - Center.Y);
    if (NX <= PX && PY >= NY)
    {
        return false;
    }

    if (PX < NX && PY < NY)
    {
        return true;
    }

    return PX * PX + PY * PY < NX * NX + NY * NY;
}

// CircleLineContact at 00406fd0, including its reversed-edge coordinate quirk.
bool CircleLineContact(FCollisionCircle Circle, FCollisionPoint A, FCollisionPoint B, FCollisionPoint& Contact)
{
    const bool bEndpointInside = PointInCircle(Circle, A) || PointInCircle(Circle, B);
    const bool bVertical = A.X == B.X;
    if (!bEndpointInside)
    {
        const FBounds Box = Bounds(Circle);
        if (bVertical)
        {
            if (!Between(A.X, Box.Min.X, Box.Max.X) || !Between(Circle.Center.Y, A.Y, B.Y))
            {
                return false;
            }

            Contact = { .X = A.X, .Y = Circle.Center.Y };
        }
        else
        {
            if (!Between(A.Y, Box.Min.Y, Box.Max.Y) || !Between(Circle.Center.X, A.X, B.X))
            {
                return false;
            }

            Contact = { .X = Circle.Center.X, .Y = A.Y };
        }

        return true;
    }

    if (bVertical)
    {
        Contact.X = A.X;
        if (A.Y <= B.Y)
        {
            Contact.Y = Between(Circle.Center.Y, A.Y, B.Y) ? Circle.Center.Y :
                (std::abs(Circle.Center.Y - A.Y) < std::abs(Circle.Center.Y - B.Y) ? A.Y : B.Y);
            return true;
        }
    }
    else
    {
        Contact.Y = A.Y;
        if (B.X < A.X)
        {
            Contact.Y = A.Y <= Circle.Center.Y && Circle.Center.Y <= B.Y ? Circle.Center.Y :
                (std::abs(Circle.Center.Y - A.Y) < std::abs(Circle.Center.Y - B.Y) ? A.Y : B.Y);
            return true;
        }
    }

    Contact.X = A.X <= Circle.Center.X && Circle.Center.X <= B.X ? Circle.Center.X :
        (std::abs(Circle.Center.X - A.X) < std::abs(Circle.Center.X - B.X) ? A.X : B.X);
    return true;
}

// SweptCircleLineContact at 00407360 deliberately does not clamp to segment ends.
bool SweptCircleLineContact(FCollisionCircle Previous, FCollisionCircle Candidate, FCollisionPoint A, FCollisionPoint B, FCollisionPoint& Contact)
{
    const FBounds OldBox = Bounds(Previous);
    const FBounds NewBox = Bounds(Candidate);
    if (A.X == B.X)
    {
        if (!((OldBox.Max.X <= A.X && A.X <= NewBox.Min.X) || (NewBox.Max.X <= A.X && A.X <= OldBox.Min.X)))
        {
            return false;
        }

        const float Radius = NewBox.Max.X - Candidate.Center.X;
        const float EdgeCenter = Candidate.Center.X - Previous.Center.X < 0.f ? A.X + Radius : A.X - Radius;
        Contact.X = A.X;
        Contact.Y = ((EdgeCenter - Previous.Center.X) * (Candidate.Center.Y - Previous.Center.Y)) /
            (Candidate.Center.X - Previous.Center.X) + Previous.Center.Y;
    }
    else
    {
        if (!((OldBox.Max.Y <= A.Y && A.Y <= NewBox.Min.Y) || (NewBox.Max.Y <= A.Y && A.Y <= OldBox.Min.Y)))
        {
            return false;
        }

        const float Radius = NewBox.Max.Y - Candidate.Center.Y;
        const float EdgeCenter = Candidate.Center.Y - Previous.Center.Y < 0.f ? A.Y + Radius : A.Y - Radius;
        Contact.Y = A.Y;
        Contact.X = ((EdgeCenter - Previous.Center.Y) * (Candidate.Center.X - Previous.Center.X)) /
            (Candidate.Center.Y - Previous.Center.Y) + Previous.Center.X;
    }

    return true;
}
}

FCollisionPoint ToCollisionSpace(FCollisionPoint World)
{
    // AnimInitializeBounds at 0041a5d0 and AnimUpdateBounds at 0041b4b0.
    return {
        .X = (World.Y * InverseBlockHeight + World.X * InverseBlockWidth) * CellDiagonal,
        .Y = (World.Y * InverseBlockHeight - World.X * InverseBlockWidth) * CellDiagonal,
    };
}

FCollisionPoint ToWorldSpace(FCollisionPoint Collision)
{
    return { .X = (Collision.X - Collision.Y) * WorldFromCollisionX, .Y = (Collision.X + Collision.Y) * WorldFromCollisionY };
}

std::expected<FTerrainCollision, std::string> BuildTerrainCollision(const FLevel& Level)
{
    const size_t Width = Level.GridWidth;
    const size_t Height = Level.GridHeight;
    if (Width == 0 || Height == 0 || Width > std::numeric_limits<size_t>::max() / Height || Level.Cells.size() != Width * Height)
    {
        return std::unexpected("Invalid terrain collision grid");
    }

    std::vector<uint32_t> Markers(Width * Height);
    const auto Modify = [&](size_t X, size_t Y, uint32_t Mask, bool bClear)
    {
        // The original writes beyond border cells unchecked; clip untrusted input.
        if (X < Width && Y < Height)
        {
            uint32_t& Marker = Markers[X * Height + Y];
            Marker = bClear ? Marker & ~Mask : Marker | Mask;
        }
    };
    const auto Type = [&](int64_t X, int64_t Y, size_t Slot)
    {
        if (X < 0 || Y < 0 || static_cast<uint64_t>(X) >= Width || static_cast<uint64_t>(Y) >= Height)
        {
            return ETerrain::None;
        }

        const size_t Index = Level.GetCell(static_cast<uint32_t>(X), static_cast<uint32_t>(Y))[Slot];
        return Index < Level.TileTypes.size() ? Level.TileTypes[Index] : ETerrain::None;
    };

    // BuildTerrainMarkers at 0041ed30: clears must preserve cell and slot order.
    for (size_t X = 0; X < Width; ++X)
    {
        for (size_t Y = 0; Y < Height; ++Y)
        {
            for (const size_t Slot : { size_t{ 0 }, size_t{ 3 }, size_t{ 1 }, size_t{ 2 } })
            {
                const ETerrain Tile = Type(X, Y, Slot);
                const bool bFull = Tile == ETerrain::Corner || Tile == ETerrain::Ground || Tile == ETerrain::GroundLeftWall || Tile == ETerrain::GroundRightWall || Tile == ETerrain::Obstacle || Tile == ETerrain::Water;
                const bool bLower = bFull || Tile == ETerrain::HalfGround || Tile == ETerrain::HorizontalWall || Tile == ETerrain::HalfRoof;
                const bool bClear = Tile == ETerrain::Obstacle || Tile == ETerrain::HalfRoof;
                const bool bWater = Tile == ETerrain::Water;
                if (bFull)
                {
                    Modify(X, Y + 1, bWater ? WaterMarker : MarkerUpperLeft, bClear);
                    Modify(X + 1, Y + 1, bWater ? WaterMarker : MarkerUpperRight, bClear);
                }

                if (bLower)
                {
                    Modify(X, Y + 2, bWater ? WaterMarker : MarkerLowerLeft, bClear);
                    Modify(X + 1, Y + 2, bWater ? WaterMarker : MarkerLowerRight, bClear);
                }
            }
        }
    }

    struct FGeneratedShape
    {
        FStaticCollisionShape Shape;
        uint32_t Marker = 0;
        size_t X = 0;
        size_t Y = 0;
        bool bDisabled = false;
    };
    std::vector<FGeneratedShape> Generated;
    std::vector<size_t> CellShapes(Width * Height, NoShape);
    // GenerateTerrainShapes at 0041adb0 visits rows first, then columns.
    for (size_t Y = 0; Y < Height; ++Y)
    {
        for (size_t X = 0; X < Width; ++X)
        {
            uint32_t Marker = Markers[X * Height + Y] & MarkerEdgeMask;
            float CX = static_cast<float>(X + Y) * CellDiagonal;
            float CY = (static_cast<float>(Y) - static_cast<float>(X)) * CellDiagonal;
            if (Marker == 0 && Y > 0)
            {
                if (Type(X, static_cast<int64_t>(Y) - 1, 3) == ETerrain::Obstacle)
                {
                    Marker = MarkerLowerRight;
                }
                else if (X > 0 && Type(X, Y, 0) == ETerrain::Water && Type(X, Y, 3) == ETerrain::LeftWall)
                {
                    Marker = MarkerLowerRight;
                    CX += CellDiagonal;
                    CY -= CellDiagonal;
                }
                else if (Y > 1 && Type(X, static_cast<int64_t>(Y) - 2, 3) == ETerrain::Obstacle)
                {
                    Marker = MarkerUpperRight;
                }
                else if (X > 0 && Type(static_cast<int64_t>(X) - 1, static_cast<int64_t>(Y) - 1, 3) == ETerrain::Obstacle)
                {
                    Marker = MarkerLowerLeft;
                }
                else if (X > 0 && Y > 1 && Type(static_cast<int64_t>(X) - 1, static_cast<int64_t>(Y) - 2, 3) == ETerrain::Obstacle)
                {
                    Marker = MarkerUpperLeft;
                }
            }

            if (!IsEdgeMarker(Marker))
            {
                continue;
            }

            const bool bVertical = Marker == MarkerUpperLeft || Marker == MarkerLowerRight;
            const int64_t SampleX = static_cast<int64_t>(X) - ((Marker == MarkerUpperLeft || Marker == MarkerLowerLeft) ? 1 : 0);
            const int64_t SampleY = static_cast<int64_t>(Y) - ((Marker == MarkerUpperLeft || Marker == MarkerUpperRight) ? 2 : 1);
            const FStaticCollisionShape Shape{
                .Points = {
                    FCollisionPoint{ .X = bVertical ? CX + CellDiagonal : CX, .Y = bVertical ? CY - CellDiagonal : CY },
                    FCollisionPoint{ .X = bVertical ? CX + CellDiagonal : CX + CellDiagonal * 2.f, .Y = bVertical ? CY + CellDiagonal : CY },
                },
                .PointCount = 2,
                .bWater = Type(SampleX, SampleY, 0) == ETerrain::Water,
            };
            CellShapes[X * Height + Y] = Generated.size();
            Generated.push_back({ .Shape = Shape, .Marker = Marker, .X = X, .Y = Y });
        }
    }

    const auto Find = [&](int64_t X, int64_t Y, uint32_t Marker) -> size_t
    {
        if (X < 0 || Y < 0 || static_cast<uint64_t>(X) >= Width || static_cast<uint64_t>(Y) >= Height)
        {
            return NoShape;
        }

        const size_t Index = CellShapes[static_cast<size_t>(X) * Height + static_cast<size_t>(Y)];
        return Index != NoShape && !Generated[Index].bDisabled && !Generated[Index].Shape.bWater && Generated[Index].Marker == Marker ? Index : NoShape;
    };
    // MergeTerrainRectangles at 0041aa00 visits rows and columns in reverse.
    for (auto It = Generated.rbegin(); It != Generated.rend(); ++It)
    {
        if (It->bDisabled || It->Shape.bWater || It->Shape.PointCount != 2)
        {
            continue;
        }

        const int64_t X = It->X;
        const int64_t Y = It->Y;
        size_t Other = NoShape;
        switch (It->Marker)
        {
        case MarkerUpperRight: Other = Find(X + 1, Y, MarkerUpperLeft); break;
        case MarkerLowerRight:
            Other = Find(X, Y + 1, MarkerUpperRight);
            if (Other == NoShape)
            {
                Other = Find(X + 1, Y, MarkerLowerLeft);
            }

            break;
        case MarkerLowerLeft: Other = Find(X, Y + 1, MarkerUpperLeft); break;
        case MarkerUpperLeft: Other = Find(X, Y - 1, MarkerLowerLeft); break;
        }

        if (Other == NoShape)
        {
            continue;
        }

        auto& P = It->Shape.Points;
        const FCollisionPoint A = P[0];
        const FCollisionPoint B = P[1];
        if (It->Marker == MarkerUpperRight)
        {
            P = { FCollisionPoint{ .X = A.X, .Y = A.Y - CellDiagonal * 2.f }, FCollisionPoint{ .X = B.X, .Y = A.Y - CellDiagonal * 2.f }, B, A };
        }
        else if (It->Marker == MarkerLowerLeft)
        {
            P = { A, B, FCollisionPoint{ .X = B.X, .Y = B.Y + CellDiagonal * 2.f }, FCollisionPoint{ .X = A.X, .Y = B.Y + CellDiagonal * 2.f } };
        }
        else if (It->Marker == MarkerLowerRight)
        {
            P = { A, FCollisionPoint{ .X = B.X + CellDiagonal * 2.f, .Y = A.Y }, FCollisionPoint{ .X = B.X + CellDiagonal * 2.f, .Y = B.Y }, B };
        }
        else
        {
            P = { FCollisionPoint{ .X = A.X - CellDiagonal * 2.f, .Y = A.Y }, FCollisionPoint{ .X = B.X, .Y = A.Y }, B, FCollisionPoint{ .X = A.X - CellDiagonal * 2.f, .Y = B.Y } };
        }

        It->Shape.PointCount = 4;
        Generated[Other].bDisabled = true;
    }

    FTerrainCollision Result;
    for (auto It = Generated.rbegin(); It != Generated.rend(); ++It)
    {
        if (It->bDisabled)
        {
            continue;
        }

        const size_t Index = Result.Shapes.size();
        Result.Shapes.push_back(It->Shape);
        const FBounds Box = Bounds(It->Shape);
        for (size_t Axis = 0; Axis < 2; ++Axis)
        {
            Result.Endpoints[Axis].push_back({ .Position = Axis == 0 ? Box.Max.X : Box.Max.Y, .ShapeIndex = Index, .bMaximum = true });
            Result.Endpoints[Axis].push_back({ .Position = Axis == 0 ? Box.Min.X : Box.Min.Y, .ShapeIndex = Index, .bMaximum = false });
        }
    }

    for (auto& Axis : Result.Endpoints)
    {
        // OrderedInsert at 0040e360 inserts each new endpoint before equal keys.
        std::ranges::sort(Axis, [](const auto& A, const auto& B)
        {
            if (A.Position != B.Position)
            {
                return A.Position < B.Position;
            }

            return A.ShapeIndex != B.ShapeIndex ? A.ShapeIndex > B.ShapeIndex : A.bMaximum < B.bMaximum;
        });
    }

    return Result;
}

std::optional<FCollisionPoint> IntersectTerrainShape(FCollisionCircle Previous, FCollisionCircle Candidate, const FStaticCollisionShape& Shape)
{
    if ((Shape.PointCount != 2 && Shape.PointCount != 4) || Candidate.Radius <= 0.f || Previous.Radius <= 0.f)
    {
        return std::nullopt;
    }

    std::optional<FCollisionPoint> Result;
    // The original may leave one contact coordinate uninitialized on reversed edges.
    FCollisionPoint Contact{};
    const size_t EdgeCount = Shape.PointCount == 2 ? 1 : Shape.PointCount;
    for (size_t Edge = 0; Edge < EdgeCount; ++Edge)
    {
        const auto A = Shape.Points[Edge];
        const auto B = Shape.Points[(Edge + 1) % Shape.PointCount];
        if (CircleLineContact(Candidate, A, B, Contact) || SweptCircleLineContact(Previous, Candidate, A, B, Contact))
        {
            if (!Result || !KeepPreviousContact(*Result, Contact, Candidate.Center))
            {
                Result = Contact;
            }
        }
    }

    return Result;
}

std::optional<FTerrainHit> QueryTerrainCollision(const FTerrainCollision& Terrain, FCollisionCircle Previous, FCollisionCircle Candidate)
{
    const FBounds OldBox = Bounds(Previous);
    const FBounds NewBox = Bounds(Candidate);
    const FBounds Sweep{
        .Min = { .X = std::min(OldBox.Min.X, NewBox.Min.X), .Y = std::min(OldBox.Min.Y, NewBox.Min.Y) },
        .Max = { .X = std::max(OldBox.Max.X, NewBox.Max.X), .Y = std::max(OldBox.Max.Y, NewBox.Max.Y) },
    };
    std::optional<FTerrainHit> Result;
    // MovementCollision at 004105d0 scans the X endpoint list, then the Y list.
    for (size_t Axis = 0; Axis < 2; ++Axis)
    {
        const float OldMin = Axis == 0 ? OldBox.Min.X : OldBox.Min.Y;
        const float NewMin = Axis == 0 ? NewBox.Min.X : NewBox.Min.Y;
        if (OldMin == NewMin)
        {
            continue;
        }

        const bool bForward = OldMin < NewMin;
        const float Start = bForward ? (Axis == 0 ? OldBox.Max.X : OldBox.Max.Y) : OldMin;
        const float Limit = bForward ? (Axis == 0 ? NewBox.Max.X : NewBox.Max.Y) + Candidate.Radius : NewMin - Candidate.Radius;
        const float DeltaX = Candidate.Center.X - Previous.Center.X;
        const float DeltaY = Candidate.Center.Y - Previous.Center.Y;
        const float Slope = Axis == 0 ? DeltaY / DeltaX : DeltaX / DeltaY;
        const float Intercept = Axis == 0 ? Candidate.Center.Y - Slope * Candidate.Center.X : Candidate.Center.X - Slope * Candidate.Center.Y;
        const auto Visit = [&](const FTerrainCollision::FEndpoint& Endpoint)
        {
            if ((bForward && (Endpoint.Position < Start || Limit < Endpoint.Position)) ||
                (!bForward && (Start <= Endpoint.Position || Endpoint.Position < Limit)))
            {
                return;
            }

            const auto& Shape = Terrain.Shapes[Endpoint.ShapeIndex];
            const FBounds Box = Bounds(Shape);
            if (!(Box.Min.X <= Sweep.Max.X - CollisionInset && Sweep.Min.X - CollisionInset <= Box.Max.X &&
                Box.Min.Y <= Sweep.Max.Y - CollisionInset && Sweep.Min.Y - CollisionInset <= Box.Max.Y))
            {
                return;
            }

            const float Coordinate = bForward ? Endpoint.Position - Candidate.Radius * 0.5f : Endpoint.Position + Candidate.Radius * 0.5f;
            const FCollisionPoint Crossing{ .X = Axis == 0 ? Coordinate : Coordinate * Slope + Intercept, .Y = Axis == 0 ? Coordinate * Slope + Intercept : Coordinate };
            if (!(Box.Min.X <= Crossing.X + Candidate.Radius && Crossing.X - Candidate.Radius <= Box.Max.X &&
                Box.Min.Y <= Crossing.Y + Candidate.Radius && Crossing.Y - Candidate.Radius <= Box.Max.Y))
            {
                return;
            }

            if (Result && Result->ShapeIndex == Endpoint.ShapeIndex)
            {
                return;
            }

            const auto Contact = IntersectTerrainShape(Previous, Candidate, Shape);
            if (Contact && (!Result || !KeepPreviousContact(Result->Point, *Contact, Candidate.Center)))
            {
                Result = FTerrainHit{ .ShapeIndex = Endpoint.ShapeIndex, .Point = *Contact, .bWater = Shape.bWater };
            }
        };
        const auto& Endpoints = Terrain.Endpoints[Axis];
        if (bForward)
        {
            for (const auto& Endpoint : Endpoints)
            {
                Visit(Endpoint);
            }
        }
        else
        {
            for (auto It = Endpoints.rbegin(); It != Endpoints.rend(); ++It)
            {
                Visit(*It);
            }
        }
    }

    return Result;
}

std::expected<void, std::string> VerifyTerrainCollision()
{
    const FStaticCollisionShape Line{
        .Points = { FCollisionPoint{ .X = 100.f, .Y = 0.f }, FCollisionPoint{ .X = 100.f, .Y = 200.f } },
        .PointCount = 2,
    };
    const FCollisionCircle Old{ .Center = { .X = 50.f, .Y = 100.f }, .Radius = 20.f };
    const FCollisionCircle Touching{ .Center = { .X = 80.f, .Y = 100.f }, .Radius = 20.f };
    const FCollisionCircle Crossed{ .Center = { .X = 150.f, .Y = 100.f }, .Radius = 20.f };
    const auto Contact = IntersectTerrainShape(Old, Touching, Line);
    const auto SweptContact = IntersectTerrainShape(Old, Crossed, Line);
    if (!Contact || Contact->X != 100.f || Contact->Y != 100.f || !SweptContact || SweptContact->X != 100.f || SweptContact->Y != 100.f)
    {
        return std::unexpected("Terrain circle contact self-check failed");
    }

    FLevel Level;
    Level.GridWidth = 4;
    Level.GridHeight = 5;
    Level.Cells.resize(size_t{ Level.GridWidth } * Level.GridHeight);
    Level.TileTypes = { ETerrain::None, ETerrain::Ground, ETerrain::Obstacle, ETerrain::Water };
    Level.Cells[1 * Level.GridHeight + 1][0] = 1;
    auto Terrain = BuildTerrainCollision(Level);
    if (!Terrain || Terrain->GetShapes().size() != 4)
    {
        return std::unexpected("Terrain marker self-check failed");
    }

    Level.Cells[1 * Level.GridHeight + 1][3] = 2;
    Terrain = BuildTerrainCollision(Level);
    if (!Terrain || std::ranges::none_of(Terrain->GetShapes(), [](const auto& Shape) { return Shape.PointCount == 4; }))
    {
        return std::unexpected("Terrain rectangle merge self-check failed");
    }

    Level.Cells[1 * Level.GridHeight + 1][0] = 3;
    Terrain = BuildTerrainCollision(Level);
    if (!Terrain || std::ranges::none_of(Terrain->GetShapes(), [](const auto& Shape) { return Shape.bWater; }))
    {
        return std::unexpected("Terrain water self-check failed");
    }

    Level.Cells.back()[0] = 1;
    if (!BuildTerrainCollision(Level) || BuildTerrainCollision(FLevel{}))
    {
        return std::unexpected("Terrain bounds self-check failed");
    }

    return {};
}
