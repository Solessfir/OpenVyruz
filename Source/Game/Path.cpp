#include "Game/Path.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace
{
constexpr float CornerClearance = 8.f;
constexpr uint32_t OutsideY = 1;
constexpr uint32_t OutsideX = 2;
constexpr uint32_t Above = 4;
constexpr uint32_t Below = 8;
constexpr uint32_t Left = 16;
constexpr uint32_t Right = 32;
constexpr uint32_t HorizontalSides = Left | Right;
constexpr uint32_t VerticalSides = Above | Below;
constexpr uint32_t Outside = OutsideX | OutsideY;
constexpr uint32_t Sides = HorizontalSides | VerticalSides;
constexpr size_t CornerCount = 4;

bool SamePoint(FCollisionPoint A, FCollisionPoint B)
{
    return A.X == B.X && A.Y == B.Y;
}

bool IsFinite(FCollisionPoint Point)
{
    return std::isfinite(Point.X) && std::isfinite(Point.Y);
}

double DistanceSquared(FCollisionPoint A, FCollisionPoint B)
{
    const double X = double(A.X) - B.X;
    const double Y = double(A.Y) - B.Y;
    return X * X + Y * Y;
}

bool Blocks(const FPathObstacle& Obstacle, bool bIgnoreWater)
{
    return !Obstacle.bDisabled && !Obstacle.bActor && !(Obstacle.bWater && bIgnoreWater);
}

bool Overlaps(const FPathObstacle& Obstacle, FCollisionPoint Min, FCollisionPoint Max)
{
    return Obstacle.Min.X <= Max.X && Min.X <= Obstacle.Max.X && Obstacle.Min.Y <= Max.Y && Min.Y <= Obstacle.Max.Y;
}

struct FEndpoint
{
    float Position;
    size_t Obstacle;
    bool bMaximum;
};

class FPathQuery
{
public:
    explicit FPathQuery(std::span<const FPathObstacle> InObstacles, bool bInIgnoreWater)
        : Obstacles(InObstacles), bIgnoreWater(bInIgnoreWater)
    {
        for (size_t Index = 0; Index < Obstacles.size(); ++Index)
        {
            const auto& Obstacle = Obstacles[Index];
            for (size_t Axis = 0; Axis < Endpoints.size(); ++Axis)
            {
                Endpoints[Axis].push_back({ .Position = Axis == 0 ? Obstacle.Max.X : Obstacle.Max.Y, .Obstacle = Index, .bMaximum = true });
                Endpoints[Axis].push_back({ .Position = Axis == 0 ? Obstacle.Min.X : Obstacle.Min.Y, .Obstacle = Index, .bMaximum = false });
            }
        }

        for (auto& Axis : Endpoints)
        {
            // OrderedInsert at 0040e360 places newly inserted endpoints before equal keys.
            std::ranges::sort(Axis, [](const FEndpoint& A, const FEndpoint& B)
            {
                if (A.Position != B.Position)
                {
                    return A.Position < B.Position;
                }

                return A.Obstacle != B.Obstacle ? A.Obstacle > B.Obstacle : A.bMaximum < B.bMaximum;
            });
        }
    }

    std::optional<size_t> FindContaining(FCollisionPoint Point, FCollisionPoint HalfExtent) const
    {
        const FCollisionPoint Min{ .X = Point.X - HalfExtent.X, .Y = Point.Y - HalfExtent.Y };
        const FCollisionPoint Max{ .X = Point.X + HalfExtent.X, .Y = Point.Y + HalfExtent.Y };
        // PathPointObstacle at 0040f7b0 searches backwards on X before searching forwards.
        for (auto It = Endpoints[0].rbegin(); It != Endpoints[0].rend(); ++It)
        {
            if (It->Position < Point.X && Min.X <= It->Position && Blocks(Obstacles[It->Obstacle], bIgnoreWater) && Overlaps(Obstacles[It->Obstacle], Min, Max))
            {
                return It->Obstacle;
            }
        }

        for (const FEndpoint& Endpoint : Endpoints[0])
        {
            if (Point.X <= Endpoint.Position && Endpoint.Position <= Max.X && Blocks(Obstacles[Endpoint.Obstacle], bIgnoreWater) && Overlaps(Obstacles[Endpoint.Obstacle], Min, Max))
            {
                return Endpoint.Obstacle;
            }
        }

        return std::nullopt;
    }

    std::optional<size_t> FindCrossing(FCollisionPoint From, FCollisionPoint To, FCollisionPoint HalfExtent) const
    {
        const auto X = Scan(From, To, HalfExtent, false);
        const auto Y = Scan(From, To, HalfExtent, true);
        if (!X || (Y && Y->Obstacle != X->Obstacle && DistanceSquared(Y->Point, From) <= DistanceSquared(X->Point, From)))
        {
            return Y ? std::optional{ Y->Obstacle } : std::nullopt;
        }

        return X->Obstacle;
    }

private:
    struct FHit
    {
        size_t Obstacle = 0;
        FCollisionPoint Point;
    };

    std::optional<FHit> Scan(FCollisionPoint From, FCollisionPoint To, FCollisionPoint HalfExtent, bool bPrimaryY) const
    {
        if (SamePoint(From, To))
        {
            return std::nullopt;
        }

        const bool bAxisY = bPrimaryY ? From.Y != To.Y : From.X == To.X;
        const float Start = bAxisY ? From.Y : From.X;
        const float Finish = bAxisY ? To.Y : To.X;
        const bool bForward = Start < Finish;
        const float Padding = bAxisY || (!bForward && !bPrimaryY) ? HalfExtent.Y : HalfExtent.X;
        const float Limit = bForward ? Finish + Padding : Finish - Padding;
        const bool bConstant = bAxisY ? From.X == To.X : From.Y == To.Y;
        const float Slope = bConstant ? 0.f : float(bAxisY ? (double(To.X) - From.X) / (double(To.Y) - From.Y) : (double(To.Y) - From.Y) / (double(To.X) - From.X));
        const float Intercept = float(bAxisY ? double(To.X) - double(Slope) * To.Y : double(To.Y) - double(Slope) * To.X);
        const FCollisionPoint Min{ .X = std::min(From.X, To.X), .Y = std::min(From.Y, To.Y) };
        const FCollisionPoint Max{ .X = std::max(From.X, To.X), .Y = std::max(From.Y, To.Y) };
        const auto Visit = [&](const FEndpoint& Endpoint) -> std::optional<FHit>
        {
            if ((bForward && (Endpoint.Position < Start || Limit < Endpoint.Position)) ||
                (!bForward && (Start <= Endpoint.Position || Endpoint.Position < Limit)) || !Blocks(Obstacles[Endpoint.Obstacle], bIgnoreWater))
            {
                return std::nullopt;
            }

            const FCollisionPoint Point{
                .X = bAxisY ? (bConstant ? To.X : float(double(Endpoint.Position) * Slope + Intercept)) : Endpoint.Position,
                .Y = bAxisY ? Endpoint.Position : (bConstant ? To.Y : float(double(Endpoint.Position) * Slope + Intercept)),
            };
            if (Min.X <= Point.X && Point.X <= Max.X && Min.Y <= Point.Y && Point.Y <= Max.Y && Overlaps(Obstacles[Endpoint.Obstacle], Point, Point))
            {
                return FHit{ .Obstacle = Endpoint.Obstacle, .Point = Point };
            }

            return std::nullopt;
        };
        const auto& Axis = Endpoints[bAxisY ? 1 : 0];
        if (bForward)
        {
            for (const FEndpoint& Endpoint : Axis)
            {
                if (const auto Hit = Visit(Endpoint))
                {
                    return Hit;
                }
            }
        }
        else
        {
            for (auto It = Axis.rbegin(); It != Axis.rend(); ++It)
            {
                if (const auto Hit = Visit(*It))
                {
                    return Hit;
                }
            }
        }

        return std::nullopt;
    }

    std::span<const FPathObstacle> Obstacles;
    bool bIgnoreWater;
    std::array<std::vector<FEndpoint>, 2> Endpoints;
};

uint32_t CornerMask(uint32_t SideMask)
{
    return ((SideMask & (Left | Above)) == (Left | Above) ? 1u : 0u) |
        ((SideMask & (Right | Above)) == (Right | Above) ? 2u : 0u) |
        ((SideMask & (Left | Below)) == (Left | Below) ? 4u : 0u) |
        ((SideMask & (Right | Below)) == (Right | Below) ? 8u : 0u);
}

uint32_t ClassifyPoint(FCollisionPoint Point, const FPathObstacle& Obstacle, FCollisionPoint ExpandedMin, FCollisionPoint ExpandedMax)
{
    uint32_t Result = 0;
    if (Point.X < ExpandedMin.X || ExpandedMax.X < Point.X)
    {
        Result |= OutsideX;
    }

    if (Point.Y < ExpandedMin.Y || ExpandedMax.Y < Point.Y)
    {
        Result |= OutsideY;
    }

    Result |= Point.X < Obstacle.Min.X ? Left : (Obstacle.Max.X < Point.X ? Right : 0);
    Result |= Point.Y < Obstacle.Min.Y ? Above : (Obstacle.Max.Y < Point.Y ? Below : 0);
    return Result;
}

// PathObstacleCorners at 0040f310 selects only the corners relevant to these two waypoints.
uint32_t SelectCorners(FCollisionPoint From, FCollisionPoint To, const FPathObstacle& Obstacle, FCollisionPoint HalfExtent)
{
    const FCollisionPoint Min{ .X = float(double(Obstacle.Min.X) - (double(HalfExtent.X) + CornerClearance)), .Y = float(double(Obstacle.Min.Y) - (double(HalfExtent.Y) + CornerClearance)) };
    const FCollisionPoint Max{ .X = float(double(HalfExtent.X) + CornerClearance + Obstacle.Max.X), .Y = float(double(HalfExtent.Y) + CornerClearance + Obstacle.Max.Y) };
    uint32_t NearSides = 0;
    if (Min.X < From.X && From.X <= Obstacle.Min.X)
    {
        NearSides |= Left;
    }
    else if (Obstacle.Max.X <= From.X && From.X < Max.X)
    {
        NearSides |= Right;
    }

    if (Min.Y < From.Y && From.Y <= Obstacle.Min.Y)
    {
        NearSides |= Above;
    }
    else if (Obstacle.Max.Y <= From.Y && From.Y < Max.Y)
    {
        NearSides |= Below;
    }

    if ((NearSides & HorizontalSides) != 0 && (NearSides & VerticalSides) != 0)
    {
        return CornerMask(NearSides);
    }

    const uint32_t Start = ClassifyPoint(From, Obstacle, Min, Max);
    const uint32_t Finish = ClassifyPoint(To, Obstacle, Min, Max);
    if ((Start & Outside) != 0)
    {
        uint32_t Mask = Start & Sides;
        if ((Mask & HorizontalSides) == 0)
        {
            Mask |= HorizontalSides;
        }

        if ((Mask & VerticalSides) == 0)
        {
            Mask |= VerticalSides;
        }

        return CornerMask(Mask);
    }

    if ((Finish & Outside) == 0)
    {
        return 0;
    }

    if ((Finish & ~Outside) == Start)
    {
        return CornerMask(Finish);
    }

    if (Start == Above || Start == Below)
    {
        const uint32_t Horizontal = Finish & HorizontalSides;
        return CornerMask(Start | (Horizontal != 0 ? Horizontal : HorizontalSides));
    }

    if (Start == Left || Start == Right)
    {
        const uint32_t Vertical = Finish & VerticalSides;
        return CornerMask(Start | (Vertical != 0 ? Vertical : VerticalSides));
    }

    uint32_t Result = 0;
    if ((Start & Left) != 0 && (Finish & Right) != 0)
    {
        Result |= CornerMask((Start & VerticalSides) | Right);
    }
    else if ((Start & Right) != 0 && (Finish & Left) != 0)
    {
        Result |= CornerMask((Start & VerticalSides) | Left);
    }

    if ((Start & Above) != 0 && (Finish & Below) != 0)
    {
        Result |= CornerMask((Start & HorizontalSides) | Below);
    }
    else if ((Start & Below) != 0 && (Finish & Above) != 0)
    {
        Result |= CornerMask((Start & HorizontalSides) | Above);
    }

    return Result;
}

bool ValidQuery(std::span<const FPathObstacle> Obstacles, FCollisionPoint HalfExtent)
{
    return IsFinite(HalfExtent) && HalfExtent.X >= 0.f && HalfExtent.Y >= 0.f && std::ranges::all_of(Obstacles, [](const FPathObstacle& Obstacle)
    {
        return IsFinite(Obstacle.Min) && IsFinite(Obstacle.Max) && Obstacle.Min.X <= Obstacle.Max.X && Obstacle.Min.Y <= Obstacle.Max.Y;
    });
}

void SimplifyRoute(FPathRoute& Route, const FPathQuery& Query, FCollisionPoint HalfExtent)
{
    // PathSimplify at 00411120 retains the furthest visible later waypoint at each step.
    for (size_t Start = 0; Start + 1 < Route.Points.size(); ++Start)
    {
        size_t LastClear = Start + 1;
        for (size_t End = Start + 2; End < Route.Points.size(); ++End)
        {
            if (!Query.FindCrossing(Route.Points[Start], Route.Points[End], HalfExtent))
            {
                LastClear = End;
            }
        }

        if (LastClear > Start + 1)
        {
            Route.Points.erase(Route.Points.begin() + static_cast<ptrdiff_t>(Start + 1), Route.Points.begin() + static_cast<ptrdiff_t>(LastClear));
        }
    }

    Route.Current = 0;
}

float RouteScore(const FPathRoute& Route)
{
    // SeekerRouteScore at 0042d6c0 keeps the first point as its reference throughout the loop.
    double Score = 0.;
    for (size_t Index = 1; Index < Route.Points.size(); ++Index)
    {
        const double X = double(Route.Points[Index].X) - Route.Points.front().X;
        const double Y = double(Route.Points[Index].Y) - Route.Points.front().Y;
        Score = X * X + Y * Y + Score;
    }

    return float(Score);
}
}

bool FPathSearch::Start(FCollisionPoint From, FCollisionPoint To)
{
    // PathStartSearch at 0040f6f0 begins with a single unchecked segment.
    Routes.clear();
    GeneratedPoints.clear();
    if (!IsFinite(From) || !IsFinite(To))
    {
        return false;
    }

    GeneratedPoints = { To, From };
    Routes.push_back({ .Points = { From, To }, .Current = 0 });
    return true;
}

EPathStatus FPathSearch::Advance(std::span<const FPathObstacle> Obstacles, FCollisionPoint HalfExtent, float MaxDistance, bool bIgnoreWater)
{
    // PathAdvanceSearch at 00410460 processes the existing route list once, deferring appended branches.
    if (!ValidQuery(Obstacles, HalfExtent) || !std::isfinite(MaxDistance))
    {
        Routes.clear();
        return EPathStatus::Failed;
    }

    const FPathQuery Query(Obstacles, bIgnoreWater);
    std::vector<FPathRoute> Pending;
    std::vector<FPathRoute> Next;
    bool bProcessed = false;
    bool bFound = false;
    for (auto& Route : Routes)
    {
        if (Route.Current + 1 == Route.Points.size())
        {
            bFound = true;
            Next.push_back(std::move(Route));
            continue;
        }

        bProcessed = true;
        const FCollisionPoint From = Route.Points[Route.Current];
        const FCollisionPoint To = Route.Points[Route.Current + 1];
        if (SamePoint(From, To))
        {
            continue;
        }

        const auto Containing = Route.Current != 0 ? Query.FindContaining(From, HalfExtent) : std::nullopt;
        const auto Blocking = Containing ? Containing : Query.FindCrossing(From, To, HalfExtent);
        if (!Blocking)
        {
            ++Route.Current;
            bFound |= Route.Current + 1 == Route.Points.size();
            Next.push_back(std::move(Route));
            continue;
        }

        const auto& Obstacle = Obstacles[*Blocking];
        const uint32_t Mask = SelectCorners(From, To, Obstacle, HalfExtent);
        const std::array<FCollisionPoint, CornerCount> Corners{
            FCollisionPoint{ .X = float((double(Obstacle.Min.X) - HalfExtent.X) - CornerClearance), .Y = float((double(Obstacle.Min.Y) - HalfExtent.Y) - CornerClearance) },
            FCollisionPoint{ .X = float(double(Obstacle.Max.X) + HalfExtent.X + CornerClearance), .Y = float((double(Obstacle.Min.Y) - HalfExtent.Y) - CornerClearance) },
            FCollisionPoint{ .X = float((double(Obstacle.Min.X) - HalfExtent.X) - CornerClearance), .Y = float(double(Obstacle.Max.Y) + HalfExtent.Y + CornerClearance) },
            FCollisionPoint{ .X = float(double(Obstacle.Max.X) + HalfExtent.X + CornerClearance), .Y = float(double(Obstacle.Max.Y) + HalfExtent.Y + CornerClearance) },
        };
        // PathBranch at 00410250 inserts corners in reverse order and shares duplicate suppression across routes.
        for (size_t Reverse = CornerCount; Reverse > 0; --Reverse)
        {
            const size_t CornerIndex = Reverse - 1;
            const FCollisionPoint Corner = Corners[CornerIndex];
            if ((Mask & (1u << CornerIndex)) == 0 || !IsFinite(Corner) ||
                (MaxDistance > 0.f && DistanceSquared(Corner, Route.Points.front()) > double(MaxDistance) * MaxDistance) ||
                std::ranges::any_of(std::span(GeneratedPoints).subspan(1), [&](FCollisionPoint Point) { return SamePoint(Point, Corner); }))
            {
                continue;
            }

            GeneratedPoints.push_back(Corner);
            FPathRoute Branch = Route;
            if (Containing && Branch.Current + 2 < Branch.Points.size())
            {
                Branch.Points.erase(Branch.Points.begin() + static_cast<ptrdiff_t>(Branch.Current + 1));
            }

            Branch.Points.insert(Branch.Points.begin() + static_cast<ptrdiff_t>(Branch.Current + 1), Corner);
            Pending.push_back(std::move(Branch));
            if (Route.Current != 0 && Route.Current + 2 < Route.Points.size())
            {
                FPathRoute Prefix = Route;
                Prefix.Points.resize(Route.Current + 1);
                Prefix.Points.push_back(Corner);
                Prefix.Points.push_back(Route.Points.back());
                Pending.push_back(std::move(Prefix));
            }
        }
    }

    for (auto& Route : Pending)
    {
        Next.push_back(std::move(Route));
    }

    Routes = std::move(Next);
    if (bProcessed)
    {
        return bFound ? EPathStatus::Found : EPathStatus::Searching;
    }

    return Routes.empty() ? EPathStatus::Failed : EPathStatus::Finished;
}

std::optional<FPathRoute> FPathSearch::SelectFinishedRoute(std::span<const FPathObstacle> Obstacles, FCollisionPoint HalfExtent, bool bIgnoreWater) const
{
    if (!ValidQuery(Obstacles, HalfExtent))
    {
        return std::nullopt;
    }

    const FPathQuery Query(Obstacles, bIgnoreWater);
    std::optional<FPathRoute> Result;
    float BestScore = 0.f;
    // SeekerChooseRoute at 0042d520 simplifies finished routes before comparing them; ties keep the earlier route.
    for (const auto& Route : Routes)
    {
        if (Route.Current + 1 != Route.Points.size())
        {
            continue;
        }

        FPathRoute Candidate = Route;
        SimplifyRoute(Candidate, Query, HalfExtent);
        const float Score = RouteScore(Candidate);
        if (!Result || Score < BestScore)
        {
            Result = std::move(Candidate);
            BestScore = Score;
        }
    }

    return Result;
}

std::expected<void, std::string> VerifyPathSearch()
{
    FPathSearch Search;
    const FCollisionPoint From{ .X = 0.f, .Y = 0.f };
    const FCollisionPoint To{ .X = 100.f, .Y = 0.f };
    const FCollisionPoint HalfExtent{ .X = 2.f, .Y = 2.f };
    const std::array TiedCrossings{
        FPathObstacle{ .Min = { .X = 40.f, .Y = 0.f }, .Max = { .X = 40.f, .Y = 100.f } },
        FPathObstacle{ .Min = { .X = 0.f, .Y = 40.f }, .Max = { .X = 100.f, .Y = 40.f } },
    };
    if (FPathQuery(TiedCrossings, false).FindCrossing(From, { .X = 100.f, .Y = 100.f }, HalfExtent) != 1)
    {
        return std::unexpected("Path Y-contact tie self-check failed");
    }

    if (!Search.Start(From, To) || Search.Advance({}, HalfExtent) != EPathStatus::Found || Search.Advance({}, HalfExtent) != EPathStatus::Finished)
    {
        return std::unexpected("Path direct-route status self-check failed");
    }

    const std::array Obstacles{ FPathObstacle{ .Min = { .X = 40.f, .Y = -10.f }, .Max = { .X = 60.f, .Y = 10.f } } };
    const std::array EqualEndpoints{ Obstacles[0], Obstacles[0] };
    const FPathQuery EqualQuery(EqualEndpoints, false);
    if (EqualQuery.FindCrossing(From, To, HalfExtent) != 1 || EqualQuery.FindCrossing(To, From, HalfExtent) != 0 ||
        !EqualQuery.FindCrossing({ .X = 40.f, .Y = 0.f }, To, HalfExtent))
    {
        return std::unexpected("Path equal-endpoint ordering self-check failed");
    }

    Search.Start(From, To);
    if (Search.Advance(Obstacles, HalfExtent) != EPathStatus::Searching || Search.GetRoutes().size() != 2 ||
        !SamePoint(Search.GetRoutes()[0].Points[1], { .X = 30.f, .Y = 20.f }) ||
        !SamePoint(Search.GetRoutes()[1].Points[1], { .X = 30.f, .Y = -20.f }) || Search.SelectFinishedRoute(Obstacles, HalfExtent))
    {
        return std::unexpected("Path ordered-corner branching self-check failed");
    }

    if (Search.Advance(Obstacles, HalfExtent) != EPathStatus::Searching || Search.Advance(Obstacles, HalfExtent) != EPathStatus::Found)
    {
        return std::unexpected("Path incremental-route self-check failed");
    }

    const auto Selected = Search.SelectFinishedRoute(Obstacles, HalfExtent);
    if (!Selected || Selected->Points.size() != 3 || !SamePoint(Selected->Points[1], { .X = 30.f, .Y = 20.f }) || Selected->Current != 0)
    {
        return std::unexpected("Path stable-route selection self-check failed");
    }

    const auto Simplified = Search.SelectFinishedRoute({}, HalfExtent);
    if (!Simplified || Simplified->Points.size() != 2)
    {
        return std::unexpected("Path route-simplification self-check failed");
    }

    Search.Start(From, To);
    if (Search.Advance(Obstacles, HalfExtent, 1.f) != EPathStatus::Searching || Search.Advance(Obstacles, HalfExtent) != EPathStatus::Failed)
    {
        return std::unexpected("Path branch-distance self-check failed");
    }

    auto Water = Obstacles;
    Water[0].bWater = true;
    Search.Start(From, To);
    if (Search.Advance(Water, HalfExtent, 0.f, true) != EPathStatus::Found)
    {
        return std::unexpected("Path water exclusion self-check failed");
    }

    auto Actor = Obstacles;
    Actor[0].bActor = true;
    Search.Start(From, To);
    if (Search.Advance(Actor, HalfExtent) != EPathStatus::Found)
    {
        return std::unexpected("Path actor exclusion self-check failed");
    }

    auto Disabled = Obstacles;
    Disabled[0].bDisabled = true;
    Search.Start(From, To);
    if (Search.Advance(Disabled, HalfExtent) != EPathStatus::Found)
    {
        return std::unexpected("Path disabled-object exclusion self-check failed");
    }

    const FPathRoute Scored{ .Points = { From, { .X = 10.f, .Y = 0.f }, { .X = 20.f, .Y = 0.f } }, .Current = 2 };
    if (RouteScore(Scored) != 500.f)
    {
        return std::unexpected("Path origin-distance score self-check failed");
    }

    Search.Start(From, From);
    if (Search.Advance({}, HalfExtent) != EPathStatus::Searching || Search.Advance({}, HalfExtent) != EPathStatus::Failed)
    {
        return std::unexpected("Path zero-segment self-check failed");
    }

    return {};
}
