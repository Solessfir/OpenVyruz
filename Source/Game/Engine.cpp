#include "Game/Engine.h"

#include "Game/Level.h"
#include "Game/Loop.h"
#include "Pg/Binary.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>
#include <utility>

namespace
{
constexpr int DirectionCount = ObjectDirectionCount;
constexpr int NoDirection = DirectionCount;
// Direction vectors are indexed three steps ahead of the facing direction, and strafing uses the facing six steps back (90 degrees).
constexpr int DirectionVectorOffset = 3;
constexpr int StrafeDirectionOffset = DirectionCount - 6;
// DirectionDistance at 00404330 treats directions this far apart or more as facing away.
constexpr int FacingAwayDistance = 7;
constexpr int PosesPerDirection = PlayerPoseCount / DirectionCount;
// PatrolMoveUpdate gives its work attribute a 64-unit arrival radius.
constexpr uint32_t PatrolArrivalRadius = 64;
constexpr uint32_t SpiderArrivalRadius = 4;
constexpr uint32_t WalkerArrivalRadius = 5;

// Game speeds are tuned for 14 ms steps; EngineRunLevel at 0043e5f0 stores 14 / step as a float.
constexpr double ReferenceStepMilliseconds = 14.;
constexpr float BackwardDecelerationScale = 3.f;
constexpr float SpeedLimitScale = 0.1f;
constexpr float PushAwaySpeed = -2.f;
constexpr float ImpactProbeSpeed = 0.1f;

// The x87 constants FindDirection at 004075c0 uses: pi/2, 3*pi/2, half a direction sector (pi/24) and sectors per radian (12/pi).
constexpr float HalfPi = 1.5707964f;
constexpr float ThreeHalfPi = 4.712389f;
constexpr float HalfSector = 0.1308997f;
constexpr float SectorsPerRadian = 3.8197186f;
constexpr float Pi = 3.1415927f;

// Exact bits of 0.1 * sin(i * 15 degrees), i = 0..6, from the direction table at 0050d508.
constexpr std::array<uint32_t, 7> TenthSineBits = { 0x00000000, 0x3CD40647, 0x3D4CCCCD, 0x3D90D0C5, 0x3DB15CA7, 0x3DC5D257, 0x3DCCCCCD };

constexpr float TenthSine(const int Steps)
{
    // Steps of 15 degrees; the table is symmetric, so fold every angle into 0..90 degrees.
    const int Wrapped = ((Steps % DirectionCount) + DirectionCount) % DirectionCount;
    const int QuarterTurn = DirectionCount / 4;
    const int Folded = Wrapped <= QuarterTurn ? Wrapped : Wrapped <= QuarterTurn * 2 ? QuarterTurn * 2 - Wrapped : Wrapped <= QuarterTurn * 3 ? Wrapped - QuarterTurn * 2 : DirectionCount - Wrapped;
    const float Magnitude = std::bit_cast<float>(TenthSineBits[size_t(Folded)]);
    return Wrapped > QuarterTurn * 2 ? -Magnitude : Magnitude;
}

// Entry k is 0.1 * (sin, cos) of (k - 3) * 15 degrees in collision space.
constexpr FCollisionPoint DirectionVector(const int Index)
{
    return { .X = TenthSine(Index - DirectionVectorOffset), .Y = TenthSine(Index - DirectionVectorOffset + DirectionCount / 4) };
}
static_assert(std::bit_cast<uint32_t>(DirectionVector(0).X) == 0xBD90D0C5 && std::bit_cast<uint32_t>(DirectionVector(0).Y) == 0x3D90D0C5);
static_assert(std::bit_cast<uint32_t>(DirectionVector(13).X) == 0x3D4CCCCD && std::bit_cast<uint32_t>(DirectionVector(13).Y) == 0xBDB15CA7);
static_assert(std::bit_cast<uint32_t>(DirectionVector(22).X) == 0xBDC5D257 && std::bit_cast<uint32_t>(DirectionVector(22).Y) == 0x3CD40647);

constexpr int Wrap(const int Direction)
{
    return ((Direction % DirectionCount) + DirectionCount) % DirectionCount;
}

constexpr FCollisionPoint FacingVector(const int Direction, const int Offset = 0)
{
    return DirectionVector(Wrap(Direction + Offset + DirectionVectorOffset));
}

// FindDirection at 004075c0; NoDirection for a zero or degenerate vector.
int FindDirection(const FCollisionPoint Vector)
{
    double Angle = std::atan(double(Vector.Y) / double(Vector.X));
    if (std::isnan(Angle))
    {
        return NoDirection;
    }

    Angle -= Vector.X < 0.f ? double(ThreeHalfPi) : double(HalfPi);
    if (Angle > 0.)
    {
        return NoDirection;
    }

    int Sector = int((Angle - double(HalfSector)) * double(SectorsPerRadian));
    if (Sector == -DirectionCount)
    {
        Sector = 0;
    }

    const int Direction = -Sector;
    return Direction < 0 || Direction >= DirectionCount ? NoDirection : Direction;
}

// DirectionDistance at 00404330: the shorter way around, zero when either direction is unknown.
constexpr int DirectionDistance(int A, int B)
{
    if (A == B || A == NoDirection || B == NoDirection)
    {
        return 0;
    }

    if (B < A)
    {
        std::swap(A, B);
    }

    return B - A < DirectionCount / 2 ? B - A : A - B + DirectionCount;
}
static_assert(DirectionDistance(1, 23) == 2 && DirectionDistance(3, 15) == 12 && DirectionDistance(5, NoDirection) == 0);

// The x87 code keeps intermediates in double precision and rounds only on stores to float fields.
constexpr FCollisionPoint WorldFromCollision(const FCollisionPoint Collision)
{
    return {
        .X = float((double(Collision.X) - double(Collision.Y)) * double(0.8944272f)),
        .Y = float((double(Collision.X) + double(Collision.Y)) * double(0.4472136f)),
    };
}

// Exact bits of the world-space table at 0050d440 that WeaponFire (00443bd0) offsets the muzzle with: x, y per direction. It is the world
// image of each facing vector, but rounded differently from what WorldFromCollision produces.
constexpr std::array<uint32_t, ObjectDirectionCount * 2> WorldFacingBits = {
    0xBDB72DBC, 0x3D372DC9, 0xBD8186E9, 0x3D6058EB, 0xBD061898, 0x3D7A3A08, 0x00000000, 0x3D8186E9,
    0x3D061898, 0x3D7A3A08, 0x3D8186E9, 0x3D6058EB, 0x3DB72DBC, 0x3D372DC9, 0x3DE05921, 0x3D0186E9,
    0x3DFA3A08, 0x3C8618B3, 0x3E0186DB, 0x00000000, 0x3DFA3A08, 0xBC8618B3, 0x3DE05921, 0xBD0186E9,
    0x3DB72DBC, 0xBD372DC9, 0x3D8186E9, 0xBD6058EB, 0x3D061898, 0xBD7A3A08, 0x00000000, 0xBD8186E9,
    0xBD061898, 0xBD7A3A08, 0xBD8186E9, 0xBD6058EB, 0xBDB72DBC, 0xBD372DC9, 0xBDE05921, 0xBD0186E9,
    0xBDFA3A08, 0xBC8618B3, 0xBE0186DB, 0x00000000, 0xBDFA3A08, 0x3C8618B3, 0xBDE05921, 0x3D0186E9,
};

constexpr FCollisionPoint WorldFacingVector(const int Direction)
{
    const size_t Index = size_t(Wrap(Direction)) * 2;
    return { .X = std::bit_cast<float>(WorldFacingBits[Index]), .Y = std::bit_cast<float>(WorldFacingBits[Index + 1]) };
}

// AnimUpdateBounds at 0041b4b0 recomputes the collision centre from the world position.
FCollisionPoint CollisionFromWorld(const FCollisionPoint World)
{
    constexpr double InverseWidth = 1. / BlockWidth;
    constexpr double InverseHeight = 1. / BlockHeight;
    constexpr double CellDiagonal = 35.77709f;
    return {
        .X = float((double(World.X) * InverseWidth + double(World.Y) * InverseHeight) * CellDiagonal),
        .Y = float((double(World.Y) * InverseHeight - double(World.X) * InverseWidth) * CellDiagonal),
    };
}

bool IsZero(const FCollisionPoint Point)
{
    return Point.X == 0.f && Point.Y == 0.f;
}

// PlayerAdvancePose at 00438390: one pose per call, the short way round; vehicles have 24 poses, the player and cops 48.
int AdvancePose(const int Pose, const int Target, const int PoseCount = PlayerPoseCount)
{
    const int Half = PoseCount / 2;
    if (Pose < Target)
    {
        if (Target - Pose < Half)
        {
            return Pose + 1;
        }

        return Pose == 1 ? PoseCount : Pose - 1;
    }

    if (Pose > Target)
    {
        if (Pose - Target >= Half)
        {
            return Pose == PoseCount ? 1 : Pose + 1;
        }

        return Pose - 1;
    }

    return Pose;
}

// PlayerTurn at 00443990: a new direction is only taken once the body pose has caught up with the previous one.
void TurnPlayer(FPlayerState& Player, const FMovementClass& Class, const EPlayerTurn Turn, const uint32_t StepMilliseconds)
{
    // Objects without a turn timeout never turn; refilling the timer by zero would never end.
    if (Class.TurnTimeout <= 0)
    {
        return;
    }

    Player.TurnTimer -= int(StepMilliseconds);
    if (Player.TurnTimer >= 1)
    {
        return;
    }

    do
    {
        Player.TurnTimer += Class.TurnTimeout;
        if (Player.Pose != Player.TargetPose)
        {
            Player.Pose = AdvancePose(Player.Pose, Player.TargetPose);
            if (Player.Pose == Player.TargetPose && Turn == EPlayerTurn::None)
            {
                return;
            }

            if (Player.TurnTimer >= 0)
            {
                return;
            }
        }

        if (Turn != EPlayerTurn::None && Player.Pose == Player.TargetPose)
        {
            Player.Direction = Wrap(Player.Direction + (Turn == EPlayerTurn::Left ? 1 : -1));
            Player.TargetPose = Player.Direction * PosesPerDirection + 1;
        }
    } while (Player.TurnTimer < 0);
}

void DecaySpeed(FPlayerState& Player, const FMovementClass& Class)
{
    if (Player.Speed > Class.DecelerationStep)
    {
        Player.Speed = float(double(Player.Speed) - double(Class.DecelerationStep));
    }
    else if (-Class.DecelerationStep <= Player.Speed)
    {
        Player.Speed = 0.f;
    }
    else
    {
        Player.Speed = float(double(Class.DecelerationStep) + double(Player.Speed));
    }
}

// ObjectComputeDisplacement at 00438b20 for the player: collision-space displacement for this step.
FCollisionPoint Displacement(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, const float TimeScale)
{
    const float Strafe = Command.Strafe >= 0.f ? std::min(Command.Strafe, Class.ThrustMax) : std::max(Command.Strafe, Class.ThrustMin);
    double X = 0.;
    double Y = 0.;
    float PreviousMagnitude = 0.f;
    if (Player.bPushed)
    {
        X = Player.Velocity.X;
        Y = Player.Velocity.Y;
        PreviousMagnitude = Player.SpeedMagnitude;
    }

    if (Class.bUseInertia)
    {
        X = Player.Velocity.X;
        Y = Player.Velocity.Y;
        PreviousMagnitude = Player.SpeedMagnitude;
        if (Class.Grip != 1.f)
        {
            X = float(double(Class.Grip) * X);
            Y = float(double(Class.Grip) * Y);
        }
    }

    bool bPush = true;
    if (Command.Thrust == EPlayerThrust::Forward)
    {
        if (Player.Speed < Class.ThrustMax)
        {
            Player.Speed = float(double(Class.AccelerationStep) + double(Player.Speed));
        }

        if (Class.ThrustMax < Player.Speed)
        {
            Player.Speed = Class.ThrustMax;
        }
    }
    else if (Command.Thrust == EPlayerThrust::Backward)
    {
        if (Class.ThrustMin < Player.Speed)
        {
            Player.Speed = float(double(Player.Speed) - double(Class.DecelerationStep) * BackwardDecelerationScale);
        }

        if (Player.Speed < Class.ThrustMin)
        {
            Player.Speed = Class.ThrustMin;
        }
    }
    else
    {
        DecaySpeed(Player, Class);
        bPush = false;
    }

    // Thrust only pushes while the body is slower than a tenth of Speed-max.
    const float PushLimit = Class.SpeedMax == 0.f ? 0.f : float(double(Class.SpeedMax) * double(SpeedLimitScale));
    if (bPush && PreviousMagnitude < PushLimit && Player.Speed != 0.f)
    {
        const FCollisionPoint Facing = FacingVector(Player.Direction);
        const double Weight = Class.bThrustIgnoresWeight ? 1. : double(Class.Weight);
        X = float(double(Player.Speed) * double(Facing.X) / Weight + X);
        Y = float(double(Player.Speed) * double(Facing.Y) / Weight + Y);
    }

    if (Strafe != 0.f)
    {
        const FCollisionPoint Side = FacingVector(Player.Direction, StrafeDirectionOffset);
        X = float(double(Strafe) * double(Side.X) + X);
        Y = float(double(Strafe) * double(Side.Y) + Y);
    }

    // Patrol and random movers add the pull back onto their route segment before friction.
    X = float(double(Command.Correction.X) + X);
    Y = float(double(Command.Correction.Y) + Y);

    X = float(double(Class.Friction.X) * X);
    Y = float(double(Class.Friction.Y) * Y);
    const double ThresholdX = TimeScale == 1.f ? Class.FrictionThreshold.X : float(double(Class.FrictionThreshold.X) / double(TimeScale));
    const double ThresholdY = TimeScale == 1.f ? Class.FrictionThreshold.Y : float(double(Class.FrictionThreshold.Y) / double(TimeScale));
    const bool bMovingX = X != 0. && std::abs(X) > ThresholdX;
    const bool bMovingY = Y != 0. && ThresholdY < std::abs(Y);
    float Magnitude = 0.f;
    if (bMovingX || bMovingY)
    {
        Magnitude = float(std::sqrt(Y * Y + X * X));
    }
    else
    {
        X = 0.;
        Y = 0.;
    }

    Player.SpeedMagnitude = Magnitude;
    constexpr float PushedStopMagnitude = 1.f;
    if (Player.bPushed && Magnitude < PushedStopMagnitude)
    {
        Player.bPushed = false;
    }

    if (TimeScale != 1.f)
    {
        X = float(X / double(TimeScale));
        Y = float(Y / double(TimeScale));
    }

    return { .X = float(X), .Y = float(Y) };
}

enum class EBounce : uint8_t
{
    StopThrust,
    Bounced,
    Accept,
};

// ObjectBounce at 004367d0 for the player against static terrain; the player skips the impact-point adjustment.
EBounce Bounce(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, const FCollisionPoint Contact)
{
    const FCollisionPoint Relative{ .X = float(double(Contact.X) - double(Player.Center.X)), .Y = float(double(Contact.Y) - double(Player.Center.Y)) };

    // ObjectMotionVector at 00436ac0: a blocked player that is thrusting probes along its facing instead of its velocity.
    FCollisionPoint Motion = Player.Velocity;
    bool bMoving = Player.Position.X != Player.PreviousPosition.X || Player.Position.Y != Player.PreviousPosition.Y;
    if (!bMoving && Command.Thrust != EPlayerThrust::None)
    {
        const FCollisionPoint Facing = FacingVector(Player.Direction);
        Motion = { .X = float(double(Player.Speed) * double(Facing.X)), .Y = float(double(Player.Speed) * double(Facing.Y)) };
        bMoving = true;
    }

    if (DirectionDistance(FindDirection(Relative), FindDirection(Motion)) >= FacingAwayDistance)
    {
        return EBounce::Accept;
    }

    if (!bMoving)
    {
        return EBounce::Bounced;
    }

    double Normal = std::atan(double(Relative.Y) / double(Relative.X));
    if (Relative.X < 0.f)
    {
        Normal = float(Normal) + double(Pi);
    }

    const float NormalAngle = float(Normal);
    EBounce Result = EBounce::Bounced;
    if (IsZero(Motion))
    {
        if (Command.Thrust == EPlayerThrust::None)
        {
            return EBounce::Bounced;
        }

        Result = EBounce::StopThrust;
        const double Scale = -(double(ImpactProbeSpeed) / std::sqrt(double(Relative.X) * Relative.X + double(Relative.Y) * Relative.Y));
        Motion = { .X = float(double(Player.Speed) * Scale * Relative.X), .Y = float(double(Player.Speed) * Scale * Relative.Y) };
    }

    double MotionAngle = std::atan(double(Motion.Y) / double(Motion.X));
    if (Motion.X < 0.f)
    {
        MotionAngle += double(Pi);
    }

    const double Projection = std::cos(double(NormalAngle) - MotionAngle) * std::sqrt(double(Motion.X) * Motion.X + double(Motion.Y) * Motion.Y);
    if (Projection == 0.)
    {
        const FCollisionPoint Away = DirectionVector(Wrap(FindDirection(Relative) + DirectionVectorOffset));
        Player.Velocity = { .X = float(double(Away.X) * PushAwaySpeed), .Y = float(double(Away.Y) * PushAwaySpeed) };
        return Result;
    }

    // Static terrain shapes are lines and rectangles, which reflect off the contact normal.
    const double Magnitude = std::abs(Projection);
    const FCollisionPoint Reflected{ .X = float(std::cos(double(NormalAngle)) * Magnitude), .Y = float(std::sin(double(NormalAngle)) * Magnitude) };
    double X = float((double(Motion.X) - Reflected.X) - Reflected.X);
    double Y = float((double(Motion.Y) - Reflected.Y) - Reflected.Y);
    if (Class.Elasticity != 1.f)
    {
        X = float(X * Class.Elasticity);
        Y = float(Y * Class.Elasticity);
    }

    Player.Velocity = { .X = float(X), .Y = float(Y) };
    return Result;
}

// ObjectPush at 00436dd0: an object hit. Immovable targets (zero friction) reflect the mover; otherwise both exchange momentum by weight.
EBounce Push(FPlayerState& Mover, const FMovementClass& MoverClass, const FPlayerCommand& Command, FObjectBody& Target, const FCollisionPoint Contact)
{
    const FCollisionPoint Relative{ .X = float(double(Contact.X) - double(Mover.Center.X)), .Y = float(double(Contact.Y) - double(Mover.Center.Y)) };
    FCollisionPoint Motion = Mover.Velocity;
    const bool bMoved = Mover.Position.X != Mover.PreviousPosition.X || Mover.Position.Y != Mover.PreviousPosition.Y;
    if (!bMoved && Command.Thrust != EPlayerThrust::None)
    {
        const FCollisionPoint Facing = FacingVector(Mover.Direction);
        Motion = { .X = float(double(Mover.Speed) * double(Facing.X)), .Y = float(double(Mover.Speed) * double(Facing.Y)) };
    }

    if (DirectionDistance(FindDirection(Relative), FindDirection(Motion)) >= FacingAwayDistance)
    {
        return EBounce::Accept;
    }

    const auto AngleFrom = [](const double X, const double Y)
    {
        const double Angle = std::atan(Y / X);
        return X < 0. ? Angle + double(Pi) : Angle;
    };
    const auto Project = [&](const FCollisionPoint Vector, const double Angle)
    {
        const double Length = std::cos(Angle - AngleFrom(Vector.X, Vector.Y)) * std::sqrt(double(Vector.X) * Vector.X + double(Vector.Y) * Vector.Y);
        return FCollisionPoint{ .X = float(std::cos(Angle) * float(Length)), .Y = float(std::sin(Angle) * float(Length)) };
    };

    const double Normal = AngleFrom(double(Mover.Center.X) - Contact.X, double(Mover.Center.Y) - Contact.Y);
    const FCollisionPoint Along = Project(Motion, Normal);
    const auto Elastic = [](const double Value, const float Elasticity) { return Elasticity == 1.f ? Value : Value * Elasticity; };
    if (Target.Class.Friction.X == 0.f)
    {
        Mover.Velocity = { .X = float(-Elastic(Along.X, MoverClass.Elasticity)), .Y = float(-Elastic(Along.Y, MoverClass.Elasticity)) };
        return EBounce::Bounced;
    }

    constexpr double WeightScale = 0.1;
    const double MoverWeight = float(MoverClass.Weight * WeightScale);
    const double TargetWeight = float(Target.Class.Weight * WeightScale);
    const double Total = float(TargetWeight + MoverWeight);
    FCollisionPoint Other{};
    if (!IsZero(Target.State.Velocity))
    {
        if (MoverClass.Friction.X == 0.f)
        {
            const double TargetNormal = float(AngleFrom(double(Target.State.Center.X) - Contact.X, double(Target.State.Center.Y) - Contact.Y));
            const FCollisionPoint Back = Project(Target.State.Velocity, TargetNormal);
            Target.State.Velocity = { .X = float(-Elastic(Back.X, Target.Class.Elasticity)), .Y = float(-Elastic(Back.Y, Target.Class.Elasticity)) };
            Target.State.bPushed = true;
            return EBounce::Bounced;
        }

        const double TargetNormal = float(AngleFrom(double(Target.State.Center.X) - Contact.X, double(Target.State.Center.Y) - Contact.Y));
        Other = Project(Target.State.Velocity, TargetNormal);
    }

    // Elastic exchange along the contact normal, weighted by Weight / 10.
    const auto Exchange = [&](const double Mine, const double Theirs, float& MoverOut, float& TargetOut)
    {
        const double Shared = float((MoverWeight * Mine + Theirs * TargetWeight) / Total);
        const double Relative = float((Mine - Theirs) / Total);
        MoverOut = float(Shared - float(Elastic(Relative, MoverClass.Elasticity)) * TargetWeight);
        TargetOut = float(float(Elastic(Relative, Target.Class.Elasticity)) * MoverWeight + Shared);
    };
    Exchange(Along.X, Other.X, Mover.Velocity.X, Target.State.Velocity.X);
    Exchange(Along.Y, Other.Y, Mover.Velocity.Y, Target.State.Velocity.Y);
    Target.State.bPushed = true;
    return EBounce::Bounced;
}

const FLevelPropertyBlock* FindBlock(const FLevelPropertyBlock& Block, std::string_view Kind);

const FLevelPropertyBlock* FindItemBlock(const FLevelObject& Object)
{
    for (const FLevelPropertyBlock& Child : Object.Properties.Children)
    {
        if (Child.Kind == "TYPE" && Child.Name == "Item")
        {
            return &Child;
        }
    }

    return nullptr;
}

// ponytail: crates also carry item blocks for the surprises they drop; the exe tells them apart by an item-attribute flag that is not decoded
// yet, so pickups are recognised by being indestructible.
bool IsPickup(const FLevelObject& Object)
{
    return Object.SubType == "item" && FindItemBlock(Object) && ParseLevelFloat(Object.Properties.Find("Indestructible")).value_or(0.f) != 0.f;
}

const FLevelPropertyBlock* FindBlock(const FLevelPropertyBlock& Block, const std::string_view Kind)
{
    for (const FLevelPropertyBlock& Child : Block.Children)
    {
        if (Child.Name.empty() && Child.Kind.size() == Kind.size() &&
            std::equal(Child.Kind.begin(), Child.Kind.end(), Kind.begin(), [](const char A, const char B) { return std::tolower(uint8_t(A)) == std::tolower(uint8_t(B)); }))
        {
            return &Child;
        }
    }

    return nullptr;
}

const FLevelPropertyBlock* FindTypeBlock(const FLevelPropertyBlock& Class, const std::string_view Name)
{
    const auto Found = std::ranges::find_if(Class.Children, [Name](const FLevelPropertyBlock& Child) { return Child.Kind == "TYPE" && Child.Name == Name; });
    return Found == Class.Children.end() ? nullptr : &*Found;
}

std::string ToLowerName(const std::string_view Text)
{
    std::string Lower(Text);
    std::ranges::transform(Lower, Lower.begin(), [](const char Character) { return char(std::tolower(uint8_t(Character))); });
    return Lower;
}

std::optional<FCollisionPoint> ParsePair(const std::string_view Text)
{
    const size_t Comma = Text.find(',');
    const std::optional<float> X = Comma == std::string_view::npos ? std::nullopt : ParseLevelFloat(Text.substr(0, Comma));
    const std::optional<float> Y = Comma == std::string_view::npos ? std::nullopt : ParseLevelFloat(Text.substr(Comma + 1));
    return X && Y ? std::optional<FCollisionPoint>({ .X = *X, .Y = *Y }) : std::nullopt;
}
}

int SelectHeliTilt(FActor& Actor, const float SpeedMagnitude)
{
    // Above each speed band's limit (2, 1 and 0.5 units per step) the counter climbs while at or below 24, 20 or 10 and otherwise drops,
    // except in the fastest band where it holds; slower than that it settles around 5.
    constexpr std::array<std::pair<float, int>, 3> Bands = { { { 2.f, 24 }, { 1.f, 20 }, { 0.5f, 10 } } };
    constexpr int SlowLimit = 5;
    const auto Band = std::ranges::find_if(Bands, [SpeedMagnitude](const auto& Entry) { return Entry.first < SpeedMagnitude; });
    const int Limit = Band == Bands.end() ? SlowLimit : Band->second;
    Actor.Tilt += Actor.Tilt <= Limit ? 1 : Band == Bands.begin() ? 0 : -1;
    constexpr int SecondTilt = 5;
    constexpr int ThirdTilt = 10;
    constexpr int FourthTilt = 20;
    return Actor.Tilt < SecondTilt ? 0 : Actor.Tilt < ThirdTilt ? 1 : Actor.Tilt < FourthTilt ? 2 : 3;
}

std::optional<uint32_t> ParseBehaviour(const std::string_view Text)
{
    uint32_t Bits = 0;
    bool bMovement = false;
    bool bTarget = false;
    bool bGuardian = false;
    for (const auto Word : Text | std::views::split(' '))
    {
        const std::string Name = ToLowerName(std::string_view(Word));
        if (Name.empty())
        {
            continue;
        }

        constexpr std::array<std::pair<std::string_view, uint32_t>, 5> Movements = { {
            { "player", BehaviourPlayer },
            { "random", BehaviourRandom },
            { "fixed", BehaviourFixed },
            { "patrol", BehaviourPatrol },
            { "straight", BehaviourStraight },
        } };
        if (const auto Movement = std::ranges::find(Movements, Name, &std::pair<std::string_view, uint32_t>::first); Movement != Movements.end())
        {
            if (Bits != 0)
            {
                return std::nullopt;
            }

            Bits = Movement->second;
            bMovement = Movement->second != BehaviourPlayer;
            bTarget = Movement->second == BehaviourPlayer;
        }
        else if (Name == "seeker" || Name == "tracker")
        {
            if (bTarget || !bMovement)
            {
                return std::nullopt;
            }

            Bits = (Bits & ~BehaviourTargetMask) | (Name == "seeker" ? BehaviourSeeker : BehaviourTracker);
            bTarget = true;
        }
        else if (Name == "guardian" && !bGuardian && bTarget && bMovement)
        {
            Bits |= BehaviourGuardian;
            bGuardian = true;
        }
        else
        {
            return std::nullopt;
        }
    }

    return Bits;
}

FPlayerCommand PlayerCommandFromKeys(const bool bForward, const bool bBackward, const bool bTurnLeft, const bool bTurnRight, const bool bStrafeLeft, const bool bStrafeRight)
{
    constexpr float StrafeSpeed = 2.f;
    FPlayerCommand Command{
        .Turn = bTurnLeft ? EPlayerTurn::Left : bTurnRight ? EPlayerTurn::Right : EPlayerTurn::None,
        .Thrust = bForward ? EPlayerThrust::Forward : bBackward ? EPlayerThrust::Backward : EPlayerThrust::None,
    };
    if (bStrafeLeft)
    {
        Command.Strafe = -StrafeSpeed;
    }

    if (bStrafeRight)
    {
        Command.Strafe = StrafeSpeed;
    }

    return Command;
}

FPlayerCommand PlayerCommandFromInput(const uint32_t InputBits)
{
    const auto Has = [InputBits](const uint32_t Bit) { return (InputBits & Bit) != 0; };
    FPlayerCommand Command = PlayerCommandFromKeys(Has(PlayerInputForward), Has(PlayerInputBackward), Has(PlayerInputTurnLeft), Has(PlayerInputTurnRight),
        Has(PlayerInputStrafeLeft), Has(PlayerInputStrafeRight));
    if (Command.Strafe != 0.f)
    {
        Command.Turn = EPlayerTurn::None;
    }

    return Command;
}

namespace
{
std::expected<FMovementClass, std::string> ReadMovementProperties(const FLevelPropertyBlock& Properties, const std::string_view ClassName, const int BoundingBoxSize)
{
    const auto Number = [&](const FLevelPropertyBlock& Block, const std::string_view Field) -> std::expected<float, std::string>
    {
        const std::optional<float> Value = ParseLevelFloat(Block.Find(Field));
        return Value ? std::expected<float, std::string>(*Value) : std::unexpected(std::format("{}: missing or invalid {}", ClassName, Field));
    };

    const FLevelPropertyBlock* const Thrust = FindBlock(Properties, "Thrust");
    const FLevelPropertyBlock* const Weight = FindBlock(Properties, "Weight");
    const std::optional<FCollisionPoint> Friction = ParsePair(Properties.Find("Friction-factor"));
    const std::optional<FCollisionPoint> Threshold = ParsePair(Properties.Find("Friction-threshold"));
    if (!Thrust || !Weight || !Friction || !Threshold)
    {
        return std::unexpected(std::format("{}: missing Thrust, Weight or friction properties", ClassName));
    }

    FMovementClass Class;
    for (const auto& [Target, Block, Field] : {
             std::tuple{ &Class.ThrustInitial, Thrust, "Initial" },
             std::tuple{ &Class.ThrustMin, Thrust, "Min" },
             std::tuple{ &Class.ThrustMax, Thrust, "Max" },
             std::tuple{ &Class.Weight, Weight, "Initial" },
             std::tuple{ &Class.AccelerationStep, &Properties, "Acceleration-step" },
             std::tuple{ &Class.DecelerationStep, &Properties, "Decceleration-step" },
             std::tuple{ &Class.SpeedMax, &Properties, "Speed-max" },
             std::tuple{ &Class.Elasticity, &Properties, "Elasticity" },
             std::tuple{ &Class.Grip, &Properties, "Grip" },
         })
    {
        const std::expected<float, std::string> Value = Number(*Block, Field);
        if (!Value)
        {
            return std::unexpected(Value.error());
        }

        *Target = *Value;
    }

    const std::expected<float, std::string> TurnTimeout = Number(Properties, "Turn-timeout");
    const std::expected<float, std::string> UseInertia = Number(Properties, "Use-inertia");
    if (!TurnTimeout || !UseInertia)
    {
        return std::unexpected(!TurnTimeout ? TurnTimeout.error() : UseInertia.error());
    }

    if (Class.Weight == 0.f)
    {
        return std::unexpected(std::format("{}: zero Weight", ClassName));
    }

    Class.Friction = *Friction;
    Class.FrictionThreshold = *Threshold;
    Class.TurnTimeout = int(*TurnTimeout);
    Class.bUseInertia = *UseInertia != 0.f;
    Class.Radius = BoundingBoxSize != 0 ? float(BoundingBoxSize) : DefaultCollisionRadius;
    return Class;
}
}

std::expected<FMovementClass, std::string> ReadMovementClass(const FLevelObject& Object)
{
    return ReadMovementProperties(Object.Properties, Object.ClassName, Object.BoundingBoxSize);
}

FPlayerState SpawnPlayer(const FLevelObject& Object, const FMovementClass& Class)
{
    FPlayerState Player{
        .Position = { .X = Object.X, .Y = Object.Y },
        .Speed = Class.ThrustInitial,
        .Direction = Object.Direction < DirectionCount ? Object.Direction : 0,
    };
    Player.PreviousPosition = Player.Position;
    Player.Center = CollisionFromWorld(Player.Position);
    Player.Pose = Player.TargetPose = Player.Direction * PosesPerDirection + 1;
    return Player;
}

const FObjectBody* StepPlayer(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, const uint32_t StepMilliseconds,
    const FTerrainCollision& Terrain, const FLevel& Level, const std::span<FObjectBody> Bodies, const FObjectBody* const Self)
{
    // Actors turn their body in their behaviour update instead (SeekerTurnBody at 0042cfa0).
    if (!Self || !Self->Actor)
    {
        TurnPlayer(Player, Class, Command.Turn, StepMilliseconds);
    }

    if (Command.Thrust == EPlayerThrust::None && Command.Strafe == 0.f && IsZero(Player.Velocity))
    {
        Player.PreviousPosition = Player.Position;
        DecaySpeed(Player, Class);
        return nullptr;
    }

    const float TimeScale = float(ReferenceStepMilliseconds / double(StepMilliseconds));
    const FCollisionPoint Delta = Displacement(Player, Class, Command, TimeScale);
    if (IsZero(Delta) && Player.Pose == Player.TargetPose)
    {
        Player.PreviousPosition = Player.Position;
        Player.Velocity = {};
        return nullptr;
    }

    const FCollisionPoint Target{ .X = float(double(Delta.X) + double(Player.Center.X)), .Y = float(double(Delta.Y) + double(Player.Center.Y)) };
    Player.PreviousPosition = WorldFromCollision(Player.Center);
    Player.Position = WorldFromCollision(Target);

    // The player is held inside the level instead of being removed like other objects.
    constexpr float LowerLimit = 1.f;
    constexpr float UpperMargin = 2.f;
    const float ExtentX = float(Level.WorldExtentX);
    const float ExtentY = float(Level.WorldExtentY);
    if (Player.Position.X < LowerLimit)
    {
        Player.Position.X = LowerLimit;
    }
    else if (ExtentX <= Player.Position.X)
    {
        Player.Position.X = float(double(ExtentX) - UpperMargin);
    }

    if (Player.Position.Y < LowerLimit)
    {
        Player.Position.Y = LowerLimit;
    }
    else if (ExtentY <= Player.Position.Y)
    {
        Player.Position.Y = float(double(ExtentY) - UpperMargin);
    }

    const FCollisionPoint Candidate = CollisionFromWorld(Player.Position);
    Player.Velocity = { .X = float(double(Candidate.X) - double(Player.Center.X)), .Y = float(double(Candidate.Y) - double(Player.Center.Y)) };
    const std::optional<FTerrainHit> TerrainHit = QueryTerrainCollision(Terrain, { .Center = Player.Center, .Radius = Class.Radius }, { .Center = Candidate, .Radius = Class.Radius },
        Class.bOverWater);

    // ponytail: objects are scanned after the terrain in object order instead of interleaved through the sweep endpoints; the nearest contact
    // still wins, so only exact ties between a wall and an object can resolve differently.
    std::optional<FCollisionPoint> Contact = TerrainHit ? std::optional(TerrainHit->Point) : std::nullopt;
    FObjectBody* HitBody = nullptr;
    for (FObjectBody& Body : Bodies)
    {
        if (&Body == Self || Body.bPassable)
        {
            continue;
        }

        const std::optional<FCollisionPoint> BodyContact = IntersectCircles({ .Center = Candidate, .Radius = Class.Radius }, { .Center = Body.State.Center, .Radius = Body.Class.Radius });
        if (BodyContact && (!Contact || PrefersContact(*Contact, *BodyContact, Candidate)))
        {
            Contact = BodyContact;
            HitBody = &Body;
        }
    }

    if (!Contact)
    {
        Player.Velocity = Class.bDropsVelocity ? FCollisionPoint{} : FCollisionPoint{ .X = float(double(TimeScale) * Delta.X), .Y = float(double(TimeScale) * Delta.Y) };
        Player.Center = Candidate;
        return nullptr;
    }

    // ObjectRestorePosition at 004449f0 undoes the move before the bounce is resolved.
    Player.Position = Player.PreviousPosition;
    const EBounce Result = HitBody ? Push(Player, Class, Command, *HitBody, *Contact) : Bounce(Player, Class, Command, *Contact);
    if (Result == EBounce::Accept)
    {
        // Moving away from the contact commits the collision centre; the world position catches up on the next step.
        Player.Center = Candidate;
    }

    return HitBody;
}

std::optional<FObjectBody> CreateObjectBody(const FLevel& Level, const size_t Index)
{
    const FLevelObject& Object = Level.Objects[Index];
    // Kinds come from the class Sub-type or the GAF folder (CreateObject at 00413800); pickups are collected instead of blocking.
    constexpr std::array<std::string_view, 8> SolidKinds = { "item", "thing", "turret", "staticgen", "burner", "roller", "lift", "gate" };
    constexpr std::array<std::string_view, 6> ActorKinds = { "cop", "jeep", "tank", "walker", "spider", "heli" };
    constexpr std::string_view CopKind = "cop";
    constexpr std::string_view TankKind = "tank";
    constexpr std::string_view WalkerKind = "walker";
    constexpr std::string_view SpiderKind = "spider";
    const bool bActor = std::ranges::contains(ActorKinds, Object.SubType);
    if (Object.BoundingBoxSize <= 0 || (!bActor && !std::ranges::contains(SolidKinds, Object.SubType)) || IsPickup(Object))
    {
        return std::nullopt;
    }

    const FLevelPropertyBlock& Properties = Object.Properties;
    const auto Number = [](const FLevelPropertyBlock* Block, const std::string_view Field)
    {
        return Block ? ParseLevelFloat(Block->Find(Field)).value_or(0.f) : 0.f;
    };
    FMovementClass Class{
        .Weight = Number(FindBlock(Properties, "Weight"), "Initial"),
        .DecelerationStep = Number(&Properties, "Decceleration-step"),
        .Elasticity = Number(&Properties, "Elasticity"),
        .Grip = Number(&Properties, "Grip"),
        .Friction = ParsePair(Properties.Find("Friction-factor")).value_or(FCollisionPoint{}),
        .FrictionThreshold = ParsePair(Properties.Find("Friction-threshold")).value_or(FCollisionPoint{}),
        .bUseInertia = Number(&Properties, "Use-inertia") != 0.f,
        .Radius = float(Object.BoundingBoxSize),
    };
    FObjectBody Body{
        .ObjectIndex = Index,
        .Class = Class,
        .Energy = Number(FindBlock(Properties, "Energy"), "Initial"),
        .Shield = Number(FindBlock(Properties, "Shield"), "Initial"),
        .Reward = Number(&Properties, "Reward"),
        .bIndestructible = Number(&Properties, "Indestructible") != 0.f,
    };
    for (const FLevelPropertyBlock& Child : Properties.Children)
    {
        if (Child.Kind == "TYPE" && Child.Name == "Absorption")
        {
            Body.Absorptions.push_back({ .AmmoClass = ToLowerName(Child.Find("Class-name")), .Coeff = Number(&Child, "Coeff") });
        }
    }

    for (const FLevelPropertyBlock& Child : Properties.Children)
    {
        if (Child.Kind == "TYPE" && Child.Name == "Explosion")
        {
            Body.Explosions.push_back({ .BoomEffect = ToLowerName(Child.Find("Boom-fx")), .Damage = Number(&Child, "Damage"), .Zone = Number(&Child, "Damage-zone") });
        }
    }

    Body.Altitude = Object.Altitude;
    // .3DO instances can override the starting Energy and Shield with plain fields.
    Body.Energy = ParseLevelFloat(Properties.Find("Energy")).value_or(Body.Energy);
    Body.Shield = ParseLevelFloat(Properties.Find("Shield")).value_or(Body.Shield);
    Body.State.Position = Body.State.PreviousPosition = { .X = Object.X, .Y = Object.Y };
    Body.State.Center = CollisionFromWorld(Body.State.Position);
    if (bActor)
    {
        const std::optional<uint32_t> Behaviour = ParseBehaviour(Object.Behaviour);
        const std::expected<FMovementClass, std::string> Movement = ReadMovementProperties(Properties, Object.ClassName, Object.BoundingBoxSize);
        if (!Behaviour || !Movement)
        {
            return std::nullopt;
        }

        Body.Class = *Movement;
        const bool bLegged = Object.SubType == WalkerKind || Object.SubType == SpiderKind;
        Body.Class.bThrustIgnoresWeight = bLegged;
        Body.Class.bDropsVelocity = bLegged || Object.SubType == TankKind;
        Body.Class.bOverWater = Number(&Properties, "Over-water") != 0.f;
        Body.State.Speed = Movement->ThrustInitial;
        Body.State.Direction = Object.Direction < DirectionCount ? Object.Direction : 0;
        const int ActorPoses = Object.SubType == CopKind ? PosesPerDirection : 1;
        Body.State.Pose = Body.State.TargetPose = Body.State.Direction * ActorPoses + 1;
        constexpr float TankCorrectionScale = 0.01f;
        FActor& Actor = Body.Actor.emplace(FActor{
            .Behaviour = *Behaviour,
            .bTurnOnPlace = Number(&Properties, "Can-turn-on-place") != 0.f,
            .PosesPerDirection = ActorPoses,
            .bTwoPieces = Number(&Properties, "Two-pieces") != 0.f,
            .CorrectionScale = Object.SubType == TankKind || bLegged ? TankCorrectionScale : 1.f,
            .bSpider = Object.SubType == SpiderKind,
            .TopPose = Body.State.Pose,
            .TopTargetPose = Body.State.Pose,
            .TopTurnTimeout = int32_t(Number(&Properties, "Turret-turn-timeout")),
        });
        if (const FLevelPropertyBlock* const Patrol = FindTypeBlock(Properties, "Patrol"); Patrol && (*Behaviour & BehaviourMovementMask) == BehaviourPatrol)
        {
            const int PathId = int(Number(Patrol, "Path-ID"));
            const auto Path = std::ranges::find(Level.PatrolPaths | std::views::reverse, PathId, &FPatrolPath::Id);
            Actor.Patrol = FPatrol{
                .Path = Path.base() == Level.PatrolPaths.begin() ? std::nullopt : std::optional(size_t(Path.base() - Level.PatrolPaths.begin()) - 1),
                .bReverse = Number(Patrol, "Reverse") != 0.f,
                .bClosed = Number(Patrol, "Closed") != 0.f,
                .Work = { .Interval = int32_t(Number(Patrol, "Scan-delay")), .Radius = Object.SubType == SpiderKind ? SpiderArrivalRadius
                                                                                       : Object.SubType == WalkerKind ? WalkerArrivalRadius
                                                                                                                      : PatrolArrivalRadius },
            };
        }

        if (const FLevelPropertyBlock* const Random = FindTypeBlock(Properties, "Random"); Random && (*Behaviour & BehaviourMovementMask) == BehaviourRandom)
        {
            // RandomMoveUpdate gives its work attribute no arrival radius, so each route is driven to its end.
            Actor.RandomMove = FRandomMove{ .Range = Number(Random, "Range"), .Work = { .Interval = int32_t(Number(Random, "Delay")) } };
        }

        // SeekerFindAttribute (0042c510): a seeker drives the TYPE Seeker block, a tracker the TYPE Tracker block.
        const uint32_t Target = *Behaviour & BehaviourTargetMask;
        const bool bTracker = Target == BehaviourTracker;
        if (const FLevelPropertyBlock* const Seeker = FindTypeBlock(Properties, bTracker ? "Tracker" : "Seeker"); Seeker && (Target == BehaviourSeeker || bTracker))
        {
            // ponytail: the Vision-Angle conversion is not traced; it is read as for turrets, half a direction per 30 degrees.
            constexpr float DegreesPerVisionStep = 30.f;
            Actor.Seeker = FSeeker{
                .bTracker = bTracker,
                .VisionDirections = int(Number(Seeker, "Vision-Angle") / DegreesPerVisionStep),
                .SensorRange = Number(Seeker, "Sensor-Range"),
                .Work = { .Interval = int32_t(Number(Seeker, "Scan-Delay")), .Radius = uint32_t(Number(Seeker, "Min-distance")) },
            };
            // A negative Location means the placement position.
            if (const FLevelPropertyBlock* const Guardian = FindTypeBlock(Properties, "Guardian"); Guardian && (*Behaviour & BehaviourGuardian) != 0)
            {
                const FCollisionPoint Location = ParsePair(Guardian->Find("Location")).value_or(FCollisionPoint{ .X = -1.f, .Y = -1.f });
                Actor.Seeker->bGuardian = true;
                Actor.Seeker->GuardRange = Number(Guardian, "Range");
                Actor.Seeker->GuardLocation = Location.X < 0.f || Location.Y < 0.f ? Body.State.Position : Location;
            }
        }
    }

    if (const FLevelPropertyBlock* const Generator = FindTypeBlock(Properties, "Generator"))
    {
        // Keywords 0x66 to 0x6d of LoadObjectClassLibrary: Active (flag 2) starts the first birth at once.
        Body.Generator = FGenerator{
            .Product = ToLowerName(Generator->Find("Product")),
            .MaxBirths = int(Number(Generator, "Max-births")),
            .BirthDelay = int32_t(Number(Generator, "Birth-delay")),
            .BirthDuration = int32_t(Number(Generator, "Birth-duration")),
            .bStressable = Number(Generator, "Stressable") != 0.f,
            .bBirthing = Number(Generator, "Active") != 0.f,
            .bStablePopulation = Number(Generator, "Stable-population") != 0.f,
        };
    }

    return Body;
}

std::vector<FObjectBody> CreateObjectBodies(const FLevel& Level)
{
    std::vector<FObjectBody> Bodies;
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        if (std::optional<FObjectBody> Body = CreateObjectBody(Level, Index))
        {
            Bodies.push_back(std::move(*Body));
        }
    }

    return Bodies;
}

void StepObjectBodies(const std::span<FObjectBody> Bodies, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level)
{
    for (FObjectBody& Body : Bodies)
    {
        // Actors move every step in StepActors.
        if (!Body.Actor && (Body.State.bPushed || !IsZero(Body.State.Velocity)))
        {
            StepPlayer(Body.State, Body.Class, {}, StepMilliseconds, Terrain, Level, Bodies, &Body);
        }
    }
}

std::expected<void, std::string> VerifyPlayerMovement()
{
    for (int Direction = 0; Direction < DirectionCount; ++Direction)
    {
        if (FindDirection(FacingVector(Direction)) != Direction)
        {
            return std::unexpected(std::format("Direction {} does not round-trip through FindDirection", Direction));
        }
    }

    if (FindDirection({}) != NoDirection || AdvancePose(1, 47) != PlayerPoseCount || AdvancePose(PlayerPoseCount, 2) != 1 || AdvancePose(5, 3) != 4)
    {
        return std::unexpected("Player pose stepping self-check failed");
    }

    const FPlayerCommand Demo = PlayerCommandFromInput(PlayerInputTurnLeft | PlayerInputStrafeRight | PlayerInputForward);
    const FPlayerCommand Keys = PlayerCommandFromKeys(true, false, true, false, false, true);
    if (Demo.Turn != EPlayerTurn::None || Demo.Strafe <= 0.f || Demo.Thrust != EPlayerThrust::Forward || Keys.Turn != EPlayerTurn::Left)
    {
        return std::unexpected("Player command self-check failed");
    }

    // Twenty forward steps east from rest on open ground: speed takes twenty acceleration steps and only world X grows.
    // The first step does not move, since its push stays under the friction threshold.
    FLevel Level;
    Level.WorldExtentX = 4096;
    Level.WorldExtentY = 4096;
    const FTerrainCollision Terrain;
    const FMovementClass Class{
        .ThrustMin = -10.f,
        .ThrustMax = 45.f,
        .Weight = 15.f,
        .AccelerationStep = 1.f,
        .DecelerationStep = 0.5f,
        .SpeedMax = 45.f,
        .Friction = { .X = 0.972f, .Y = 0.972f },
        .FrictionThreshold = { .X = 0.01f, .Y = 0.01f },
        .TurnTimeout = 18,
        .bUseInertia = true,
    };
    FPlayerState Player{ .Position = { .X = 2000.f, .Y = 2000.f }, .Direction = 9 };
    Player.Center = CollisionFromWorld(Player.Position);
    constexpr int ForwardSteps = 20;
    constexpr uint32_t ReferenceStep = uint32_t(ReferenceStepMilliseconds);
    for (int Step = 0; Step < ForwardSteps; ++Step)
    {
        StepPlayer(Player, Class, { .Thrust = EPlayerThrust::Forward }, ReferenceStep, Terrain, Level);
    }

    constexpr float Tolerance = 0.01f;
    // Ammo has positive minimum thrust; zero strafe must stay zero rather than become sideways thrust.
    constexpr float BulletThrust = 300.f;
    const FMovementClass BulletClass{
        .ThrustInitial = BulletThrust,
        .ThrustMin = BulletThrust,
        .ThrustMax = BulletThrust,
        .Weight = 1.f,
        .SpeedMax = BulletThrust,
        .Friction = { .X = 1.f, .Y = 1.f },
    };
    for (int Direction = 0; Direction < DirectionCount; ++Direction)
    {
        FPlayerState Bullet{ .Speed = BulletThrust, .Direction = Direction };
        const FCollisionPoint Delta = Displacement(Bullet, BulletClass, { .Thrust = EPlayerThrust::Forward }, 1.f);
        const FCollisionPoint Facing = FacingVector(Direction);
        if (std::abs(Delta.X - BulletThrust * Facing.X) > Tolerance || std::abs(Delta.Y - BulletThrust * Facing.Y) > Tolerance)
        {
            return std::unexpected("Stationary bullet direction self-check failed");
        }
    }

    if (Player.Speed != float(ForwardSteps) || !(Player.Position.X > 2000.f) || std::abs(Player.Position.Y - 2000.f) > Tolerance)
    {
        return std::unexpected("Player forward step self-check failed");
    }

    // Driving east into a body: an immovable one (zero friction) stops the player short of it, a movable one is pushed away.
    for (const float BodyFriction : { 0.f, 0.9f })
    {
        FPlayerState Mover{ .Position = { .X = 2000.f, .Y = 2000.f }, .Direction = 9 };
        Mover.Center = CollisionFromWorld(Mover.Position);
        FObjectBody Body{ .Class = { .Weight = 4.f, .Elasticity = 1.f, .Friction = { .X = BodyFriction, .Y = BodyFriction }, .Radius = 10.f } };
        Body.State.Position = Body.State.PreviousPosition = { .X = 2100.f, .Y = 2000.f };
        Body.State.Center = CollisionFromWorld(Body.State.Position);
        std::array<FObjectBody, 1> Bodies{ Body };
        constexpr int PushSteps = 200;
        for (int Step = 0; Step < PushSteps; ++Step)
        {
            StepPlayer(Mover, Class, { .Thrust = EPlayerThrust::Forward }, ReferenceStep, Terrain, Level, Bodies);
            StepObjectBodies(Bodies, ReferenceStep, Terrain, Level);
        }

        const bool bBlocked = Mover.Position.X < Bodies[0].State.Position.X;
        const bool bPushed = Bodies[0].State.Position.X > 2100.f;
        if (!bBlocked || bPushed != (BodyFriction != 0.f))
        {
            return std::unexpected("Object collision self-check failed");
        }
    }

    Level.GridWidth = 8;
    Level.GridHeight = 8;
    Level.Cells.resize(size_t{ Level.GridWidth } * Level.GridHeight);
    Level.TileTypes = { ETerrain::None, ETerrain::Ground };
    Level.Cells[3 * Level.GridHeight + 3][0] = 1;
    const auto WallTerrain = BuildTerrainCollision(Level);
    if (!WallTerrain || WallTerrain->GetShapes().size() != 4)
    {
        return std::unexpected("Object wall fixture self-check failed");
    }

    // A pushed crate must stay on its side of every wall, including a shallow initial overlap.
    for (const FStaticCollisionShape& Shape : WallTerrain->GetShapes())
    {
        const bool bVertical = Shape.Points[0].X == Shape.Points[1].X;
        const FCollisionPoint Middle{
            .X = (Shape.Points[0].X + Shape.Points[1].X) * 0.5f,
            .Y = (Shape.Points[0].Y + Shape.Points[1].Y) * 0.5f,
        };
        constexpr float Radius = 10.f;
        FMovementClass WallClass = Class;
        WallClass.Radius = Radius;
        constexpr float ApproachDistance = 20.f;
        constexpr float ShallowOverlap = 0.25f;
        constexpr float CollisionInset = 0.5f;
        constexpr float PlayerGap = 5.f;
        constexpr int PushSteps = 200;
        for (const float Side : { -1.f, 1.f })
        {
            for (const float Gap : { ApproachDistance, -ShallowOverlap })
            {
                const FCollisionPoint Normal{ .X = bVertical ? Side : 0.f, .Y = bVertical ? 0.f : Side };
                FObjectBody Body{ .Class = { .Weight = 4.f, .Elasticity = 1.f, .Friction = { .X = 0.9f, .Y = 0.9f }, .Radius = Radius } };
                Body.State.Center = { .X = Middle.X + Normal.X * (Radius + Gap), .Y = Middle.Y + Normal.Y * (Radius + Gap) };
                Body.State.Position = Body.State.PreviousPosition = WorldFromCollision(Body.State.Center);
                FPlayerState Mover{
                    .Center = { .X = Body.State.Center.X + Normal.X * (WallClass.Radius + Radius + PlayerGap), .Y = Body.State.Center.Y + Normal.Y * (WallClass.Radius + Radius + PlayerGap) },
                    .Direction = FindDirection({ .X = bVertical ? -Side : 0.f, .Y = bVertical ? 0.f : -Side }),
                };
                Mover.Position = Mover.PreviousPosition = WorldFromCollision(Mover.Center);
                std::array<FObjectBody, 1> Bodies{ Body };
                bool bPushed = false;
                for (int Step = 0; Step < PushSteps; ++Step)
                {
                    StepPlayer(Mover, WallClass, { .Thrust = EPlayerThrust::Forward }, ReferenceStep, *WallTerrain, Level, Bodies);
                    bPushed |= Bodies[0].State.bPushed;
                    StepObjectBodies(Bodies, ReferenceStep, *WallTerrain, Level);
                    const FCollisionPoint Center = Bodies[0].State.Center;
                    const float Distance = Side * (bVertical ? Center.X - Middle.X : Center.Y - Middle.Y);
                    if (Distance < Radius - CollisionInset - Tolerance)
                    {
                        return std::unexpected(std::format("Pushed object crossed terrain wall: vertical {}, side {}, gap {}, distance {}", bVertical, Side, Gap, Distance));
                    }
                }

                if (!bPushed)
                {
                    return std::unexpected(std::format("Object wall push did not reach crate: wall {},{}, vertical {}, side {}, gap {}", Middle.X, Middle.Y, bVertical, Side, Gap));
                }
            }
        }
    }

    return {};
}

std::expected<std::string, std::string> VerifyDemoReplay(FResourceArchive& Archive)
{
    // The check ignores the floating messages it raises.
    FMessages Messages;
    // One .3DR record per game step: the step's start time, its input bits, and the player's position and direction before the step.
    struct FDemoRecord
    {
        uint32_t Time;
        uint32_t Input;
        float X;
        float Y;
        int32_t Direction;
    };
    static_assert(sizeof(FDemoRecord) == 20);

    const std::optional<std::vector<uint8_t>> File = Archive.Read("cave\\demo.3dr");
    const std::optional<uint32_t> Count = File ? ReadStruct<uint32_t>(*File) : std::nullopt;
    if (!Count || *Count < 2 || (File->size() - sizeof(uint32_t)) / sizeof(FDemoRecord) < *Count)
    {
        return std::unexpected("cave\\demo.3dr missing or truncated");
    }

    std::vector<FDemoRecord> Records(*Count);
    for (size_t Index = 0; Index < Records.size(); ++Index)
    {
        Records[Index] = *ReadStruct<FDemoRecord>(*File, sizeof(uint32_t) + Index * sizeof(FDemoRecord));
    }

    const std::expected<FLevel, std::string> Level = LoadLevel(Archive, "cave\\demo");
    if (!Level)
    {
        return std::unexpected(Level.error());
    }

    const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
    const auto Object = std::ranges::find_if(Level->Objects, [](const FLevelObject& Candidate) { return Candidate.Behaviour == "Player"; });
    if (!Terrain || Object == Level->Objects.end())
    {
        return std::unexpected("cave\\demo has no terrain collision or player");
    }

    std::expected<FMovementClass, std::string> Class = ReadMovementClass(*Object);
    if (!Class)
    {
        return std::unexpected(Class.error());
    }

    // The recording build used Friction-threshold 0.001, the value commented out in the shipped OBJECT.3DC: with 0.01 the first
    // recorded forward step would not move at all.
    constexpr float RecordingFrictionThreshold = 0.001f;
    Class->FrictionThreshold = { .X = RecordingFrictionThreshold, .Y = RecordingFrictionThreshold };

    // Record times are clock readings at each step start. Recording caps steps at 10 ms, so a frame whose poll measured E ms
    // produced ceil(E / 10) records with steps of 10 ms and a remainder; E is the gap since the previous frame's first record.
    // ponytail: the 1 ms clock can misplace E by one millisecond, which accounts for most remaining coasting mismatches.
    std::vector<uint32_t> StepLengths(Records.size(), DemoStepMilliseconds);
    uint32_t PreviousFrameTime = Records.front().Time - DemoStepMilliseconds;
    for (size_t First = 0; First < Records.size();)
    {
        const uint32_t Elapsed = Records[First].Time - PreviousFrameTime;
        const size_t FrameSteps = std::max<size_t>(1, (Elapsed + DemoStepMilliseconds - 1) / DemoStepMilliseconds);
        for (size_t Offset = 0; Offset < FrameSteps && First + Offset < Records.size(); ++Offset)
        {
            const uint32_t Consumed = uint32_t(Offset) * DemoStepMilliseconds;
            StepLengths[First + Offset] = Elapsed > Consumed ? std::min(DemoStepMilliseconds, Elapsed - Consumed) : 0;
        }

        PreviousFrameTime = Records[First].Time;
        First += FrameSteps;
    }

    constexpr float ExactTolerance = 0.001f;
    constexpr float CloseTolerance = 0.5f;
    size_t Exact = 0;
    size_t Close = 0;
    // ponytail: the replay keeps to terrain until weapons and object movement exist; the demo shoots barrels and crates and shoves pallets,
    // so the recorded path crosses where those objects started.
    FPlayerState Player = SpawnPlayer(*Object, *Class);
    FPlayerState FreeRun = Player;
    FLevel PickupLevel = *Level;
    std::vector<FPickup> Pickups = CreatePickups(PickupLevel);
    FPlayerInventory Inventory = CreateInventory(*Object);
    size_t Collected = 0;
    FreeRun.Position = { .X = Records.front().X, .Y = Records.front().Y };
    FreeRun.Center = CollisionFromWorld(FreeRun.Position);
    size_t FreeRunSteps = Records.size() - 1;
    for (size_t Index = 0; Index + 1 < Records.size(); ++Index)
    {
        const FDemoRecord& Record = Records[Index];
        const FDemoRecord& Next = Records[Index + 1];
        const uint32_t Step = StepLengths[Index];
        const FPlayerCommand Command = PlayerCommandFromInput(Record.Input);

        // Each step starts from the recorded state: an accepted move stores velocity = (14 / step) * centre delta, and the facing comes
        // from the next record so that the turn-timer phase, which predates the recording, cannot leak in.
        Player.Position = { .X = Record.X, .Y = Record.Y };
        Player.Center = CollisionFromWorld(Player.Position);
        Player.Velocity = {};
        if (Index > 0 && StepLengths[Index - 1] != 0)
        {
            const FDemoRecord& Previous = Records[Index - 1];
            const FCollisionPoint PreviousCenter = CollisionFromWorld({ .X = Previous.X, .Y = Previous.Y });
            const float Scale = float(ReferenceStepMilliseconds / double(StepLengths[Index - 1]));
            Player.Velocity = {
                .X = float(double(Scale) * (double(Player.Center.X) - PreviousCenter.X)),
                .Y = float(double(Scale) * (double(Player.Center.Y) - PreviousCenter.Y)),
            };
            Player.PreviousPosition = { .X = Previous.X, .Y = Previous.Y };
        }

        Player.SpeedMagnitude = float(std::hypot(double(Player.Velocity.X), double(Player.Velocity.Y)));
        if (Next.Direction >= 0 && Next.Direction < DirectionCount)
        {
            Player.Direction = Next.Direction;
            Player.Pose = Player.TargetPose = Player.Direction * PosesPerDirection + 1;
        }

        StepPlayer(Player, *Class, { .Thrust = Command.Thrust, .Strafe = Command.Strafe }, Step, *Terrain, *Level);
        const float Error = std::hypot(Player.Position.X - Next.X, Player.Position.Y - Next.Y);
        // Pickups along the recorded path, collected from the recorded position.
        FPlayerState Recorded{ .Center = CollisionFromWorld({ .X = Next.X, .Y = Next.Y }) };
        Collected += CollectPickups(Recorded, *Class, Inventory, Pickups, PickupLevel, Messages);
        Exact += Error <= ExactTolerance;
        Close += Error <= CloseTolerance;

        // The free run carries all state forward, turn timing included, until it first leaves the recording.
        if (FreeRunSteps == Records.size() - 1)
        {
            StepPlayer(FreeRun, *Class, Command, Step, *Terrain, *Level);
            if (std::hypot(FreeRun.Position.X - Next.X, FreeRun.Position.Y - Next.Y) > CloseTolerance)
            {
                FreeRunSteps = Index;
            }
        }
    }

    // Floors guard against regressions; object collisions are not modelled yet, and step lengths are inferred from 1 ms timestamps.
    constexpr double MinimumExactShare = 0.8;
    constexpr double MinimumCloseShare = 0.9;
    const size_t Steps = Records.size() - 1;
    const std::string Summary = std::format("{} of {} steps exact, {} within {} px, free run follows the recording for {} steps, {} of {} pickups collected", Exact,
        Steps, Close, CloseTolerance, FreeRunSteps, Collected, Pickups.size());
    if (double(Exact) < MinimumExactShare * double(Steps) || double(Close) < MinimumCloseShare * double(Steps))
    {
        return std::unexpected(Summary);
    }

    return Summary;
}

namespace
{
// FUN_0041ba80 maps the Surprise-type keywords; any other word is no surprise.
ESurprise SurpriseFromName(const std::string_view Name)
{
    static constexpr std::array<std::pair<std::string_view, ESurprise>, 20> Names = { {
        { "random", ESurprise::Random },
        { "bonus-auto-shield-up", ESurprise::AutoShieldUp },
        { "bonus-circular-fire", ESurprise::CircularFire },
        { "bonus-destroy-around", ESurprise::DestroyAround },
        { "bonus-invisible", ESurprise::Invisible },
        { "bonus-invulnerability", ESurprise::Invulnerability },
        { "bonus-nop", ESurprise::Nop },
        { "bonus-reload-weapons", ESurprise::ReloadWeapons },
        { "bonus-time-warp", ESurprise::TimeWarp },
        { "bonus-touch-and-destroy", ESurprise::TouchAndDestroy },
        { "bonus-triple-fire", ESurprise::TripleFire },
        { "malus-banana", ESurprise::Banana },
        { "malus-blocked-motor", ESurprise::BlockedMotor },
        { "malus-darken", ESurprise::Darken },
        { "malus-extasy", ESurprise::Extasy },
        { "malus-low-speed", ESurprise::LowSpeed },
        { "malus-pinball", ESurprise::Pinball },
        { "malus-reverse-commands", ESurprise::ReverseCommands },
        { "malus-trap", ESurprise::Trap },
        { "malus-weapon-jam", ESurprise::WeaponJam },
    } };
    const std::string Lower = ToLowerName(Name);
    const auto Found = std::ranges::find(Names, std::string_view(Lower), &std::pair<std::string_view, ESurprise>::first);
    return Found == Names.end() ? ESurprise::None : Found->second;
}

// FUN_0041ba00: the weights of DAT_0050d5d0, summed at load by FUN_0041b830, choose the type of a random surprise.
ESurprise DrawSurprise(FRandom& Random)
{
    static constexpr std::array<int, 21> Weights = { 0, 0, 10, 10, 5, 10, 5, 10, 0, 10, 1, 5, 5, 10, 10, 5, 5, 5, 10, 10, 10 };
    static constexpr std::array<int, 21> Cumulative = []
    {
        std::array<int, 21> Sums{};
        int Sum = 0;
        for (size_t Index = 0; Index < Weights.size(); ++Index)
        {
            Sum += Weights[Index];
            Sums[Index] = Sum;
        }

        return Sums;
    }();
    constexpr int FallbackLow = 2;
    constexpr int FallbackHigh = 0x14;
    const int Draw = Random.Range(0, Cumulative.back() - 1);
    for (size_t Index = 0; Index < Cumulative.size(); ++Index)
    {
        if (Draw < Cumulative[Index])
        {
            return ESurprise(Index);
        }
    }

    return ESurprise(Random.Range(FallbackLow, FallbackHigh));
}

// FUN_0041bfe0: how long each surprise lasts, in milliseconds.
int32_t SurpriseTimeout(const ESurprise Type)
{
    switch (Type)
    {
    case ESurprise::ReverseCommands:
    case ESurprise::WeaponJam:
    case ESurprise::Extasy:
    case ESurprise::CircularFire:
    case ESurprise::Invulnerability:
        return 10000;
    case ESurprise::Trap:
    case ESurprise::DestroyAround:
    case ESurprise::Nop:
        return 1;
    case ESurprise::Darken:
    case ESurprise::LowSpeed:
    case ESurprise::Pinball:
    case ESurprise::TripleFire:
        return 20000;
    case ESurprise::Banana:
    case ESurprise::AutoShieldUp:
    case ESurprise::ReloadWeapons:
        return 15000;
    case ESurprise::BlockedMotor:
        return 5000;
    case ESurprise::TouchAndDestroy:
        return 8000;
    case ESurprise::Invisible:
        return 16000;
    case ESurprise::TimeWarp:
        return 12000;
    default:
        return 0;
    }
}

// FUN_0041bd20: the text that announces a surprise.
std::string_view SurpriseText(const ESurprise Type)
{
    switch (Type)
    {
    case ESurprise::ReverseCommands: return "Reversed Commands !";
    case ESurprise::Trap: return "Trap !!!!";
    case ESurprise::WeaponJam: return "Weapon Jam !";
    case ESurprise::Darken: return "No Light !!!!";
    case ESurprise::Extasy: return "XTC !!!!";
    case ESurprise::LowSpeed: return "Thrust loss !!!!";
    case ESurprise::Pinball: return "Pinball Effect !";
    case ESurprise::Banana: return "No Inertia Control !";
    case ESurprise::BlockedMotor: return "Blocked Motor !!!!";
    case ESurprise::DestroyAround: return "Destroy Around !!!!";
    case ESurprise::TouchAndDestroy: return "Touch & Destroy !";
    case ESurprise::AutoShieldUp: return "Auto Shield Up";
    case ESurprise::Invisible: return "Invisible Drone !";
    case ESurprise::CircularFire: return "Circular Fire !";
    case ESurprise::TimeWarp: return "Time Warp !";
    case ESurprise::ReloadWeapons: return "All Weapons Available !!!";
    case ESurprise::Invulnerability: return "Invulnerability !";
    case ESurprise::TripleFire: return "Triple Fire !!!";
    default: return "";
    }
}

// FUN_0041d180: the 1-based animation of surprise.gaf that shows the surprise, or 0 for none.
uint32_t SurpriseIcon(const ESurprise Type)
{
    switch (Type)
    {
    case ESurprise::None:
    case ESurprise::Random:
    case ESurprise::Nop:
        return 0;
    case ESurprise::DestroyAround:
    case ESurprise::TouchAndDestroy:
    case ESurprise::AutoShieldUp:
    case ESurprise::Invisible:
    case ESurprise::CircularFire:
    case ESurprise::TimeWarp:
    case ESurprise::ReloadWeapons:
    case ESurprise::Invulnerability:
    case ESurprise::TripleFire:
        return uint32_t(Type);
    default:
        // The maluses take the icon before their own number.
        return uint32_t(Type) - 1;
    }
}

// surprise::FUN_0041ca90: undoes the surprise in effect when it is of this type.
void EndSurprise(FSurpriseState& State, const ESurprise Type, FPlayerState& Player, FMovementClass& Class, FPlayerInventory& Inventory)
{
    if (State.Active != Type)
    {
        return;
    }

    switch (Type)
    {
    case ESurprise::ReverseCommands:
        State.bReversed = false;
        break;
    case ESurprise::WeaponJam:
        State.JammedWeapon.reset();
        break;
    case ESurprise::Extasy:
    case ESurprise::LowSpeed:
        Class.AccelerationStep = State.AccelerationStep;
        Class.DecelerationStep = State.DecelerationStep;
        break;
    case ESurprise::Pinball:
        Class.Elasticity = State.Elasticity;
        break;
    case ESurprise::Banana:
        Class.Friction = State.Friction;
        break;
    case ESurprise::BlockedMotor:
        State.bBlockedMotor = false;
        break;
    case ESurprise::Invisible:
        // The drone is back on its body.
        Player.Position = Player.PreviousPosition = State.Body;
        Player.Center = CollisionFromWorld(State.Body);
        Player.bInvisible = false;
        break;
    case ESurprise::CircularFire:
        State.bCircularFire = false;
        break;
    case ESurprise::ReloadWeapons:
        for (size_t Index = 0; Index < Inventory.Ammo.size() && Index < State.SavedAmmo.size(); ++Index)
        {
            Inventory.Ammo[Index].Count = State.SavedAmmo[Index];
        }

        State.SavedAmmo.clear();
        break;
    case ESurprise::Invulnerability:
        State.bInvulnerable = false;
        break;
    case ESurprise::TripleFire:
        State.bTripleFire = false;
        break;
    default:
        break;
    }

    State.Active = ESurprise::None;
}
}

std::vector<FPickup> CreatePickups(const FLevel& Level)
{
    std::vector<FPickup> Pickups;
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        if (std::optional<FPickup> Pickup = CreatePickup(Level, Index))
        {
            Pickups.push_back(std::move(*Pickup));
        }
    }

    return Pickups;
}

std::optional<FPickup> CreatePickup(const FLevel& Level, const size_t Index)
{
    const FLevelObject& Object = Level.Objects[Index];
    const FLevelPropertyBlock* const Item = IsPickup(Object) ? FindItemBlock(Object) : nullptr;
    if (!Item)
    {
        return std::nullopt;
    }

    {
        const auto Number = [Item](const std::string_view Field) { return ParseLevelFloat(Item->Find(Field)).value_or(0.f); };
        const FLevelPropertyBlock* const Key = FindTypeBlock(Object.Properties, "Key");
        const auto KeyNumber = [Key](const std::string_view Field) { return int(ParseLevelFloat(Key->Find(Field)).value_or(0.f)); };
        return FPickup{
            .ObjectIndex = Index,
            .Circle = { .Center = CollisionFromWorld({ .X = Object.X, .Y = Object.Y }), .Radius = Object.BoundingBoxSize > 0 ? float(Object.BoundingBoxSize) : DefaultCollisionRadius },
            .AddEnergy = Number("Add-NRJ"),
            .AddShield = Number("Add-Shield"),
            .AddMoney = Number("Add-Money"),
            .AmmoType = std::string(Item->Find("Ammo-Type")),
            .AddAmmo = Number("Add-Ammo"),
            .Key = Key ? std::optional(FCarriedKey{ .Id = KeyNumber("Id"), .bCode = KeyNumber("Is-code") != 0, .PartId = KeyNumber("Code-part-id"),
                                                    .PartCount = KeyNumber("Code-part-count") })
                       : std::nullopt,
            .Surprise = SurpriseFromName(Item->Find("Surprise-type")),
        };
    }
}

FPlayerInventory CreateInventory(const FLevelObject& Player)
{
    const FLevelPropertyBlock& Properties = Player.Properties;
    const auto Value = [&](const std::string_view Kind, const std::string_view Field)
    {
        const FLevelPropertyBlock* const Block = FindBlock(Properties, Kind);
        return Block ? ParseLevelFloat(Block->Find(Field)).value_or(0.f) : 0.f;
    };
    FPlayerInventory Inventory{
        .Energy = Value("Energy", "Initial"),
        .EnergyMax = Value("Energy", "Max"),
        .Shield = Value("Shield", "Initial"),
        .ShieldMax = Value("Shield", "Max"),
    };

    // .3DO instances can override the starting Energy and Shield with plain fields.
    if (const std::optional<float> Energy = ParseLevelFloat(Properties.Find("Energy")))
    {
        Inventory.Energy = *Energy;
    }

    if (const std::optional<float> Shield = ParseLevelFloat(Properties.Find("Shield")))
    {
        Inventory.Shield = *Shield;
    }

    for (const FLevelPropertyBlock& Child : Properties.Children)
    {
        if (Child.Kind == "ADD" && Child.Name == "Ammo")
        {
            Inventory.Ammo.push_back({
                .Name = std::string(Child.Find("Name")),
                .Count = ParseLevelFloat(Child.Find("Count")).value_or(0.f),
                .Max = ParseLevelFloat(Child.Find("Max")).value_or(0.f),
            });
        }
    }

    return Inventory;
}

namespace
{
bool CarriesKey(const std::vector<FCarriedKey>& Keys, int Id);

// The view size at session +0xf84 and +0xf88, from which the screen messages are placed.
constexpr int32_t MessageViewWidth = 640;
constexpr int32_t MessageViewHeight = 480;

// The floating texts of the item pickup branch: world messages that rise from the item and type themselves out.
void AddPickupMessage(FMessages& Messages, const FLevelObject& Item, const int32_t OffsetY, std::string Text, const uint32_t Diffuse)
{
    constexpr int32_t TextX = -0x20;
    constexpr float RiseVelocity = -1.f;
    constexpr uint32_t LifeMilliseconds = 2000;
    constexpr uint32_t Font = 6;
    Messages.Add({
        .Text = std::move(Text),
        .X = float(TruncateLow(Item.X) + TextX),
        .Y = float(TruncateLow(Item.Y) + OffsetY),
        .VelocityY = RiseVelocity,
        .LifeMilliseconds = LifeMilliseconds,
        .bWorld = true,
        .bTypewriter = true,
        .Font = Font,
        .Color = { .Diffuse = Diffuse },
    }, EMessagePlacement::Replace);
}

// The screen messages of keys and gates, centred 100 pixels up and left of the view's middle.
void AddCentreMessage(FMessages& Messages, std::string Text, const uint32_t Diffuse)
{
    constexpr int32_t CentreOffset = -100;
    constexpr uint32_t LifeMilliseconds = 2000;
    constexpr uint32_t Font = 6;
    Messages.Add({
        .Text = std::move(Text),
        .X = float((MessageViewWidth >> 1) + CentreOffset),
        .Y = float((MessageViewHeight >> 1) + CentreOffset),
        .LifeMilliseconds = LifeMilliseconds,
        .Font = Font,
        .Color = { .Diffuse = Diffuse },
    }, EMessagePlacement::Replace);
}
}


// FUN_0041cf40 for a newly collected surprise: the surprise in effect ends, the new one waits half a second, and its text and blinking icon
// appear for as long as it lasts.
namespace
{
void CollectSurprise(FCombat& Combat, FPlayerInventory& Inventory, ESurprise Type, FMessages& Messages)
{
    FSurpriseState& State = Inventory.Surprises;
    std::erase_if(State.Carried, [&](const FCarriedSurprise& Carried)
    {
        if (Carried.Type != State.Active)
        {
            return false;
        }

        Messages.Remove(Carried.IconMessage);
        return true;
    });

    if (Type == ESurprise::Random)
    {
        Type = DrawSurprise(Combat.Random);
    }

    // FUN_0041bfb0 gives every surprise a 500 ms delay.
    constexpr int32_t StartDelay = 500;
    constexpr int32_t ShortTimeout = 2;
    constexpr int32_t ShortTimeoutShown = 3000;
    constexpr int32_t MinShown = 5000;
    constexpr int32_t TextOffset = -0x84;
    constexpr uint32_t TextColor = 0xff2010;
    constexpr uint32_t Font = 6;
    constexpr int32_t IconX = -0x50;
    constexpr int32_t IconY = -0x36;
    constexpr int32_t IconBlinkPeriod = 10;
    const int32_t Timeout = SurpriseTimeout(Type);
    const int32_t Shown = std::max(MinShown, (Timeout < ShortTimeout ? ShortTimeoutShown : Timeout) + StartDelay);
    Messages.Add({
        .Text = std::string(SurpriseText(Type)),
        .X = float((MessageViewWidth >> 1) + TextOffset),
        .Y = float((MessageViewHeight >> 1) + TextOffset),
        .LifeMilliseconds = uint32_t(Shown),
        .Font = Font,
        .Color = { .Diffuse = TextColor },
    }, EMessagePlacement::Replace);

    FCarriedSurprise Carried{ .Type = Type, .Delay = StartDelay, .Timeout = Timeout };
    if (const uint32_t Icon = SurpriseIcon(Type); Icon != 0 && Icon <= Combat.SurpriseIcons.size())
    {
        Carried.IconMessage = Messages.Add({
            .X = float(MessageViewWidth + IconX),
            .Y = float(MessageViewHeight + IconY),
            .LifeMilliseconds = uint32_t(Shown),
            .Gaf = EMessageGaf::Surprise,
            .Animation = Combat.SurpriseIcons[Icon - 1],
            .BlinkPeriod = IconBlinkPeriod,
            .BlinkTimer = IconBlinkPeriod,
            .bFade = true,
        }, EMessagePlacement::Always);
    }

    State.Carried.insert(State.Carried.begin(), Carried);
}
}

size_t CollectPickups(const FPlayerState& Player, const FMovementClass& Class, FPlayerInventory& Inventory, std::vector<FPickup>& Pickups, FLevel& Level,
    FMessages& Messages, FCombat* const Combat)
{
    // Energy is only topped up below 95% of its maximum; shield below its maximum; money always.
    constexpr float EnergyRefillLimit = 0.95f;
    constexpr uint32_t EnergyColor = 0x80ff20;
    constexpr uint32_t MoneyColor = 0xffc040;
    constexpr uint32_t ShieldColor = 0xff4020;
    constexpr uint32_t KeyColor = 0x00ff00;
    constexpr uint32_t PlainColor = 0xffffff;
    // Each further text of the same item sits 20 pixels higher.
    constexpr int32_t LineStep = -0x14;
    size_t Collected = 0;
    for (FPickup& Pickup : Pickups)
    {
        FLevelObject& Object = Level.Objects[Pickup.ObjectIndex];
        if (Object.bRemoved || !IntersectCircles({ .Center = Player.Center, .Radius = Class.Radius }, Pickup.Circle))
        {
            continue;
        }

        bool bUsed = false;
        int32_t OffsetY = 0;
        if (Pickup.AddEnergy != 0.f && Inventory.Energy < Inventory.EnergyMax * EnergyRefillLimit)
        {
            Inventory.Energy = std::min(Inventory.EnergyMax, Inventory.Energy + Pickup.AddEnergy);
            AddPickupMessage(Messages, Object, OffsetY, std::format("Energy +{}", int32_t(Pickup.AddEnergy)), EnergyColor);
            bUsed = true;
        }

        if (Pickup.AddMoney != 0.f)
        {
            Inventory.Money += Pickup.AddMoney;
            AddPickupMessage(Messages, Object, OffsetY, std::format("Money +{}", int32_t(Pickup.AddMoney)), MoneyColor);
            bUsed = true;
        }

        if (Pickup.AddShield != 0.f && Inventory.Shield < Inventory.ShieldMax)
        {
            OffsetY = bUsed ? LineStep : OffsetY;
            Inventory.Shield = std::min(Inventory.ShieldMax, Inventory.Shield + Pickup.AddShield);
            AddPickupMessage(Messages, Object, OffsetY, std::format("Shield +{}", int32_t(Pickup.AddShield)), ShieldColor);
            bUsed = true;
        }

        // ponytail: AddAmmo (00444540) is not traced yet; ammo fills up to the stock's maximum and counts as used when anything was added.
        if (!Pickup.AmmoType.empty() && Pickup.AddAmmo != 0.f)
        {
            const auto Stock = std::ranges::find(Inventory.Ammo, Pickup.AmmoType, &FAmmoStock::Name);
            if (Stock != Inventory.Ammo.end() && Stock->Count < Stock->Max)
            {
                Stock->Count = std::min(Stock->Max, Stock->Count + Pickup.AddAmmo);
                OffsetY += bUsed ? LineStep : 0;
                AddPickupMessage(Messages, Object, OffsetY, std::format("{} +{}", Pickup.AmmoType, int32_t(Pickup.AddAmmo)), PlainColor);
                bUsed = true;
            }
        }

        // ConsumeItem at 00444660 carries an item without an action, which is what keys and code parts are.
        if (Pickup.Key)
        {
            const FCarriedKey& Key = *Pickup.Key;
            Inventory.Keys.push_back(Key);
            if (Key.Id != 0)
            {
                const std::string Text = !Key.bCode                ? std::format("Key #{} !", Key.Id)
                                         : CarriesKey(Inventory.Keys, Key.Id) ? std::format("Code #{} complete !", Key.Id)
                                                                              : std::format("Code #{} : part {} !", Key.Id, Key.PartId);
                AddCentreMessage(Messages, Text, KeyColor);
            }

            bUsed = true;
        }

        // The update leaves a surprise lying while the player carries one; ConsumeItem hands it to FUN_0041cf40.
        if (Pickup.Surprise != ESurprise::None && (!Combat || !Inventory.Surprises.Carried.empty()))
        {
            continue;
        }

        if (Pickup.Surprise != ESurprise::None)
        {
            CollectSurprise(*Combat, Inventory, Pickup.Surprise, Messages);
            bUsed = true;
        }

        if (bUsed)
        {
            Object.bRemoved = true;
            ++Collected;
        }
    }

    return Collected;
}

namespace
{
// ApplyDamage at 0043ca30: the shield absorbs first and is reset once it breaks; whatever is left comes off the energy.
// ponytail: the shield resets to a class field at +0x6c that is not decoded yet; zero until then.
void ApplyDamage(float& Energy, float& Shield, float Amount)
{
    if (0.f < Shield)
    {
        Shield = float(double(Shield) - double(Amount));
        if (0.f <= Shield)
        {
            return;
        }

        Amount = -Shield;
        Shield = 0.f;
    }

    Energy = float(double(Energy) - double(Amount));
}

void ApplyDamage(FObjectBody& Body, const float Amount)
{
    if (Body.bIndestructible)
    {
        return;
    }

    Body.bDamaged = true;
    ApplyDamage(Body.Energy, Body.Shield, Amount);
}

struct FWorldCell
{
    int X = 0;
    int Y = 0;
};

// The 64 x 32 world-pixel grid cell an object is linked into.
FWorldCell CellOf(const FCollisionPoint World)
{
    return { .X = int(uint32_t(int64_t(World.X)) / BlockWidth), .Y = int(uint32_t(int64_t(World.Y)) / BlockHeight) };
}

static_assert(TruncateLow(-2.7) == -2);
static_assert(TruncateLow(std::numeric_limits<double>::quiet_NaN()) == 0);
static_assert(TruncateLow(4294967298.5) == 2);

enum class EBobMode : int8_t
{
    // Wind back to the trough: a static object without Vertical-bounce-when-static.
    Settle = -1,
    Play = 0,
    // Rise to the peak and hold there: a moving object with Vertical-bounce-when-moving.
    Rise = 1,
};

// ObjectSelectBobMode at 00433f40; it would also play the bob of ammo and FX kinds, which no placed object has.
std::optional<EBobMode> BobMode(const FObjectBob& Bob, const float Speed)
{
    if (Bob.Period == 0.f || Bob.Amplitude == 0.f)
    {
        return std::nullopt;
    }

    if (Speed == 0.f)
    {
        return Bob.bWhenStatic ? EBobMode::Play : EBobMode::Settle;
    }

    return Bob.bWhenMoving ? std::optional(EBobMode::Rise) : std::nullopt;
}

// ObjectPlayBob at 0043c090: the altitude the bob shows, or nullopt once a rising or settling bob has stopped. The stop test leaves out the phase.
std::optional<int> PlayBob(FObjectBob& Bob, const EBobMode Mode, const int32_t StepMilliseconds)
{
    constexpr double QuarterTurn = std::numbers::pi_v<float> / 2.f;
    const double Angle = double(Bob.Timer) / double(Bob.Period);
    const int Altitude = TruncateLow(std::sin(Angle + double(Bob.Phase)) * double(Bob.Amplitude)) + Bob.Average;
    if (Mode == EBobMode::Play || (Mode == EBobMode::Rise && Angle < QuarterTurn))
    {
        Bob.Timer += StepMilliseconds;
        return Altitude;
    }

    if (Mode == EBobMode::Settle && Angle > -QuarterTurn)
    {
        Bob.Timer -= StepMilliseconds;
        return Altitude;
    }

    return std::nullopt;
}

std::vector<FObjectBody*> BodiesByObject(const FLevel& Level, const std::span<FObjectBody> Bodies)
{
    std::vector<FObjectBody*> Result(Level.Objects.size(), nullptr);
    for (FObjectBody& Body : Bodies)
    {
        Result[Body.ObjectIndex] = &Body;
    }

    return Result;
}

// EngineActivateObjects (0043c700) visits the grid from its last row and column back to the first, and each cell lists its newest object first.
// ObjectActivate (0043c150) gives every object one draw for its bob phase and shows the bob's first altitude, even without a bob.
// ponytail: objects that a generator places at load time are not created, so they draw nothing here.
std::vector<FObjectBob> ActivateObjects(FLevel& Level, const std::span<FObjectBody> Bodies, FRandom& Random)
{
    std::vector<size_t> Order(Level.Objects.size());
    std::iota(Order.begin(), Order.end(), size_t{ 0 });
    std::ranges::sort(Order, std::ranges::greater{}, [&](const size_t Index)
    {
        const FWorldCell Cell = CellOf({ .X = Level.Objects[Index].X, .Y = Level.Objects[Index].Y });
        return std::tuple(Cell.Y, Cell.X, Index);
    });

    const std::vector<FObjectBody*> BodyOf = BodiesByObject(Level, Bodies);
    std::vector<FObjectBob> Bobs(Level.Objects.size());
    for (const size_t Index : Order)
    {
        FLevelObject& Object = Level.Objects[Index];
        const auto Number = [&Object](const std::string_view Field) { return ParseLevelFloat(Object.Properties.Find(Field)).value_or(0.f); };
        const int Average = int(Number("Bounce-average-altitude"));
        FObjectBob& Bob = Bobs[Index];
        Bob = {
            .Period = Number("Bounce-period"),
            .Amplitude = Number("Bounce-amplitude"),
            .Average = Average != 0 ? Average : int(Object.Altitude),
            .bWhenStatic = Number("Vertical-bounce-when-static") != 0.f,
            .bWhenMoving = Number("Vertical-bounce-when-moving") != 0.f,
        };
        constexpr int PhaseDivisorMax = 256;
        constexpr float FullTurn = 2.f * std::numbers::pi_v<float>;
        // A zero draw makes the phase infinite, so that object's bob stays at its average altitude.
        const int Divisor = Random.Range(0, PhaseDivisorMax);
        Bob.Phase = Divisor == 0 ? std::numeric_limits<float>::infinity() : float(double(FullTurn) / double(Divisor));
        Object.Altitude = float(*PlayBob(Bob, EBobMode::Play, 0));
        if (BodyOf[Index])
        {
            BodyOf[Index]->Altitude = Object.Altitude;
        }
    }

    return Bobs;
}

// ObjectCarriesKey at 0043dcb0: the newest carried key with the id decides; a code also needs all of its parts (ObjectFindCompleteCode at 00439270), counted as the first part
// says and including part 1.
bool CarriesKey(const std::vector<FCarriedKey>& Keys, const int Id)
{
    const auto Newest = std::ranges::find(Keys.rbegin(), Keys.rend(), Id, &FCarriedKey::Id);
    if (Newest == Keys.rend() || !Newest->bCode)
    {
        return Newest != Keys.rend();
    }

    int PartCount = 0;
    int Parts = 0;
    bool bFirstPart = false;
    for (const FCarriedKey& Key : Keys | std::views::reverse)
    {
        if (Key.Id != Id || !Key.bCode)
        {
            continue;
        }

        // A part whose count disagrees with the first is reported and left out.
        if (PartCount == 0)
        {
            PartCount = Key.PartCount;
        }
        else if (PartCount != Key.PartCount)
        {
            continue;
        }

        bFirstPart = bFirstPart || Key.PartId == 1;
        ++Parts;
    }

    return bFirstPart && Parts == PartCount;
}

enum class EHolder : uint8_t
{
    None,
    Other,
    Player,
};

// Objects that never open a gate or call a lift: ammo, FX, items, lifts, gates and things (kinds 0x10 to 0x15).
constexpr std::array<std::string_view, 6> NonSensingKinds = { "ammo", "fx", "item", "lift", "gate", "thing" };

FCollisionPoint ObjectPosition(const FLevel& Level, const std::vector<FObjectBody*>& BodyOf, const FPlayerTarget& Player, const size_t Index)
{
    if (Index == Player.ObjectIndex)
    {
        return Player.State.Position;
    }

    return BodyOf[Index] ? BodyOf[Index]->State.Position : FCollisionPoint{ .X = Level.Objects[Index].X, .Y = Level.Objects[Index].Y };
}

// DoorFindHolder at 00443430: something that may pass within two cells of the door. A key or code can only be carried by the player.
// ponytail: invisibility is not ported, so invisible objects are not left out.
EHolder FindHolder(const FLevel& Level, const std::vector<FObjectBody*>& BodyOf, const FPlayerTarget& Player, const FWorldCell Door, const int Id)
{
    constexpr int SensorCells = 2;
    const auto Near = [&](const FCollisionPoint Position)
    {
        const FWorldCell Cell = CellOf(Position);
        return std::abs(Cell.X - Door.X) <= SensorCells && std::abs(Cell.Y - Door.Y) <= SensorCells;
    };

    if (Id != 0)
    {
        return !Player.State.bInvisible && Near(Player.State.Position) && CarriesKey(Player.Inventory.Keys, Id) ? EHolder::Player : EHolder::None;
    }

    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        if (!Object.bRemoved && !std::ranges::contains(NonSensingKinds, Object.SubType) && !(Index == Player.ObjectIndex && Player.State.bInvisible) &&
            Near(ObjectPosition(Level, BodyOf, Player, Index)))
        {
            return Index == Player.ObjectIndex ? EHolder::Player : EHolder::Other;
        }
    }

    return EHolder::None;
}

struct FBox
{
    float MinX = 0.f;
    float MinY = 0.f;
    float MaxX = 0.f;
    float MaxY = 0.f;
};

// BoundsUpdateBox at 00405cb0: a circle's box in collision space; objects without a bounding box are a point.
FBox CircleBox(const FCollisionPoint Center, const float Radius)
{
    return { .MinX = Center.X - Radius, .MinY = Center.Y - Radius, .MaxX = Center.X + Radius, .MaxY = Center.Y + Radius };
}

// ObjectOverlapsSolid at 00405df0: whether a solid object in the body's cell or the cells around it overlaps the body's box. Lying items are left
// out.
bool ObjectOverlapsSolid(const FLevel& Level, const std::vector<FObjectBody*>& BodyOf, const FPlayerTarget& Player, const FObjectBody& Door)
{
    const FWorldCell DoorCell = CellOf(Door.State.Position);
    const FBox DoorBox = CircleBox(Door.State.Center, Door.Class.Radius);
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        const FObjectBody* const Body = BodyOf[Index];
        if (Index == Door.ObjectIndex || Object.bRemoved || (Body && Body->bPassable) || (Object.SubType == "item" && FindItemBlock(Object)))
        {
            continue;
        }

        const FCollisionPoint Position = ObjectPosition(Level, BodyOf, Player, Index);
        const FWorldCell Cell = CellOf(Position);
        if (std::abs(Cell.X - DoorCell.X) > 1 || std::abs(Cell.Y - DoorCell.Y) > 1)
        {
            continue;
        }

        const FBox Box = Index == Player.ObjectIndex ? CircleBox(Player.State.Center, Player.Radius)
                         : Body                       ? CircleBox(Body->State.Center, Body->Class.Radius)
                                                      : CircleBox(CollisionFromWorld(Position), 0.f);
        if (DoorBox.MinX < Box.MaxX && Box.MinX < DoorBox.MaxX && DoorBox.MinY < Box.MaxY && Box.MinY < DoorBox.MaxY)
        {
            return true;
        }
    }

    return false;
}

// The key part of the gate and lift updates: Destroy-key takes every key or code part with the id from the holder (ObjectDestroyKeys at 0043dd20),
// which DoorAnnounceKeyUse (00443500) announces when it is the player, and without Key-always-needed the door forgets its id.
void UseKey(FDoor& Door, FPlayerInventory& Inventory, const EHolder Holder, FMessages& Messages)
{
    constexpr uint32_t PlainColor = 0xffffff;
    if ((Door.Flags & DoorDestroyKey) != 0 && Holder == EHolder::Player && std::erase_if(Inventory.Keys, [&](const FCarriedKey& Key) { return Key.Id == Door.Id; }) != 0)
    {
        AddCentreMessage(Messages, std::format("{} #{} !", (Door.Flags & DoorNeedsCode) != 0 ? "Using Code" : "Using Key", Door.Id), PlainColor);
    }

    if ((Door.Flags & DoorKeyAlwaysNeeded) == 0)
    {
        Door.Id = 0;
    }
}

// The update copies a fresh instance of the animation and shows its third frame.
void StartDoorAnimation(FDoor& Door, const size_t Animation)
{
    constexpr uint32_t FirstMovingFrame = 3;
    Door.Animation = Animation;
    Door.Playback = StartAnimation(Door.Animations[Animation]);
    ShowFrame(Door.Playback, Door.Animations[Animation], FirstMovingFrame);
}

constexpr size_t OpenAnimation = 0;
constexpr size_t ClosedAnimation = 1;

// GateUpdate at 00443620.
void StepGate(FDoor& Door, FObjectBody& Body, const FLevel& Level, const std::vector<FObjectBody*>& BodyOf, const FPlayerTarget& Player, const int32_t Step,
    FMessages& Messages)
{
    const FWorldCell Cell = CellOf(Body.State.Position);
    if ((Door.Flags & DoorOpen) == 0)
    {
        if ((Door.Flags & DoorOpening) != 0)
        {
            Body.bPassable = true;
            Door.Flags |= DoorOpen;
            Door.Timer = Step < Door.Timer ? Door.Timer - Step : 0;
            return;
        }

        const EHolder Holder = (Door.Flags & DoorSensor) != 0 ? FindHolder(Level, BodyOf, Player, Cell, Door.Id) : EHolder::None;
        if ((Door.Flags & DoorSensor) == 0 || Holder != EHolder::None)
        {
            Door.Timer = 0;
            Door.Flags &= ~DoorStayClosed;
            if (Holder != EHolder::None && Door.Id != 0)
            {
                UseKey(Door, Player.Inventory, Holder, Messages);
            }
        }
        else
        {
            Door.Flags |= DoorStayClosed;
        }

        if ((Door.Flags & DoorStayClosed) == 0)
        {
            if (Step < Door.Timer)
            {
                Door.Timer -= Step;
                return;
            }

            Door.Timer = Door.Delay;
            StartDoorAnimation(Door, OpenAnimation);
            Door.Flags |= DoorOpening;
        }

        return;
    }

    Body.bPassable = true;
    if (Step < Door.Timer)
    {
        Door.Timer -= Step;
        return;
    }

    if (const EHolder Holder = FindHolder(Level, BodyOf, Player, Cell, Door.Id); Holder != EHolder::None)
    {
        if (Door.Id != 0)
        {
            UseKey(Door, Player.Inventory, Holder, Messages);
        }

        return;
    }

    if (ObjectOverlapsSolid(Level, BodyOf, Player, Body) || (Door.Flags & DoorStayOpen) != 0)
    {
        return;
    }

    Door.Flags &= ~(DoorOpen | DoorOpening);
    Body.bPassable = false;
    Door.Timer = Door.Delay;
    StartDoorAnimation(Door, ClosedAnimation);
}

// ObjectUpdateLevelExit at 00442bf0. A lift leaves when nothing stands on it and comes back after its delay; the player rides it once fully
// inside its box, which returns true. A negative Reward is its fare.
bool StepLift(FDoor& Door, FObjectBody& Body, const FLevel& Level, const std::vector<FObjectBody*>& BodyOf, const FPlayerTarget& Player,
    const int32_t Step, FMessages& Messages)
{
    const FWorldCell Cell = CellOf(Body.State.Position);
    if ((Door.Flags & DoorOpen) == 0)
    {
        if ((Door.Flags & DoorOpening) != 0)
        {
            const EHolder Holder = Door.Id == 0 ? EHolder::None : FindHolder(Level, BodyOf, Player, Cell, Door.Id);
            if (Door.Id == 0 || Holder != EHolder::None)
            {
                Body.bPassable = true;
                if (Door.Id != 0)
                {
                    UseKey(Door, Player.Inventory, Holder, Messages);
                }
            }

            Door.Flags |= DoorOpen;
            Door.Timer = Step < Door.Timer ? Door.Timer - Step : 0;
            return false;
        }

        if ((Door.Flags & DoorStayClosed) != 0)
        {
            return false;
        }

        if (Step < Door.Timer)
        {
            Door.Timer -= Step;
            return false;
        }

        Door.Timer = Door.Delay;
        StartDoorAnimation(Door, OpenAnimation);
        Door.Flags |= DoorOpening;
        return false;
    }

    const EHolder Holder = Door.Id == 0 ? EHolder::None : FindHolder(Level, BodyOf, Player, Cell, Door.Id);
    const bool bAccess = Door.Id == 0 || Holder != EHolder::None;
    if (bAccess)
    {
        Body.bPassable = true;
        if (Door.Id != 0)
        {
            UseKey(Door, Player.Inventory, Holder, Messages);
        }
    }

    if (Step < Door.Timer)
    {
        Door.Timer -= Step;
        return false;
    }

    if (Door.UsageCount < 1)
    {
        return false;
    }

    if (!ObjectOverlapsSolid(Level, BodyOf, Player, Body))
    {
        if ((Door.Flags & DoorStayOpen) == 0)
        {
            Door.Flags &= ~(DoorOpen | DoorOpening);
            Body.bPassable = false;
            Door.Timer = Door.Delay;
            StartDoorAnimation(Door, ClosedAnimation);
        }

        return false;
    }

    const FBox Lift = CircleBox(Body.State.Center, Body.Class.Radius);
    const FBox Rider = CircleBox(Player.State.Center, Player.Radius);
    // ObjectUpdateLevelExit leaves an invisible player behind.
    if (!bAccess || Player.State.bInvisible || Rider.MinX < Lift.MinX || Lift.MaxX < Rider.MaxX || Rider.MinY < Lift.MinY || Lift.MaxY < Rider.MaxY)
    {
        return false;
    }

    if (Body.Reward < 0.f)
    {
        if (Player.Inventory.Money < -Body.Reward)
        {
            return false;
        }

        Player.Inventory.Money += Body.Reward;
    }

    --Door.UsageCount;
    return true;
}

// AreaDamage at 0043cb20: every live object near the blast takes damage falling off linearly over Damage-zone collision units.
void AreaDamage(const FCollisionPoint Center, const FWorldCell Cell, const float Damage, const float Zone, std::vector<FObjectBody>& Bodies,
    const FPlayerTarget& Player)
{
    if (Damage == 0.f || Zone == 0.f)
    {
        return;
    }

    // Only the grid cells within the zone's extent are scanned.
    const int Radius = int(Zone);
    const int CellRadiusX = int(uint32_t(Radius) / BlockWidth) + 1;
    const int CellRadiusY = int(uint32_t(Radius) / BlockHeight) + 1;
    const auto Falloff = [&](const FCollisionPoint Position, const FCollisionPoint ObjectCenter) -> std::optional<float>
    {
        const FWorldCell ObjectCell = CellOf(Position);
        if (std::abs(ObjectCell.X - Cell.X) > CellRadiusX || std::abs(ObjectCell.Y - Cell.Y) > CellRadiusY)
        {
            return std::nullopt;
        }

        const double DeltaX = double(ObjectCenter.X) - double(Center.X);
        const double DeltaY = double(ObjectCenter.Y) - double(Center.Y);
        const int DistanceSquared = int(DeltaX * DeltaX + DeltaY * DeltaY);
        if (DistanceSquared >= Radius * Radius)
        {
            return std::nullopt;
        }

        const int Distance = int(std::sqrt(double(DistanceSquared)));
        return DistanceSquared == 0 ? Damage : float(double(Radius - Distance) / double(Radius) * double(Damage));
    };

    for (FObjectBody& Body : Bodies)
    {
        if (0.f < Body.Energy)
        {
            if (const std::optional<float> Amount = Falloff(Body.State.Position, Body.State.Center))
            {
                ApplyDamage(Body, *Amount);
            }
        }
    }

    if (0.f < Player.Inventory.Energy)
    {
        if (const std::optional<float> Amount = Falloff(Player.State.Position, Player.State.Center))
        {
            if (!Player.State.bInvisible && !Player.Inventory.Surprises.bInvulnerable)
            {
                ApplyDamage(Player.Inventory.Energy, Player.Inventory.Shield, *Amount);
            }
        }
    }
}

FObjectBody* FindStruckBody(std::vector<FObjectBody>& Bodies, const FCollisionCircle Shot, const size_t Owner, std::optional<FCollisionPoint>& Contact)
{
    FObjectBody* Struck = nullptr;
    for (FObjectBody& Body : Bodies)
    {
        if (Body.ObjectIndex == Owner || Body.bPassable)
        {
            continue;
        }

        const std::optional<FCollisionPoint> BodyContact = IntersectCircles(Shot, { .Center = Body.State.Center, .Radius = Body.Class.Radius });
        if (BodyContact && (!Contact || PrefersContact(*Contact, *BodyContact, Shot.Center)))
        {
            Contact = BodyContact;
            Struck = &Body;
        }
    }

    return Struck;
}

// AmmoImpact at 0043d350 spawns this when a shot sinks into the ground.
constexpr std::string_view GroundImpactEffect = "bullet-ground-impact";
// An exploding object's blast goes off this long after its Boom-fx appears, or at half its animation if that comes first.
constexpr int32_t BlastDelayMilliseconds = 500;

// UpdateAnimation at 0045e7e0: each frame is held for its duration; a one-shot animation ends after its last frame.
void AdvanceEffect(FEffect& Effect, const FEffectClass& Class, const uint32_t StepMilliseconds)
{
    if (Effect.bFinished)
    {
        return;
    }

    const uint32_t FrameCount = uint32_t(Class.FrameDurations.size());
    Effect.FrameTimer -= int32_t(StepMilliseconds);
    // A looping animation of zero-length frames would never leave this loop; one pass through it is enough.
    for (uint32_t Advanced = 0; Effect.FrameTimer <= 0 && Advanced <= FrameCount; ++Advanced)
    {
        ++Effect.Frame;
        if (Effect.Frame > FrameCount)
        {
            Effect.Frame = Class.bLoop ? Class.LoopStart : 0;
        }

        if (Effect.Frame == 0 || Effect.Frame > FrameCount)
        {
            Effect.Frame = 0;
            Effect.bFinished = true;
            return;
        }

        Effect.FrameTimer += Class.FrameDurations[Effect.Frame - 1] * AnimationPeriodMilliseconds;
    }
}

void SpawnEffect(FCombat& Combat, const std::string_view ClassName, const FCollisionPoint Position, const float Altitude, const float Damage = 0.f,
    const float Zone = 0.f)
{
    const auto Class = std::ranges::find(Combat.EffectClasses, ClassName, &FEffectClass::Name);
    if (Class == Combat.EffectClasses.end())
    {
        return;
    }

    FEffect Effect{
        .ClassIndex = size_t(Class - Combat.EffectClasses.begin()),
        .Position = Position,
        .Altitude = Altitude,
        .Damage = Damage,
        .Zone = Zone,
        .BlastTimer = BlastDelayMilliseconds,
    };
    // ObjectInitialize shows the first frame straight away.
    AdvanceEffect(Effect, *Class, 0);
    Combat.Effects.push_back(Effect);
}

// Kinds whose destruction pays the class Reward (mask 0x1ffc0 in EngineUpdateObjects at 004406e0).
bool PaysReward(const std::string_view SubType)
{
    constexpr std::array<std::string_view, 11> RewardKinds = { "cop", "staticgen", "vyruz", "walker", "burner", "jeep", "roller", "heli", "turret", "spider", "tank" };
    return std::ranges::contains(RewardKinds, SubType);
}

// LoadObjectClassLibrary stores slopes as degrees * pi * (1 / 180), both float constants (0050953c, 00509538).
float SlopeRadians(const float Degrees)
{
    constexpr float InverseHalfTurn = 1.f / 180.f;
    return float(double(float(double(Degrees) * double(Pi))) * double(InverseHalfTurn));
}

float ClassNumber(const FLevelPropertyBlock& Block, const std::string_view Field)
{
    return ParseLevelFloat(Block.Find(Field)).value_or(0.f);
}

const FLevelPropertyBlock* FindClass(const FLevel& Level, const std::string_view Name)
{
    const auto Found = Level.Classes.find(ToLowerName(Name));
    return Found == Level.Classes.end() ? nullptr : &Found->second;
}

// Adds a weapon class and its ammo class to the combat tables once; returns the weapon's index.
std::expected<size_t, std::string> LoadWeapon(FCombat& Combat, const FLevel& Level, const std::string_view Name)
{
    const std::string LowerName = ToLowerName(Name);
    if (const auto Known = std::ranges::find(Combat.Weapons, LowerName, &FWeaponClass::Name); Known != Combat.Weapons.end())
    {
        return size_t(Known - Combat.Weapons.begin());
    }

    const FLevelPropertyBlock* const Class = FindClass(Level, Name);
    const FLevelPropertyBlock* const Block = Class ? FindTypeBlock(*Class, "Weapon") : nullptr;
    if (!Block)
    {
        return std::unexpected(std::format("unknown weapon class '{}'", Name));
    }

    const std::string AmmoName = ToLowerName(Block->Find("Ammo"));
    if (!std::ranges::contains(Combat.AmmoClasses, AmmoName, &FAmmoClass::Name))
    {
        const FLevelPropertyBlock* const Ammo = FindClass(Level, AmmoName);
        const FLevelPropertyBlock* const AmmoBlock = Ammo ? FindTypeBlock(*Ammo, "Ammo") : nullptr;
        if (!AmmoBlock)
        {
            return std::unexpected(std::format("unknown ammo class '{}'", AmmoName));
        }

        // Ammo classes leave most movement fields out; the class record keeps them zeroed.
        const auto Field = [](const FLevelPropertyBlock* const Parent, const std::string_view FieldName) { return Parent ? ClassNumber(*Parent, FieldName) : 0.f; };
        const FLevelPropertyBlock* const Thrust = FindBlock(*Ammo, "Thrust");
        const FLevelPropertyBlock* const Weight = FindBlock(*Ammo, "Weight");
        const int BoundingBoxSize = int(ClassNumber(*Ammo, "Bounding-box-size"));
        const FMovementClass Movement{
            .ThrustInitial = Field(Thrust, "Initial"),
            .ThrustMin = Field(Thrust, "Min"),
            .ThrustMax = Field(Thrust, "Max"),
            .Weight = Weight ? ClassNumber(*Weight, "Initial") : 1.f,
            .AccelerationStep = ClassNumber(*Ammo, "Acceleration-step"),
            .DecelerationStep = ClassNumber(*Ammo, "Decceleration-step"),
            .SpeedMax = ClassNumber(*Ammo, "Speed-max"),
            .Elasticity = ClassNumber(*Ammo, "Elasticity"),
            .Grip = ClassNumber(*Ammo, "Grip"),
            .Friction = ParsePair(Ammo->Find("Friction-factor")).value_or(FCollisionPoint{}),
            .FrictionThreshold = ParsePair(Ammo->Find("Friction-threshold")).value_or(FCollisionPoint{}),
            .bUseInertia = ClassNumber(*Ammo, "Use-inertia") != 0.f,
            .Radius = BoundingBoxSize != 0 ? float(BoundingBoxSize) : DefaultCollisionRadius,
        };
        if (Movement.Weight == 0.f)
        {
            return std::unexpected(std::format("{}: zero Weight", AmmoName));
        }

        int ListedDamage = TruncateLow(ClassNumber(*AmmoBlock, "Damage"));
        int ListedArea = int(ClassNumber(*AmmoBlock, "Damage-zone"));
        if (ListedDamage == 0 || ListedArea == 0)
        {
            for (const FLevelPropertyBlock& Child : Ammo->Children)
            {
                if (Child.Kind == "TYPE" && (Child.Name == "Ammo" || Child.Name == "Explosion"))
                {
                    ListedDamage = std::max(ListedDamage, TruncateLow(ClassNumber(Child, "Damage")));
                    ListedArea = std::max(ListedArea, int(ClassNumber(Child, "Damage-zone")));
                }
            }
        }

        Combat.AmmoClasses.push_back({
            .Name = AmmoName,
            .Movement = Movement,
            .Damage = ClassNumber(*AmmoBlock, "Damage"),
            .DamageZone = ClassNumber(*AmmoBlock, "Damage-zone"),
            .DamageAfter = uint32_t(ClassNumber(*AmmoBlock, "Damage-after")),
            .HitEffect = ToLowerName(AmmoBlock->Find("Hit-fx")),
            .ListedDamage = ListedDamage,
            .ListedArea = ListedArea,
            .bUsableWhenInvisible = ClassNumber(*AmmoBlock, "Usable-when-invisible") != 0.f,
        });
    }

    Combat.Weapons.push_back({
        .Name = LowerName,
        .Ammo = AmmoName,
        .Delay = uint32_t(ClassNumber(*Block, "Delay")),
        .FireRange = ClassNumber(*Block, "Fire-Range"),
        .WeaponsGap = ClassNumber(*Block, "Weapons-gap"),
        .bLeftSide = ClassNumber(*Block, "Left-side") != 0.f,
        .bRightSide = ClassNumber(*Block, "Right-side") != 0.f,
        .bAlternated = ClassNumber(*Block, "Alternated-fire") != 0.f,
        .StartDistance = int(ClassNumber(*Block, "Start-distance")),
        .FireAltitude = int(ClassNumber(*Block, "Fire-altitude")),
        .InitialSlope = SlopeRadians(ClassNumber(*Block, "Initial-slope")),
        .SlopeStep = SlopeRadians(ClassNumber(*Block, "Slope-step")),
    });
    return Combat.Weapons.size() - 1;
}

std::expected<void, std::string> LoadEffect(FCombat& Combat, const FLevel& Level, FResourceArchive& Archive, const std::string& Name)
{
    if (Name.empty() || std::ranges::contains(Combat.EffectClasses, Name, &FEffectClass::Name))
    {
        return {};
    }

    const FLevelPropertyBlock* const Class = FindClass(Level, Name);
    const auto GafPath = Level.ClassGafPaths.find(Name);
    const FLevelPropertyBlock* const Effect = Class ? FindTypeBlock(*Class, "FX") : nullptr;
    if (!Effect || GafPath == Level.ClassGafPaths.end())
    {
        return std::unexpected(std::format("unknown FX class '{}'", Name));
    }

    const int GafIndex = int(ClassNumber(*Class, "GAF-index"));
    const std::optional<std::vector<uint8_t>> File = Archive.Read(GafPath->second);
    const std::optional<std::vector<Gaf::FFrame>> Bitmaps = File ? Gaf::LoadFrames(*File) : std::nullopt;
    const std::optional<std::vector<Gaf::FAnimation>> Animations = Bitmaps ? Gaf::LoadAnimations(*File, Bitmaps->size()) : std::nullopt;
    if (!Animations || GafIndex < 1 || size_t(GafIndex) > Animations->size())
    {
        return std::unexpected(std::format("FX class '{}': cannot load animation {} of {}", Name, GafIndex, GafPath->second));
    }

    const Gaf::FAnimation& Animation = (*Animations)[size_t(GafIndex - 1)];
    FEffectClass EffectClass{
        .Name = Name,
        .GafPath = GafPath->second,
        .GafIndex = GafIndex,
        .LoopStart = Animation.LoopStart,
        .bLoop = ClassNumber(*Effect, "Loop") != 0.f,
    };
    if (const float Stretch = ClassNumber(*Effect, "Stretch-ratio"); Stretch != 0.f)
    {
        EffectClass.Stretch = Stretch;
    }

    for (const Gaf::FAnimationFrame& Frame : Animation.Frames)
    {
        EffectClass.FrameDurations.push_back(Frame.Duration);
    }

    Combat.EffectClasses.push_back(std::move(EffectClass));
    return {};
}

// Where and how a weapon fires from: the shooter's world position, facing, velocity and altitude.
struct FShooter
{
    FCollisionPoint Position;
    FCollisionPoint Velocity;
    int Direction = 0;
    int Altitude = 0;
    float SpriteSize = 0.f;
    size_t ObjectIndex = 0;
};

// WeaponRaiseSlope (0042c210) while the trigger is held, WeaponResetSlope (0042c280) otherwise: holding flattens the slope step by step.
void UpdateSlope(FWeaponState& State, const FWeaponClass& Weapon, const bool bHeld)
{
    if (Weapon.InitialSlope == 0.f)
    {
        return;
    }

    if (!bHeld || !State.bTriggerHeld)
    {
        State.bTriggerHeld = bHeld;
        State.Slope = Weapon.InitialSlope;
    }
    else if (State.Slope != 0.f)
    {
        State.Slope = float(double(Weapon.SlopeStep) + double(State.Slope));
        if (0.f < State.Slope)
        {
            State.Slope = 0.f;
        }
    }
}

// ObjectUpdateWeapon (00444ca0) fires once Delay milliseconds of game clock have passed since the last shot; the first shot is immediate.
bool WeaponReady(const FCombat& Combat, const FWeaponState& State)
{
    const FWeaponClass& Weapon = Combat.Weapons[State.Weapon];
    return State.LastFireTime == 0 || State.LastFireTime == Combat.Clock || Combat.Clock - State.LastFireTime >= Weapon.Delay;
}

// WeaponFire at 00443bd0.
void FireWeapon(FCombat& Combat, FWeaponState& State, const FShooter& Shooter, FAmmoStock* const Stock)
{
    if (State.LastFireTime == 0)
    {
        State.LastFireTime = Combat.Clock;
    }

    if (!WeaponReady(Combat, State))
    {
        return;
    }

    State.LastFireTime = Combat.Clock;
    const FWeaponClass& Weapon = Combat.Weapons[State.Weapon];
    const auto Ammo = std::ranges::find(Combat.AmmoClasses, Weapon.Ammo, &FAmmoClass::Name);
    // ponytail: the original switches to the next loaded weapon when this one runs dry (WeaponHasAmmo at 00444450); here the weapon just stops.
    if (!Stock || Stock->Count < 1.f || Ammo == Combat.AmmoClasses.end() || Shooter.Direction == NoDirection)
    {
        return;
    }

    // Twin weapons fire from both sides of the body, Weapons-gap apart across the facing.
    FCollisionPoint Gap;
    if (Weapon.WeaponsGap != 0.f)
    {
        constexpr float RadiansPerDirection = -0.2617994f;
        const double Half = double(Weapon.WeaponsGap) * 0.5;
        const double Angle = double(Shooter.Direction) * double(RadiansPerDirection);
        const float Along = float(std::cos(Angle) * Half);
        const double Across = std::sin(Angle) * Half;
        Gap = { .X = float((double(Along) - Across) * double(0.8944272f)), .Y = float((Across + double(Along)) * double(0.4472136f)) };
    }

    if (Weapon.bAlternated)
    {
        State.bAlternateSide = !State.bAlternateSide;
    }

    const int SideCount = Weapon.WeaponsGap != 0.f ? 2 : 1;
    for (int Side = 0; Side < SideCount; ++Side)
    {
        const bool bSecondSide = Side == 1;
        const bool bSideFitted = Weapon.WeaponsGap == 0.f || (bSecondSide ? Weapon.bRightSide : Weapon.bLeftSide);
        const bool bSideTurn = !Weapon.bAlternated || State.bAlternateSide != bSecondSide;
        if (!bSideFitted || !bSideTurn)
        {
            continue;
        }

        // Without a Start-distance the muzzle sits a random distance out, up to 40% of the body's smaller sprite side.
        int Distance = Weapon.StartDistance;
        if (Distance == 0)
        {
            constexpr double MuzzleFraction = 0.4f;
            constexpr double MuzzleScale = 10.f;
            const uint32_t Random = Combat.Random.Next();
            const uint32_t Span = uint32_t(int64_t(double(Shooter.SpriteSize) * MuzzleFraction * MuzzleScale));
            Distance = Span == 0 ? 0 : int(Random % Span);
        }

        const FCollisionPoint Direction = WorldFacingVector(Shooter.Direction);
        FCollisionPoint Muzzle{
            .X = float(double(float(double(Distance) * double(Direction.X))) + double(Shooter.Position.X)),
            .Y = float(double(float(double(Distance) * double(Direction.Y))) + double(Shooter.Position.Y)),
        };
        if (Weapon.WeaponsGap != 0.f)
        {
            const double Sign = bSecondSide ? -1. : 1.;
            Muzzle = { .X = float(double(Muzzle.X) + Sign * double(Gap.X)), .Y = float(double(Muzzle.Y) + Sign * double(Gap.Y)) };
        }

        FProjectile Shot{
            .AmmoIndex = size_t(Ammo - Combat.AmmoClasses.begin()),
            .Altitude = Weapon.FireAltitude != 0 ? Weapon.FireAltitude : Shooter.Altitude,
            .DamageTimer = Ammo->DamageAfter,
            .Owner = Shooter.ObjectIndex,
        };
        Shot.State.Position = Shot.State.PreviousPosition = Muzzle;
        Shot.State.Center = CollisionFromWorld(Muzzle);
        Shot.State.Direction = Shooter.Direction;
        Shot.State.Speed = Ammo->Movement.ThrustInitial;
        const FCollisionPoint Facing = FacingVector(Shooter.Direction);
        Shot.State.Velocity = {
            .X = float(double(float(double(Shot.State.Speed) * double(Facing.X))) + double(Shooter.Velocity.X)),
            .Y = float(double(float(double(Shot.State.Speed) * double(Facing.Y))) + double(Shooter.Velocity.Y)),
        };
        if (Weapon.InitialSlope < 0.f)
        {
            Shot.bSloped = true;
            Shot.AltitudeAccumulator = float(Shot.Altitude);
            Shot.Slope = State.Slope;
        }

        Combat.Projectiles.push_back(Shot);
        Stock->Count -= 1.f;
        if (Stock->Count < 1.f)
        {
            break;
        }
    }
}

// Rotations per turret pose step: turret tops have one pose per direction (SeekerTurnTop at 0042d150).
void TurnTurretTop(FTurret& Turret, const uint32_t StepMilliseconds)
{
    Turret.TurnTimer -= int32_t(StepMilliseconds);
    if (0 < Turret.TurnTimer)
    {
        return;
    }

    Turret.TurnTimer += Turret.TurnTimeout;
    if (Turret.TurnTimer < 0)
    {
        Turret.TurnTimer = 0;
    }

    // PlayerAdvancePose: one pose toward the target, the short way round.
    const int Delta = Wrap(Turret.AimDirection - Turret.Direction);
    if (Delta != 0)
    {
        Turret.Direction = Wrap(Turret.Direction + (Delta <= DirectionCount / 2 ? 1 : -1));
    }
}
}

void UpdateAnimation(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, const int32_t Milliseconds)
{
    constexpr uint32_t LoopFlag = 2;
    if (Playback.bEnded)
    {
        return;
    }

    Playback.Timer -= Milliseconds;
    const uint32_t FrameCount = uint32_t(Animation.Frames.size());
    // A looping animation of zero-length frames would never leave this loop; one pass through it is enough.
    for (uint32_t Advanced = 0; Playback.Timer <= 0 && Advanced <= FrameCount; ++Advanced)
    {
        ++Playback.Frame;
        if (Playback.Frame > FrameCount)
        {
            Playback.Frame = (Animation.Flags & LoopFlag) != 0 ? Animation.LoopStart : 0;
        }

        if (Playback.Frame == 0 || Playback.Frame > FrameCount)
        {
            Playback = { .bEnded = true };
            return;
        }

        Playback.Timer += Animation.Frames[Playback.Frame - 1].Duration * AnimationPeriodMilliseconds;
    }
}

FAnimationPlayback StartAnimation(const Gaf::FAnimation& Animation)
{
    FAnimationPlayback Playback{ .bEnded = Animation.Frames.empty() };
    UpdateAnimation(Playback, Animation, 0);
    return Playback;
}

void ShowFrame(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, const uint32_t Frame)
{
    if (Frame == 0 || Frame > Animation.Frames.size())
    {
        Playback = { .bEnded = true };
        return;
    }

    Playback.Frame = Frame - 1;
    UpdateAnimation(Playback, Animation, Playback.Timer);
}

std::expected<std::vector<FDoor>, std::string> CreateDoors(const FLevel& Level, FResourceArchive& Archive, const std::span<FObjectBody> Bodies)
{
    std::vector<FDoor> Doors;
    std::unordered_map<std::string, std::vector<Gaf::FAnimation>> Gafs;
    const std::vector<FObjectBody*> BodyOf = BodiesByObject(Level, Bodies);
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        const bool bLift = Object.SubType == "lift";
        const FLevelPropertyBlock* const Block = FindTypeBlock(Object.Properties, bLift ? "Lift" : "Gate");
        if ((!bLift && Object.SubType != "gate") || !Block || !BodyOf[Index])
        {
            continue;
        }

        auto [Gaf, bNew] = Gafs.try_emplace(Object.GafPath);
        if (bNew)
        {
            const std::optional<std::vector<uint8_t>> File = Archive.Read(Object.GafPath);
            const std::optional<std::vector<Gaf::FFrame>> Bitmaps = File ? Gaf::LoadFrames(*File) : std::nullopt;
            std::optional<std::vector<Gaf::FAnimation>> Animations = Bitmaps ? Gaf::LoadAnimations(*File, Bitmaps->size()) : std::nullopt;
            Gaf->second = Animations ? std::move(*Animations) : std::vector<Gaf::FAnimation>{};
        }

        if (Object.GafIndex < 1 || size_t(Object.GafIndex) + 1 > Gaf->second.size())
        {
            return std::unexpected(std::format("{} needs two animations from GAF-index {} of {}", Object.ClassName, Object.GafIndex, Object.GafPath));
        }

        const auto Number = [Block](const std::string_view Field) { return int(ParseLevelFloat(Block->Find(Field)).value_or(0.f)); };
        const auto Flag = [&](const std::string_view Field, const uint32_t Bit) { return Number(Field) != 0 ? Bit : 0u; };
        FDoor Door{
            .ObjectIndex = Index,
            .bLift = bLift,
            .Flags = Flag("Active", DoorActive) | Flag(bLift ? "Present" : "Opened", DoorOpen) | Flag(bLift ? "Going-up" : "Closing", DoorOpening) |
                     Flag(bLift ? "Stay-up" : "Stay-opened", DoorStayOpen) | Flag(bLift ? "Stay-down" : "Stay-closed", DoorStayClosed) |
                     (bLift ? 0u : Flag("Sensor", DoorSensor)) | Flag("Key-always-needed", DoorKeyAlwaysNeeded) | Flag("Destroy-key", DoorDestroyKey) |
                     Flag("Needs-code", DoorNeedsCode),
            .Delay = Number("Delay"),
            .Id = Number("Id"),
            .UsageCount = Number("Usage-count"),
            .NextLevel = std::string(Block->Find("3do-file")),
            .Scenery = std::string(Block->Find("Scenery")),
            .Animations = { Gaf->second[size_t(Object.GafIndex - 1)], Gaf->second[size_t(Object.GafIndex)] },
        };
        // AnimInitializeGraphics shows the last frame of the animation for the activation state, and the timer starts full.
        const bool bOpen = (Door.Flags & DoorOpen) != 0;
        Door.Animation = bOpen ? OpenAnimation : ClosedAnimation;
        Door.Playback = StartAnimation(Door.Animations[Door.Animation]);
        ShowFrame(Door.Playback, Door.Animations[Door.Animation], uint32_t(Door.Animations[Door.Animation].Frames.size()));
        Door.Timer = Door.Delay;
        BodyOf[Index]->bPassable = bOpen;
        Doors.push_back(std::move(Door));
    }

    return Doors;
}

const FDoor* StepDoors(std::vector<FDoor>& Doors, const FLevel& Level, const std::span<FObjectBody> Bodies, const FPlayerTarget& Player,
    const uint32_t StepMilliseconds, FMessages& Messages)
{
    const std::vector<FObjectBody*> BodyOf = BodiesByObject(Level, Bodies);
    const int32_t Step = int32_t(StepMilliseconds);
    const FDoor* Exit = nullptr;
    for (FDoor& Door : Doors)
    {
        FObjectBody* const Body = BodyOf[Door.ObjectIndex];
        const Gaf::FAnimation& Animation = Door.Animations[Door.Animation];
        if (!Body)
        {
            continue;
        }

        // Graphic-update-needed: EngineUpdateObjects advances the animation before the update, which waits for a fresh one to finish.
        UpdateAnimation(Door.Playback, Animation, Step);
        const uint32_t Frame = Door.Playback.bEnded ? 0 : Door.Playback.Frame;
        if ((Door.Flags & DoorActive) == 0 || (Door.Timer == Door.Delay && Frame != Animation.Frames.size()))
        {
            continue;
        }

        if (!Door.bLift)
        {
            StepGate(Door, *Body, Level, BodyOf, Player, Step, Messages);
        }
        else if (StepLift(Door, *Body, Level, BodyOf, Player, Step, Messages) && !Exit)
        {
            Exit = &Door;
        }
    }

    return Exit;
}

void ReportDoorContact(const std::span<const FDoor> Doors, const FObjectBody& Body, const FPlayerState& Player, const FPlayerInventory& Inventory,
    FMessages& Messages)
{
    constexpr int32_t TextX = -0x96;
    constexpr int32_t InvisibleTextX = -0xfa;
    constexpr int32_t TextY = -0x20;
    constexpr int32_t KeyTextX = -0x32;
    constexpr uint32_t LifeMilliseconds = 2000;
    constexpr uint32_t Font = 6;
    const auto Door = std::ranges::find(Doors, Body.ObjectIndex, &FDoor::ObjectIndex);
    if (Door == Doors.end() || Body.bPassable)
    {
        return;
    }

    const bool bNeedsKey = Door->Id != 0 && !CarriesKey(Inventory.Keys, Door->Id);
    const bool bCode = (Door->Flags & DoorNeedsCode) != 0;
    std::string Text;
    if (Player.bInvisible)
    {
        Messages.Add({
            .Text = Door->bLift ? "Cannot take lift while invisible !" : "Cannot open gates while invisible !",
            .X = float(TruncateLow(Body.State.Position.X) + InvisibleTextX),
            .Y = float(TruncateLow(Body.State.Position.Y) + TextY),
            .LifeMilliseconds = LifeMilliseconds,
            .bWorld = true,
            .Font = Font,
        }, EMessagePlacement::Replace);
        return;
    }

    if (Door->bLift)
    {
        Text = !bNeedsKey ? "Wait, lift will come." : bCode ? "Lift needs full code !" : std::format("Lift needs key #{} !", Door->Id);
    }
    else if (bNeedsKey)
    {
        // The gate also plays a sound for 5 seconds.
        Text = std::format("Gate needs {} #{} !", bCode ? "full code" : "key", Door->Id);
    }
    else
    {
        return;
    }

    Messages.Add({
        .Text = std::move(Text),
        .X = float(TruncateLow(Body.State.Position.X) + TextX + (bNeedsKey ? KeyTextX : 0)),
        .Y = float(TruncateLow(Body.State.Position.Y) + TextY),
        .LifeMilliseconds = LifeMilliseconds,
        .bWorld = true,
        .Font = Font,
    }, EMessagePlacement::Replace);
}

namespace
{
std::expected<void, std::string> ArmSeeker(FCombat& Combat, const FLevel& Level, const FLevelObject& Object, FObjectBody& Body);
}

namespace
{
// surprise::FUN_0041c390: what a surprise does when it starts, unless another is in effect.
// ponytail: the screen effects (darkness, extasy, gamma flashes, screen shake), Touch-and-destroy's and Invulnerability's attached FX, Time-warp's
// trails, and how Triple-fire and Circular-fire change a shot are not ported; their flags are kept for when they are.
void StartSurprise(FCombat& Combat, const FLevel& Level, std::vector<FObjectBody>& Bodies, FPlayerState& Player, FMovementClass& Class, FPlayerInventory& Inventory,
    const ESurprise Type)
{
    FSurpriseState& State = Inventory.Surprises;
    if (State.Active != ESurprise::None)
    {
        return;
    }

    switch (Type)
    {
    case ESurprise::ReverseCommands:
        State.bReversed = true;
        break;
    case ESurprise::Trap:
    {
        // CreateObjectAtByName finds Explosion but no "Shock Wave" class, so only the explosion appears.
        constexpr float TrapScale = 0.75f;
        constexpr int ShakeRange = 0x10;
        SpawnEffect(Combat, "explosion", Player.Position, 0.f);
        Inventory.Energy = float(double(Inventory.Energy) * double(TrapScale));
        Inventory.Shield = float(double(Inventory.Shield) * double(TrapScale));
        State.ShakeX = Combat.Random.Range(-ShakeRange, ShakeRange);
        State.ShakeY = Combat.Random.Range(-ShakeRange, ShakeRange);
        break;
    }
    case ESurprise::WeaponJam:
        State.JammedWeapon = Combat.PlayerWeapon.Weapon;
        break;
    case ESurprise::Extasy:
        State.AccelerationStep = Class.AccelerationStep;
        State.DecelerationStep = Class.DecelerationStep;
        Class.AccelerationStep = Class.AccelerationStep + Class.AccelerationStep;
        Class.DecelerationStep = Class.DecelerationStep + Class.DecelerationStep;
        break;
    case ESurprise::LowSpeed:
    {
        constexpr float LowSpeedScale = 0.25f;
        State.AccelerationStep = Class.AccelerationStep;
        State.DecelerationStep = Class.DecelerationStep;
        Class.AccelerationStep *= LowSpeedScale;
        Class.DecelerationStep *= LowSpeedScale;
        break;
    }
    case ESurprise::Pinball:
    {
        constexpr float PinballElasticity = 1.5f;
        State.Elasticity = Class.Elasticity;
        Class.Elasticity = PinballElasticity;
        break;
    }
    case ESurprise::Banana:
    {
        // Friction 0.999 (0x3f7fbe77) and grip 1; the end gives the friction back but not the grip.
        constexpr float BananaFriction = 0.999f;
        State.Friction = Class.Friction;
        Class.Friction = { .X = BananaFriction, .Y = BananaFriction };
        Class.Grip = 1.f;
        break;
    }
    case ESurprise::BlockedMotor:
        State.bBlockedMotor = true;
        break;
    case ESurprise::DestroyAround:
    {
        // FUN_0043d980: everything destructible within six cells loses its energy, and the screen shakes.
        constexpr int AroundCells = 6;
        constexpr int ShakeRange = 0x20;
        const FWorldCell Centre = CellOf(Player.Position);
        for (FObjectBody& Body : Bodies)
        {
            const FWorldCell Cell = CellOf(Body.State.Position);
            if (!Body.bIndestructible && !Level.Objects[Body.ObjectIndex].bRemoved && std::abs(Cell.X - Centre.X) <= AroundCells &&
                std::abs(Cell.Y - Centre.Y) <= AroundCells)
            {
                Body.Energy = 0.f;
                Body.bDamaged = true;
            }
        }

        State.ShakeX = Combat.Random.Range(-ShakeRange, ShakeRange);
        State.ShakeY = Combat.Random.Range(-ShakeRange, ShakeRange);
        break;
    }
    case ESurprise::Invisible:
        Player.bInvisible = true;
        State.Body = Player.Position;
        State.InvisibleTimer = SurpriseTimeout(ESurprise::Invisible);
        State.ReturnStart.reset();
        break;
    case ESurprise::CircularFire:
        State.bCircularFire = true;
        break;
    case ESurprise::ReloadWeapons:
        State.SavedAmmo.clear();
        for (FAmmoStock& Stock : Inventory.Ammo)
        {
            State.SavedAmmo.push_back(Stock.Count);
            Stock.Count = Stock.Max;
        }

        break;
    case ESurprise::Invulnerability:
        State.bInvulnerable = true;
        break;
    case ESurprise::TripleFire:
        State.bTripleFire = true;
        break;
    default:
        break;
    }

    State.Active = Type;
}
}

void StepSurprises(FCombat& Combat, const FLevel& Level, std::vector<FObjectBody>& Bodies, FPlayerState& Player, FMovementClass& Class, FPlayerInventory& Inventory,
    FMessages& Messages, const uint32_t StepMilliseconds)
{
    FSurpriseState& State = Inventory.Surprises;
    const int32_t Step = int32_t(StepMilliseconds);
    bool bInEffect = false;
    for (auto Carried = State.Carried.begin(); Carried != State.Carried.end();)
    {
        if (0 < Carried->Delay)
        {
            Carried->Delay -= Step;
            ++Carried;
            continue;
        }

        if (0 < Carried->Timeout)
        {
            Carried->Timeout -= Step;
            if (!Carried->bStarted)
            {
                StartSurprise(Combat, Level, Bodies, Player, Class, Inventory, Carried->Type);
                Carried->bStarted = true;
            }

            bInEffect = true;
            ++Carried;
            continue;
        }

        EndSurprise(State, Carried->Type, Player, Class, Inventory);
        Messages.Remove(Carried->IconMessage);
        Carried = State.Carried.erase(Carried);
    }

    if (!bInEffect)
    {
        State.Active = ESurprise::None;
    }

    // FUN_00445380: in its last second the invisible drone flies back to its body, passable and without speed.
    if (Player.bInvisible && 0 < State.InvisibleTimer)
    {
        constexpr int32_t ReturnMilliseconds = 1000;
        constexpr float PerMillisecond = 0.001f;
        State.InvisibleTimer -= Step;
        if (State.InvisibleTimer < ReturnMilliseconds)
        {
            if (!State.ReturnStart)
            {
                State.ReturnStart = Player.Position;
            }

            const float Along = 1.f - float(State.InvisibleTimer) * PerMillisecond;
            Player.Velocity = {};
            Player.Position = {
                .X = Along * (State.Body.X - State.ReturnStart->X) + State.ReturnStart->X,
                .Y = Along * (State.Body.Y - State.ReturnStart->Y) + State.ReturnStart->Y,
            };
            Player.PreviousPosition = Player.Position;
            Player.Center = CollisionFromWorld(Player.Position);
        }
    }
}

void StepGenerators(FCombat& Combat, FLevel& Level, std::vector<FObjectBody>& Bodies, const FPlayerTarget& Player, const uint32_t StepMilliseconds)
{
    // Bodies grows while the loop runs, so the generators are found by index.
    for (size_t Index = 0; Index < Bodies.size(); ++Index)
    {
        if (!Bodies[Index].Generator || Level.Objects[Bodies[Index].ObjectIndex].bRemoved)
        {
            continue;
        }

        FObjectBody& Body = Bodies[Index];
        FGenerator& Generator = *Body.Generator;
        if ((0 <= Generator.MaxBirths && Generator.MaxBirths <= Generator.Births) || (Generator.BirthDelay == 0 && Generator.BirthDuration == 0) ||
            Generator.Product.empty())
        {
            continue;
        }

        int32_t Step = int32_t(StepMilliseconds);
        if (Step < Generator.Timer)
        {
            const FLevelPropertyBlock* const Energy = FindBlock(Level.Objects[Body.ObjectIndex].Properties, "Energy");
            const float EnergyMax = Energy ? ParseLevelFloat(Energy->Find("Max")).value_or(0.f) : 0.f;
            Step *= Generator.bStressable && Body.Energy < EnergyMax ? 2 : 1;
            Generator.Timer -= Step;
            continue;
        }

        // An open generator closes for the overlap test; nothing may stand on it when it closes or gives birth.
        if (!Generator.bBirthing && !Body.bPassable)
        {
            continue;
        }

        const bool bWasPassable = Body.bPassable;
        Body.bPassable = Generator.bBirthing ? Body.bPassable : false;
        const bool bBlocked = ObjectOverlapsSolid(Level, BodiesByObject(Level, Bodies), Player, Body);
        Body.bPassable = Generator.bBirthing ? Body.bPassable : bWasPassable;
        if (bBlocked)
        {
            continue;
        }

        if (!Generator.bBirthing)
        {
            Generator.Timer = Generator.BirthDuration;
            Generator.bBirthing = true;
            Body.bPassable = false;
            continue;
        }

        // The product appears on the generator, which opens again.
        const size_t GeneratorObject = Body.ObjectIndex;
        const FCollisionPoint Position = Body.State.Position;
        const std::optional<size_t> Created = CreateLevelObject(Level, Generator.Product, Position.X, Position.Y);
        std::optional<FObjectBody> Product = Created ? CreateObjectBody(Level, *Created) : std::nullopt;
        if (!Product)
        {
            continue;
        }

        Product->Parent = GeneratorObject;
        if (const std::expected<void, std::string> Armed = ArmSeeker(Combat, Level, Level.Objects[*Created], *Product); !Armed)
        {
            continue;
        }

        ++Generator.Births;
        Generator.Timer = Generator.BirthDelay;
        Generator.bBirthing = false;
        Body.bPassable = true;
        Combat.CreatedObjects.push_back(*Created);
        Bodies.push_back(std::move(*Product));
    }
}

std::string NextLevelPath(const std::string_view CurrentPath, const FDoor& Lift)
{
    const size_t Separator = CurrentPath.find('\\');
    const std::string_view Zone = Lift.Scenery.empty() ? CurrentPath.substr(0, Separator == std::string_view::npos ? 0 : Separator) : Lift.Scenery;
    return ToLowerName(std::format("{}\\{}", Zone, Lift.NextLevel));
}

bool FViewCells::Contains(const FCollisionPoint World) const
{
    const FWorldCell Cell = CellOf(World);
    return Cell.X >= Left && Cell.X < Right && Cell.Y >= Top && Cell.Y < Bottom;
}

namespace
{
// A moving seeker or tracker takes its class's first ADD Weapon and ADD Ammo (ObjectSetWeaponSlot with slot 1).
std::expected<void, std::string> ArmSeeker(FCombat& Combat, const FLevel& Level, const FLevelObject& Object, FObjectBody& Body)
{
    const auto WeaponEntry = std::ranges::find_if(Object.Properties.Children, [](const FLevelPropertyBlock& Child) { return Child.Kind == "ADD" && Child.Name == "Weapon"; });
    const auto AmmoEntry = std::ranges::find_if(Object.Properties.Children, [](const FLevelPropertyBlock& Child) { return Child.Kind == "ADD" && Child.Name == "Ammo"; });
    FSeeker* const Seeker = Body.Actor && Body.Actor->Seeker ? &*Body.Actor->Seeker : nullptr;
    if (!Seeker || WeaponEntry == Object.Properties.Children.end())
    {
        return {};
    }

    const std::expected<size_t, std::string> Weapon = LoadWeapon(Combat, Level, WeaponEntry->Find("Name"));
    if (!Weapon)
    {
        return std::unexpected(std::format("{}: {}", Object.ClassName, Weapon.error()));
    }

    Seeker->Weapon = *Weapon;
    Seeker->WeaponState = { .Weapon = *Weapon, .Slope = Combat.Weapons[*Weapon].InitialSlope };
    if (AmmoEntry != Object.Properties.Children.end())
    {
        Seeker->Ammo = { .Name = ToLowerName(AmmoEntry->Find("Name")), .Count = ClassNumber(*AmmoEntry, "Count"), .Max = ClassNumber(*AmmoEntry, "Max") };
    }

    return {};
}
}

std::expected<FCombat, std::string> CreateCombat(FLevel& Level, const FLevelObject& Player, FResourceArchive& Archive, std::vector<FObjectBody>& Bodies)
{
    FCombat Combat;
    Combat.Bobs = ActivateObjects(Level, Bodies, Combat.Random);
    for (const FLevelPropertyBlock& Child : Player.Properties.Children)
    {
        if (Child.Kind == "ADD" && Child.Name == "Weapon")
        {
            const std::expected<size_t, std::string> Weapon = LoadWeapon(Combat, Level, Child.Find("Name"));
            if (!Weapon)
            {
                return std::unexpected(Weapon.error());
            }

            Combat.PlayerWeapons.push_back(*Weapon);
        }
    }

    for (const size_t Weapon : Combat.PlayerWeapons)
    {
        Combat.PlayerSlotStates.push_back({ .Weapon = Weapon, .Slope = Combat.Weapons[Weapon].InitialSlope });
    }

    if (!Combat.PlayerSlotStates.empty())
    {
        Combat.PlayerWeapon = Combat.PlayerSlotStates.front();
    }

    for (const FLevelPropertyBlock& Child : Player.Properties.Children)
    {
        if (Child.Kind == "TYPE" && Child.Name == "Explosion")
        {
            Combat.PlayerExplosions.push_back({
                .BoomEffect = ToLowerName(Child.Find("Boom-fx")),
                .Damage = ClassNumber(Child, "Damage"),
                .Zone = ClassNumber(Child, "Damage-zone"),
            });
        }
    }

    // Moving seekers carry their first ADD Weapon and ADD Ammo.
    for (FObjectBody& Body : Bodies)
    {
        if (std::expected<void, std::string> Armed = ArmSeeker(Combat, Level, Level.Objects[Body.ObjectIndex], Body); !Armed)
        {
            return std::unexpected(Armed.error());
        }
    }

    // A generator's product may be of a class no placed object uses: its weapon and effects load now, while the archive is at hand.
    std::vector<std::string> ProductEffects;
    for (FObjectBody& Body : Bodies)
    {
        if (!Body.Generator)
        {
            continue;
        }

        const auto Product = Level.ClassObjects.find(Body.Generator->Product);
        if (Product == Level.ClassObjects.end())
        {
            return std::unexpected(std::format("unknown generator product '{}'", Body.Generator->Product));
        }

        const std::optional<uint32_t> Behaviour = ParseBehaviour(Product->second.Behaviour);
        const uint32_t Target = Behaviour ? *Behaviour & BehaviourTargetMask : 0;
        Body.Generator->bProductHunts = Target == BehaviourSeeker || Target == BehaviourTracker;
        const auto WeaponEntry = std::ranges::find_if(Product->second.Properties.Children, [](const FLevelPropertyBlock& Child) { return Child.Kind == "ADD" && Child.Name == "Weapon"; });
        if (Body.Generator->bProductHunts && WeaponEntry != Product->second.Properties.Children.end())
        {
            if (const std::expected<size_t, std::string> Weapon = LoadWeapon(Combat, Level, WeaponEntry->Find("Name")); !Weapon)
            {
                return std::unexpected(std::format("{}: {}", Product->second.ClassName, Weapon.error()));
            }
        }

        for (const FLevelPropertyBlock& Child : Product->second.Properties.Children)
        {
            if (Child.Kind == "TYPE" && Child.Name == "Explosion")
            {
                ProductEffects.push_back(ToLowerName(Child.Find("Boom-fx")));
            }
        }
    }

    for (FObjectBody& Body : Bodies)
    {
        const FLevelObject& Object = Level.Objects[Body.ObjectIndex];
        const FLevelPropertyBlock* const Seeker = FindTypeBlock(Object.Properties, "Seeker");
        const auto WeaponEntry = std::ranges::find_if(Object.Properties.Children, [](const FLevelPropertyBlock& Child) { return Child.Kind == "ADD" && Child.Name == "Weapon"; });
        const auto AmmoEntry = std::ranges::find_if(Object.Properties.Children, [](const FLevelPropertyBlock& Child) { return Child.Kind == "ADD" && Child.Name == "Ammo"; });
        if (Object.SubType != "turret" || !Seeker || WeaponEntry == Object.Properties.Children.end())
        {
            continue;
        }

        const std::expected<size_t, std::string> Weapon = LoadWeapon(Combat, Level, WeaponEntry->Find("Name"));
        if (!Weapon)
        {
            return std::unexpected(std::format("{}: {}", Object.ClassName, Weapon.error()));
        }

        // ponytail: the Vision-Angle conversion is not traced; it is read as the full cone, half a direction (15 degrees) per 30 degrees.
        constexpr float DegreesPerVisionStep = 30.f;
        FTurret Turret{
            .VisionDirections = int(ClassNumber(*Seeker, "Vision-Angle") / DegreesPerVisionStep),
            .SensorRange = ClassNumber(*Seeker, "Sensor-Range"),
            .ScanDelay = int32_t(ClassNumber(*Seeker, "Scan-Delay")),
            .MinDistance = ClassNumber(*Seeker, "Min-distance"),
            .TurnTimeout = int32_t(ClassNumber(Object.Properties, "Turret-turn-timeout")),
            .BaseDirection = Object.Direction == ObjectDirectionCount ? 0 : Object.Direction,
            .Direction = Object.Direction == ObjectDirectionCount ? 0 : Object.Direction,
            .Weapon = { .Weapon = *Weapon, .Slope = Combat.Weapons[*Weapon].InitialSlope },
        };
        Turret.AimDirection = Turret.Direction;
        if (AmmoEntry != Object.Properties.Children.end())
        {
            Turret.Ammo = { .Name = ToLowerName(AmmoEntry->Find("Name")), .Count = ClassNumber(*AmmoEntry, "Count"), .Max = ClassNumber(*AmmoEntry, "Max") };
        }

        Body.Turret = Turret;
    }

    std::vector<std::string> EffectNames = { std::string(GroundImpactEffect) };
    for (const FAmmoClass& Ammo : Combat.AmmoClasses)
    {
        EffectNames.push_back(Ammo.HitEffect);
    }

    for (const FObjectBody& Body : Bodies)
    {
        for (const FExplosionBlock& Explosion : Body.Explosions)
        {
            EffectNames.push_back(Explosion.BoomEffect);
        }
    }

    for (const FExplosionBlock& Explosion : Combat.PlayerExplosions)
    {
        EffectNames.push_back(Explosion.BoomEffect);
    }

    EffectNames.insert(EffectNames.end(), ProductEffects.begin(), ProductEffects.end());
    // A trap surprise sets off an Explosion FX.
    EffectNames.push_back("explosion");
    if (const std::optional<std::vector<uint8_t>> File = Archive.Read("common\\data\\object\\item\\surprise.gaf"))
    {
        const std::optional<std::vector<Gaf::FFrame>> Bitmaps = Gaf::LoadFrames(*File);
        std::optional<std::vector<Gaf::FAnimation>> Animations = Bitmaps ? Gaf::LoadAnimations(*File, Bitmaps->size()) : std::nullopt;
        Combat.SurpriseIcons = Animations ? std::move(*Animations) : std::vector<Gaf::FAnimation>{};
    }

    for (const std::string& Name : EffectNames)
    {
        if (const std::expected<void, std::string> Result = LoadEffect(Combat, Level, Archive, Name); !Result)
        {
            return std::unexpected(Result.error());
        }
    }

    return Combat;
}

void StepObjectBobs(FCombat& Combat, FLevel& Level, const std::span<FObjectBody> Bodies, const uint32_t StepMilliseconds, const FViewCells View)
{
    const std::vector<FObjectBody*> BodyOf = BodiesByObject(Level, Bodies);
    for (size_t Index = 0; Index < Level.Objects.size() && Index < Combat.Bobs.size(); ++Index)
    {
        FLevelObject& Object = Level.Objects[Index];
        FObjectBody* const Body = BodyOf[Index];
        if (Object.bRemoved || !View.Contains(Body ? Body->State.Position : FCollisionPoint{ .X = Object.X, .Y = Object.Y }))
        {
            continue;
        }

        const std::optional<EBobMode> Mode = BobMode(Combat.Bobs[Index], Body ? Body->State.Speed : 0.f);
        if (const std::optional<int> Altitude = Mode ? PlayBob(Combat.Bobs[Index], *Mode, int32_t(StepMilliseconds)) : std::nullopt)
        {
            Object.Altitude = float(*Altitude);
            if (Body)
            {
                Body->Altitude = Object.Altitude;
            }
        }
    }
}

namespace
{
// The stock that feeds a slot's weapon, if the slot holds one.
const FAmmoStock* SlotAmmo(const FCombat& Combat, const size_t Slot, const FPlayerInventory& Inventory)
{
    if (Slot == 0 || Slot > Combat.PlayerWeapons.size())
    {
        return nullptr;
    }

    const FWeaponClass& Weapon = Combat.Weapons[Combat.PlayerWeapons[Slot - 1]];
    const auto Stock = std::ranges::find_if(Inventory.Ammo, [&Weapon](const FAmmoStock& Entry) { return ToLowerName(Entry.Name) == Weapon.Ammo; });
    return Stock == Inventory.Ammo.end() ? nullptr : &*Stock;
}

constexpr size_t WeaponSlotCount = 9;
}

void ExplodePlayer(FCombat& Combat, const FCollisionPoint Position, const float Altitude)
{
    for (const FExplosionBlock& Explosion : Combat.PlayerExplosions)
    {
        SpawnEffect(Combat, Explosion.BoomEffect, Position, Altitude, Explosion.Damage, Explosion.Zone);
    }
}

size_t NextPlayerWeaponSlot(const FCombat& Combat, const FPlayerInventory& Inventory)
{
    const size_t Current = Combat.PlayerSlot;
    if (Current == 0 || Current > WeaponSlotCount)
    {
        return Current;
    }

    for (size_t Slot = Current % WeaponSlotCount + 1; Slot != Current; Slot = Slot % WeaponSlotCount + 1)
    {
        const FAmmoStock* const Stock = SlotAmmo(Combat, Slot, Inventory);
        if (Stock && Stock->Count >= 1.f)
        {
            return Slot;
        }
    }

    return Current;
}

void SelectPlayerWeapon(FCombat& Combat, const size_t Slot, const FPlayerInventory& Inventory, FMessages& Messages)
{
    // FUN_004375f0 keeps the weapon in hand's state with its ADD Weapon entry and clears the new one's last shot.
    if (Combat.PlayerSlot >= 1 && Combat.PlayerSlot <= Combat.PlayerSlotStates.size())
    {
        Combat.PlayerSlotStates[Combat.PlayerSlot - 1] = Combat.PlayerWeapon;
    }

    Combat.PlayerSlot = Slot;
    if (Slot == 0 || Slot > Combat.PlayerSlotStates.size())
    {
        return;
    }

    Combat.PlayerWeapon = Combat.PlayerSlotStates[Slot - 1];
    Combat.PlayerWeapon.LastFireTime = 0;

    // Option flag 8 ("HUD Green Info") lists the weapon and its ammo at 400, 140 in the panel's font 3. The port keeps the flag on.
    constexpr int32_t ListX = 400;
    constexpr std::array<int32_t, 3> WeaponLines = { 0x8c, 0x96, 0xa0 };
    constexpr std::array<int32_t, 3> AmmoLines = { 0xb4, 0xbe, 0xc8 };
    constexpr uint32_t ListLifeMilliseconds = 4000;
    constexpr uint32_t PanelScreenFont = 3;
    const auto List = [&](const int32_t Y, std::string Text)
    {
        Messages.Add({
            .Text = std::move(Text),
            .X = float(ListX),
            .Y = float(Y),
            .LifeMilliseconds = ListLifeMilliseconds,
            .bTypewriter = true,
            .Gaf = EMessageGaf::Panel,
            .Font = PanelScreenFont,
        }, EMessagePlacement::Replace);
    };
    const auto Upper = [](std::string Text)
    {
        std::ranges::transform(Text, Text.begin(), [](const char Character) { return char(std::toupper(uint8_t(Character))); });
        return Text;
    };
    const FWeaponClass& Weapon = Combat.Weapons[Combat.PlayerWeapon.Weapon];
    List(WeaponLines[0], std::format("WEAPON {}", Upper(Weapon.Name)));
    List(WeaponLines[1], std::format("FIRE RANGE {}", int32_t(Weapon.FireRange)));
    List(WeaponLines[2], std::format("FIRE DELAY {}", Weapon.Delay));

    // FUN_00437540 binds only a stock that is not empty.
    const FAmmoStock* const Stock = SlotAmmo(Combat, Slot, Inventory);
    const auto Ammo = std::ranges::find(Combat.AmmoClasses, Weapon.Ammo, &FAmmoClass::Name);
    if (Stock && Stock->Count != 0.f && Ammo != Combat.AmmoClasses.end())
    {
        List(AmmoLines[0], std::format("AMMO {}", Upper(Ammo->Name)));
        List(AmmoLines[1], std::format("DAMAGE {}", Ammo->ListedDamage));
        List(AmmoLines[2], std::format("DAMAGE AREA {}", Ammo->ListedArea));
    }
}

void StepPlayerWeapon(FCombat& Combat, const bool bFire, const FPlayerState& Player, const FLevelObject& PlayerObject, const size_t PlayerIndex,
    const float SpriteSize, FPlayerInventory& Inventory, FMessages& Messages)
{
    if (Combat.PlayerSlot == 0 || Combat.PlayerSlot > Combat.PlayerSlotStates.size())
    {
        return;
    }

    // WeaponHasAmmo at 00444450: firing an empty weapon moves to the next one with ammo, and the shot goes out from that.
    if (const FAmmoStock* const Stock = SlotAmmo(Combat, Combat.PlayerSlot, Inventory); bFire && Stock && Stock->Count == 0.f)
    {
        SelectPlayerWeapon(Combat, NextPlayerWeaponSlot(Combat, Inventory), Inventory, Messages);
        if (Combat.PlayerSlot == 0 || Combat.PlayerSlot > Combat.PlayerSlotStates.size())
        {
            return;
        }
    }

    FWeaponState& State = Combat.PlayerWeapon;
    const FWeaponClass& Weapon = Combat.Weapons[State.Weapon];
    UpdateSlope(State, Weapon, bFire);
    // WeaponFire holds back the jammed weapon (session +0x220c).
    if (!bFire || Inventory.Surprises.JammedWeapon == State.Weapon)
    {
        return;
    }

    // ObjectUpdateWeapon fires only Usable-when-invisible ammo while the player is invisible, and says so with option flag 8.
    const auto AmmoClass = std::ranges::find(Combat.AmmoClasses, Weapon.Ammo, &FAmmoClass::Name);
    if (Player.bInvisible && (AmmoClass == Combat.AmmoClasses.end() || !AmmoClass->bUsableWhenInvisible))
    {
        constexpr int32_t NoteX = 100;
        constexpr int32_t NoteY = 0x6e;
        constexpr uint32_t NoteLifeMilliseconds = 2000;
        constexpr uint32_t PanelScreenFont = 3;
        if (Messages.Exists(NoteX, NoteY))
        {
            Messages.SetLife(NoteX, NoteY, NoteLifeMilliseconds);
            return;
        }

        Messages.Add({
            .Text = "NOTE : ONLY MINES CAN BE USED WHILE INVISIBLE",
            .X = float(NoteX),
            .Y = float(NoteY),
            .LifeMilliseconds = NoteLifeMilliseconds,
            .bTypewriter = true,
            .Gaf = EMessageGaf::Panel,
            .Font = PanelScreenFont,
        }, EMessagePlacement::Replace);
        return;
    }

    const auto Stock = std::ranges::find_if(Inventory.Ammo, [&Weapon](const FAmmoStock& Entry) { return ToLowerName(Entry.Name) == Weapon.Ammo; });
    const FShooter Shooter{
        .Position = Player.Position,
        .Velocity = Player.Velocity,
        .Direction = Player.Direction,
        .Altitude = int(PlayerObject.Altitude),
        .SpriteSize = SpriteSize,
        .ObjectIndex = PlayerIndex,
    };
    FireWeapon(Combat, State, Shooter, Stock == Inventory.Ammo.end() ? nullptr : &*Stock);
}

namespace
{
std::vector<FPathObstacle> TerrainPathObstacles(const FTerrainCollision& Terrain)
{
    std::vector<FPathObstacle> Obstacles;
    Obstacles.reserve(Terrain.GetShapes().size());
    // Path graph queries ignore kind-3 actors; terrain shapes already have the original insertion order.
    for (const FStaticCollisionShape& Shape : Terrain.GetShapes())
    {
        FPathObstacle Obstacle{ .Min = Shape.Points[0], .Max = Shape.Points[0], .bWater = Shape.bWater };
        for (size_t Index = 1; Index < Shape.PointCount; ++Index)
        {
            Obstacle.Min.X = std::min(Obstacle.Min.X, Shape.Points[Index].X);
            Obstacle.Min.Y = std::min(Obstacle.Min.Y, Shape.Points[Index].Y);
            Obstacle.Max.X = std::max(Obstacle.Max.X, Shape.Points[Index].X);
            Obstacle.Max.Y = std::max(Obstacle.Max.Y, Shape.Points[Index].Y);
        }

        Obstacles.push_back(Obstacle);
    }

    return Obstacles;
}
}

namespace
{
// SeekerUpdate (0042c5a0) and the movement updates advance route searches every 17 ms.
constexpr int32_t RouteSearchPeriodMilliseconds = 17;
// Object +0x13c bits.
constexpr uint32_t RouteFollowing = 1;
constexpr uint32_t RouteMovementAllowed = 2;
// Object +0x194 values.
constexpr uint32_t ModeSeeker = 1;
constexpr uint32_t ModeTracker = 2;
// SeekerRangeState bits.
constexpr uint32_t RangeNearEnd = 1;
constexpr uint32_t RangeClose = 2;
constexpr uint32_t RangeFiring = 4;

// FUN_00407630: the direction of a vector after rotating it as if it were in world space; the range checks pass it collision-space deltas.
int RotatedDirection(const FCollisionPoint Vector)
{
    return Vector.X == 0.f && Vector.Y == 0.f ? NoDirection : FindDirection(CollisionFromWorld(Vector));
}

// SeekerUpdateSegment at 0042c9d0; false for a degenerate segment.
bool UpdateSegment(FRouteWork& Work)
{
    Work.SegmentDelta = { .X = Work.SegmentEnd.X - Work.SegmentStart.X, .Y = Work.SegmentEnd.Y - Work.SegmentStart.Y };
    Work.SegmentLengthSquared = Work.SegmentDelta.X * Work.SegmentDelta.X + Work.SegmentDelta.Y * Work.SegmentDelta.Y;
    Work.SegmentDirection = FindDirection(Work.SegmentDelta);
    return Work.SegmentDirection != NoDirection;
}

// FUN_0042c960: step to the route's next segment; false once the route is used up.
bool NextSegment(FRouteWork& Work)
{
    FPathRoute* const Route = Work.Route ? &*Work.Route : nullptr;
    if (!Route || Route->Current >= Route->Points.size())
    {
        return false;
    }

    ++Route->Current;
    if (Route->Current >= Route->Points.size())
    {
        return false;
    }

    Work.SegmentStart = Route->Points[Route->Current - 1];
    Work.SegmentEnd = Route->Points[Route->Current];
    return UpdateSegment(Work);
}

// The route's current node is its last point, or the route is used up.
bool OnLastSegment(const FRouteWork& Work)
{
    return Work.Route->Current + 1 >= Work.Route->Points.size();
}

// SeekerOnSegment at 0042d820: the actor is still inside the segment's bounds, widened by its size and speed.
bool OnSegment(const FObjectBody& Body, const FRouteWork& Work)
{
    const float Slack = float(double(Body.Class.Radius) * 2. + double(Body.State.SpeedMagnitude));
    const auto Within = [Slack](const float Value, const float Start, const float End)
    {
        return End < Start ? End - Slack <= Value && Value <= Slack + Start : Start - Slack <= Value && Value <= Slack + End;
    };
    return Within(Body.State.Center.X, Work.SegmentStart.X, Work.SegmentEnd.X) && Within(Body.State.Center.Y, Work.SegmentStart.Y, Work.SegmentEnd.Y);
}

// SeekerRangeState at 0042cd30, with SeekerDirectRange (0042ce40) first when there is a target.
uint32_t RangeState(const FObjectBody& Body, const FRouteWork& Work, const FPlayerState* const Target)
{
    // The facing checks use FUN_00407630, which compares against the facing-table index (object +0x150, three directions on).
    const bool bFacingCheck = Target && !Body.Actor->bTurnOnPlace;
    const int TableDirection = Wrap(Body.State.Direction + DirectionVectorOffset);
    const float RadiusSquared = float(Work.Radius * Work.Radius);
    if (Target && OnLastSegment(Work))
    {
        const FCollisionPoint Delta{ .X = Target->Center.X - Body.State.Center.X, .Y = Target->Center.Y - Body.State.Center.Y };
        constexpr float CloseScale = 0.25f;
        const float DistanceSquared = Delta.X * Delta.X + Delta.Y * Delta.Y;
        if (!bFacingCheck || DirectionDistance(RotatedDirection(Delta), TableDirection) <= 1)
        {
            if (DistanceSquared <= RadiusSquared * CloseScale)
            {
                return RangeFiring | RangeClose | RangeNearEnd;
            }

            if (DistanceSquared <= RadiusSquared)
            {
                return RangeFiring | RangeNearEnd;
            }
        }
    }

    const FCollisionPoint Delta{ .X = Work.SegmentEnd.X - Body.State.Center.X, .Y = Work.SegmentEnd.Y - Body.State.Center.Y };
    const float DistanceSquared = Delta.X * Delta.X + Delta.Y * Delta.Y;
    if (!OnLastSegment(Work))
    {
        // Inner corners count from 8 units.
        constexpr float CornerDistanceSquared = 64.f;
        return DistanceSquared < CornerDistanceSquared ? RangeNearEnd : 0;
    }

    if (bFacingCheck && DirectionDistance(RotatedDirection(Delta), TableDirection) > 1)
    {
        return 0;
    }

    constexpr float RouteEndScale = 0.5625f;
    if (DistanceSquared <= RadiusSquared * RouteEndScale)
    {
        return RangeClose | RangeNearEnd;
    }

    return DistanceSquared < RadiusSquared ? RangeNearEnd : 0;
}

// FUN_0042cc60: one hundredth of the world-space offset back to the segment line, applied by ObjectComputeDisplacement.
FCollisionPoint SegmentCorrection(const FObjectBody& Body, const FRouteWork& Work)
{
    const float X = Body.State.Center.X - Work.SegmentStart.X;
    const float Y = Body.State.Center.Y - Work.SegmentStart.Y;
    const float Dot = Y * Work.SegmentDelta.Y + X * Work.SegmentDelta.X;
    const float Along = std::sqrt(Dot * Dot / Work.SegmentLengthSquared / Work.SegmentLengthSquared);
    const float OffsetX = Along * Work.SegmentDelta.X - X;
    const float OffsetY = Along * Work.SegmentDelta.Y - Y;
    constexpr float WorldXScale = 0.008944272f;
    constexpr float WorldYScale = 0.004472136f;
    return CollisionFromWorld({ .X = (OffsetX - OffsetY) * WorldXScale, .Y = (OffsetY + OffsetX) * WorldYScale });
}

// SeekerTurnBody at 0042cfa0 for a body that can turn on the spot: one pose toward the segment per Turn-timeout.
// ponytail: bodies that cannot turn on the spot scale the timeout with speed; only cops move so far, and they all turn on the spot.
// A two-piece body holds still while its top aims, and its top turns along with it.
void TurnActorBody(FPlayerState& State, const FMovementClass& Class, FActor& Actor, const uint32_t Range, const int Direction, const int32_t Step)
{
    constexpr uint32_t Firing = 4;
    if (Actor.bTwoPieces && (Range & Firing) != 0)
    {
        return;
    }

    State.TurnTimer -= Step;
    if (State.TurnTimer >= 1)
    {
        return;
    }

    const int PosesPerStep = Actor.PosesPerDirection;
    const int PoseCount = DirectionCount * PosesPerStep;
    State.TurnTimer = std::max(0, State.TurnTimer + Class.TurnTimeout);
    // A spider stops at the nearest of the four axis directions.
    constexpr int SpiderStep = DirectionCount / 4;
    const int Target = Actor.bSpider ? (Direction + SpiderStep / 2) / SpiderStep * SpiderStep % DirectionCount : Direction;
    State.TargetPose = Target * PosesPerStep + 1;
    const int Previous = State.Pose;
    State.Pose = AdvancePose(State.Pose, State.TargetPose, PoseCount);
    State.Direction = Actor.bSpider ? (State.Pose - 1) / SpiderStep * SpiderStep : (State.Pose - 1) / PosesPerStep;
    if (Actor.bTwoPieces && State.Pose != Previous)
    {
        const auto Rotate = [PoseCount, Delta = State.Pose - Previous](const int Pose)
        {
            return ((Pose - 1 + Delta) % PoseCount + PoseCount) % PoseCount + 1;
        };
        Actor.TopPose = Rotate(Actor.TopPose);
        Actor.TopTargetPose = Rotate(Actor.TopTargetPose);
    }
}

// SeekerTurnTop at 0042d150: in firing range, one top pose toward the target per Turret-turn-timeout.
void TurnActorTop(FActor& Actor, const uint32_t Range, const int Direction, const int32_t Step)
{
    constexpr uint32_t Firing = 4;
    if (!Actor.bTwoPieces || (Range & Firing) == 0)
    {
        return;
    }

    Actor.TopTurnTimer -= Step;
    if (Actor.TopTurnTimer >= 1)
    {
        return;
    }

    Actor.TopTurnTimer = std::max(0, Actor.TopTurnTimer + Actor.TopTurnTimeout);
    Actor.TopTargetPose = Direction * Actor.PosesPerDirection + 1;
    Actor.TopPose = AdvancePose(Actor.TopPose, Actor.TopTargetPose, DirectionCount * Actor.PosesPerDirection);
}

// The direction the actor aims and fires in: its top's for a two-piece body.
int AimDirection(const FObjectBody& Body)
{
    const FActor& Actor = *Body.Actor;
    return Actor.bTwoPieces ? (Actor.TopPose - 1) / Actor.PosesPerDirection : Body.State.Direction;
}

// SeekerAimAndFire at 0042d2d0 for a body without a turret top: inside the weapon's Fire-Range, facing the target and not turning, the trigger
// is pulled once the weapon's Delay has passed.
bool AimAndFire(const FCombat& Combat, const FObjectBody& Body, FActor& Actor, const FSeeker& Seeker, const FPlayerState& Target)
{
    if (!OnLastSegment(Seeker.Work))
    {
        return false;
    }

    if (AimDirection(Body) != Seeker.Work.SegmentDirection)
    {
        // A body that cannot turn on the spot drives forward to come round.
        if (!Actor.bTwoPieces && (Seeker.Work.RangeState & RangeFiring) != 0 && !Actor.bTurnOnPlace)
        {
            Actor.Command.Thrust = EPlayerThrust::Forward;
        }

        return false;
    }

    if (!Seeker.Weapon)
    {
        return false;
    }

    const FWeaponClass& Weapon = Combat.Weapons[*Seeker.Weapon];
    const FCollisionPoint Delta{ .X = Target.Center.X - Body.State.Center.X, .Y = Target.Center.Y - Body.State.Center.Y };
    const int FireRange = int(Weapon.FireRange);
    if (uint32_t(int64_t(Delta.X * Delta.X + Delta.Y * Delta.Y)) > uint32_t(FireRange * FireRange))
    {
        return false;
    }

    // FUN_00436520 reports a turn while the body pose has not reached its target pose.
    const bool bTurning = Body.State.Pose != Body.State.TargetPose;
    if (WeaponReady(Combat, Seeker.WeaponState) && !bTurning && FindDirection(Delta) == AimDirection(Body))
    {
        Actor.bTrigger = true;
    }

    return true;
}

// SeekerEngage at 0042caa0: turn toward the segment and drive along it; within Min-distance of the target, face it, back off when too close
// and fire.
void Engage(FCombat* const Combat, FObjectBody& Body, FRouteWork& Work, FSeeker* const Seeker, const FPlayerState* const Target, const int32_t Step)
{
    FActor& Actor = *Body.Actor;
    Work.RangeState = RangeState(Body, Work, Target);
    if ((Work.RangeState & RangeFiring) == 0)
    {
        const bool bDrive = (Work.RangeState & RangeNearEnd) == 0 || NextSegment(Work);
        TurnActorBody(Body.State, Body.Class, Actor, Work.RangeState, Work.SegmentDirection, Step);
        Actor.Command.Correction = SegmentCorrection(Body, Work);
        constexpr int SpiderStep = DirectionCount / 4;
        if (bDrive && (!Actor.bSpider || (Body.State.Pose - 1) % SpiderStep == 0))
        {
            Actor.Command.Thrust = EPlayerThrust::Forward;
        }

        return;
    }

    TurnActorBody(Body.State, Body.Class, Actor, Work.RangeState, Work.SegmentDirection, Step);
    TurnActorTop(Actor, Work.RangeState, Work.SegmentDirection, Step);
    if ((Work.RangeState & RangeClose) != 0)
    {
        Actor.Command.Thrust = EPlayerThrust::Backward;
        Body.State.bPushed = false;
        Body.State.Speed = std::min(Body.State.Speed, 0.f);
    }

    // The segment now ends at the target, still starting from where it did.
    Work.SegmentEnd = Target->Center;
    UpdateSegment(Work);
    if (!Seeker || !Combat)
    {
        return;
    }

    const FWeaponClass* const Weapon = Seeker->Weapon ? &Combat->Weapons[*Seeker->Weapon] : nullptr;
    if (AimAndFire(*Combat, Body, Actor, *Seeker, *Target))
    {
        if (Weapon)
        {
            UpdateSlope(Seeker->WeaponState, *Weapon, true);
        }

        // A seeker stops to shoot.
        if (Actor.Command.Thrust == EPlayerThrust::Forward)
        {
            Actor.Command.Thrust = EPlayerThrust::None;
        }
    }
    else if (Weapon)
    {
        UpdateSlope(Seeker->WeaponState, *Weapon, false);
    }
}

// Drops the route segment once the actor has left it; otherwise marks the actor as following a route.
bool KeepSegment(FObjectBody& Body, FRouteWork& Work)
{
    if (!OnSegment(Body, Work))
    {
        Body.Actor->RouteFlags &= ~RouteFollowing;
        Work.bOnSegment = false;
        Work.bSearching = false;
        return false;
    }

    Body.Actor->RouteFlags |= RouteFollowing;
    return true;
}

// Advances a pending route search; on a usable route the actor starts following its first segment.
EPathStatus AdvanceRoute(FObjectBody& Body, FRouteWork& Work, std::optional<std::vector<FPathObstacle>>& Obstacles, const FTerrainCollision& Terrain,
    const float MaxDistance, bool& bRouted)
{
    bRouted = false;
    if (!Obstacles)
    {
        Obstacles = TerrainPathObstacles(Terrain);
    }

    const FCollisionPoint HalfExtent{ .X = Body.Class.Radius, .Y = Body.Class.Radius };
    const EPathStatus Status = Work.Search.Advance(*Obstacles, HalfExtent, MaxDistance);
    if (Status != EPathStatus::Found && Status != EPathStatus::Finished)
    {
        return Status;
    }

    // A failed route keeps the previous one, as FUN_0042d760 restores the saved work attribute.
    std::optional<FPathRoute> Route = Work.Search.SelectFinishedRoute(*Obstacles, HalfExtent);
    if (Route)
    {
        std::optional<FPathRoute> Previous = std::move(Work.Route);
        Work.Route = std::move(Route);
        Work.Route->Current = 0;
        bRouted = NextSegment(Work);
        if (!bRouted)
        {
            Work.Route = std::move(Previous);
        }
    }

    return Status;
}

// SeekerFindTarget at 0042da80 for the player: alive, within Sensor-Range and within the vision cone around the body's facing.
// ponytail: the grid-cell window around the seeker is replaced by the distance test it ends in; a player outside the window but in range is still found.
// A guardian searches its Guardian Range in every direction instead.
bool FindTarget(const FObjectBody& Body, const FSeeker& Seeker, const FPlayerState& Player)
{
    if (Player.bInvisible)
    {
        return false;
    }

    const FCollisionPoint Delta{ .X = Player.Center.X - Body.State.Center.X, .Y = Player.Center.Y - Body.State.Center.Y };
    const int Range = int(Seeker.bGuardian ? Seeker.GuardRange : Seeker.SensorRange);
    constexpr int AllAround = DirectionCount / 2;
    const int Vision = Seeker.bGuardian ? AllAround : Seeker.VisionDirections;
    return Delta.X * Delta.X + Delta.Y * Delta.Y <= float(Range * Range) && DirectionDistance(FindDirection(Delta), Body.State.Direction) <= Vision;
}

// SeekerUpdate at 0042c5a0 for a moving seeker or tracker.
void UpdateSeeker(FCombat* const Combat, FObjectBody& Body, FSeeker& Seeker, const FPlayerState* const Player,
    std::optional<std::vector<FPathObstacle>>& Obstacles, const FTerrainCollision& Terrain, const int32_t Step)
{
    FActor& Actor = *Body.Actor;
    FRouteWork& Work = Seeker.Work;
    const FPlayerState* const Target = Seeker.bHasTarget ? Player : nullptr;
    if (Work.bOnSegment && !KeepSegment(Body, Work))
    {
        return;
    }

    Actor.Mode = Seeker.bTracker ? ModeTracker : ModeSeeker;
    if (Work.bOnSegment)
    {
        if (Work.Timer >= 1)
        {
            Work.Timer -= Step;
            Engage(Combat, Body, Work, &Seeker, Target, Step);
            return;
        }

        // Every Scan-delay the route is checked: it ends once the target has moved, by position on the last segment and by cell before it.
        const bool bLast = OnLastSegment(Work);
        const FWorldCell Cell = Player ? CellOf(Player->Position) : FWorldCell{};
        const bool bMoved = !Target || (bLast ? Player->Position.X != Seeker.TargetPosition.X || Player->Position.Y != Seeker.TargetPosition.Y
                                              : Cell.X != Seeker.TargetCellX || Cell.Y != Seeker.TargetCellY);
        if (bMoved)
        {
            Work.bOnSegment = false;
            return;
        }

        Work.Timer += Work.Interval;
        return;
    }

    Actor.RouteFlags &= ~RouteFollowing;
    if (!Work.bSearching)
    {
        if (Work.Timer >= 1)
        {
            Work.Timer -= Step;
            return;
        }

        Work.Timer += Work.Interval;
        // FUN_0042d8f0 routes to the target, or a guardian without one back to its post.
        Seeker.bHasTarget = Player && FindTarget(Body, Seeker, *Player);
        bool bPlanned = false;
        if (Seeker.bHasTarget)
        {
            Seeker.TargetPosition = Player->Position;
            const FWorldCell Cell = CellOf(Player->Position);
            Seeker.TargetCellX = Cell.X;
            Seeker.TargetCellY = Cell.Y;
            bPlanned = Work.Search.Start(Body.State.Center, Player->Center);
        }
        else if (Seeker.bGuardian)
        {
            bPlanned = Work.Search.Start(Body.State.Center, CollisionFromWorld(Seeker.GuardLocation));
        }

        if (bPlanned)
        {
            Work.SearchTimer = 0;
            Work.bSearching = true;
        }
        else
        {
            Actor.RouteFlags |= RouteMovementAllowed;
        }

        return;
    }

    Work.SearchTimer -= Step;
    if (Work.SearchTimer > 0)
    {
        return;
    }

    Work.SearchTimer += RouteSearchPeriodMilliseconds;
    bool bRouted = false;
    const EPathStatus Status = AdvanceRoute(Body, Work, Obstacles, Terrain, Seeker.SensorRange, bRouted);
    if (Status == EPathStatus::Searching && Seeker.bTracker)
    {
        Work.Timer -= Step;
        return;
    }

    // FUN_0042ca30: a spider takes only a single straight leg along one of the four axis directions.
    constexpr int SpiderStep = DirectionCount / 4;
    if (bRouted && Actor.bSpider && (Work.Route->Points.size() != 2 || Work.SegmentDirection % SpiderStep != 0))
    {
        bRouted = false;
        Work.bOnSegment = false;
    }

    if (bRouted)
    {
        // A route to the target replaces the patrol's (FUN_0042d7d0).
        Work.bOnSegment = true;
        Actor.RouteFlags = (Actor.RouteFlags & ~RouteMovementAllowed) | RouteFollowing;
        if (Actor.Patrol)
        {
            Actor.Patrol->Work.bOnSegment = false;
            Actor.Patrol->Work.bSearching = false;
        }
    }
    else if (Status != EPathStatus::Failed && Status != EPathStatus::Searching)
    {
        Actor.RouteFlags |= RouteMovementAllowed;
    }

    Work.bSearching = false;
    Work.Timer += Work.Interval;
}

// FUN_0042e740: aim the next search at the current patrol point, moving on only after arriving at the end of a finished route.
bool StartPatrolLeg(FPatrol& Patrol, const FObjectBody& Body, const FLevel& Level)
{
    if (!Patrol.Path)
    {
        return false;
    }

    const FPatrolPath& Path = Level.PatrolPaths[*Patrol.Path];
    const int Count = int(Path.Points.size());
    if (!Patrol.bStarted)
    {
        // FUN_0042e910 starts at the nearest point by Manhattan distance, preferring the later point on ties.
        int Best = std::numeric_limits<int>::max();
        const int X = int(Body.State.Position.X);
        const int Y = int(Body.State.Position.Y);
        for (int Index = Count - 1; Index >= 0; --Index)
        {
            const int Distance = std::abs(X - Path.Points[size_t(Index)][0]) + std::abs(Y - Path.Points[size_t(Index)][1]);
            if (Distance < Best)
            {
                Best = Distance;
                Patrol.Point = Index;
            }
        }

        if (Patrol.Point < 0)
        {
            return false;
        }

        Patrol.bStarted = true;
    }
    else if (Patrol.Work.Route && Patrol.Work.Route->Current >= Patrol.Work.Route->Points.size() && (Patrol.Work.RangeState & RangeNearEnd) != 0)
    {
        // Open paths turn back at either end; closed ones wrap around.
        for (;;)
        {
            const int Next = Patrol.Point + (Patrol.bReverse ? -1 : 1);
            if (Next >= 0 && Next < Count)
            {
                Patrol.Point = Next;
                break;
            }

            if (Patrol.bClosed)
            {
                Patrol.Point = Patrol.bReverse ? Count - 1 : 0;
                break;
            }

            Patrol.bReverse = !Patrol.bReverse;
        }
    }

    const std::array<int32_t, 2>& Point = Path.Points[size_t(Patrol.Point)];
    Patrol.Goal = { .X = float(Point[0]), .Y = float(Point[1]) };
    return Patrol.Work.Search.Start(Body.State.Center, CollisionFromWorld(Patrol.Goal));
}

// FUN_0042e040: a destination drawn at random from open ground in the window around the actor, retrying the last one once if its route
// did not reach its last segment.
// ponytail: the multiplayer reseed of the generator (session flags 0x18000) and the object check inside FUN_0042c130 are not ported.
bool StartRandomLeg(FRandomMove& Move, const FObjectBody& Body, const FTerrainCollision& Terrain, FRandom& Random)
{
    const bool bRouteDone = Move.Work.Route && OnLastSegment(Move.Work);
    if (!bRouteDone)
    {
        if (Move.Goal.X != 0.f && Move.Goal.Y != 0.f && ++Move.Retries < 2)
        {
            return Move.Work.Search.Start(Body.State.Center, CollisionFromWorld(Move.Goal));
        }

        Move.Retries = 0;
    }

    int Left = 0;
    int Top = 0;
    int Right = Terrain.GetMarkerWidth() - 1;
    int Bottom = Terrain.GetMarkerHeight() - 1;
    if (const int Range = int(Move.Range); Range != 0)
    {
        const FWorldCell Cell = CellOf(Body.State.Position);
        const int HalfX = (Range + int(BlockWidth) / 2) / int(BlockWidth);
        const int HalfY = (Range + int(BlockHeight) / 2) / int(BlockHeight);
        Left = std::max(Cell.X - HalfX, 0);
        Top = std::max(Cell.Y - HalfY, 0);
        Right = std::min(Cell.X + HalfX, Right);
        Bottom = std::min(Cell.Y + HalfY, Bottom);
    }

    constexpr int Attempts = 10;
    constexpr int OpenMargin = 2;
    for (int Attempt = 0; Attempt < Attempts; ++Attempt)
    {
        const int X = Random.Range(Left, Right);
        const int Y = Random.Range(Top, Bottom);
        if (Terrain.IsOpenGround(X, Y, OpenMargin))
        {
            Move.Goal = { .X = float(X * int(BlockWidth) + int(BlockWidth) / 2), .Y = float(Y * int(BlockHeight) + int(BlockHeight) / 2) };
            return Move.Work.Search.Start(Body.State.Center, CollisionFromWorld(Move.Goal));
        }
    }

    return false;
}

// PatrolMoveUpdate at 0042e400 and RandomMoveUpdate at 0042dd20: plan a leg every Scan-delay or Delay, search for its route and follow it.
template <typename FStartLeg>
void UpdateMover(FObjectBody& Body, FRouteWork& Work, const uint32_t Mode, const float MaxDistance, const FStartLeg& StartLeg,
    std::optional<std::vector<FPathObstacle>>& Obstacles, const FTerrainCollision& Terrain, const int32_t Step)
{
    FActor& Actor = *Body.Actor;
    if (Work.bOnSegment)
    {
        if (!KeepSegment(Body, Work))
        {
            Work.Timer = 0;
            return;
        }

        if (Work.Timer >= 1)
        {
            Work.Timer -= Step;
            Engage(nullptr, Body, Work, nullptr, nullptr, Step);
        }
        else
        {
            // The route is re-planned at every interval, even toward the same point.
            Work.bOnSegment = false;
        }

        Actor.Mode = Mode;
        return;
    }

    Actor.RouteFlags &= ~RouteFollowing;
    Actor.Mode = Mode;
    if (!Work.bSearching)
    {
        if (Work.Timer >= 1)
        {
            Work.Timer -= Step;
            return;
        }

        Work.Timer += Work.Interval;
        if (StartLeg())
        {
            Work.SearchTimer = 0;
            Work.bSearching = true;
        }
        else
        {
            Actor.RouteFlags &= ~RouteMovementAllowed;
        }

        return;
    }

    Work.SearchTimer -= Step;
    if (Work.SearchTimer > 0)
    {
        return;
    }

    Work.SearchTimer += RouteSearchPeriodMilliseconds;
    bool bRouted = false;
    const EPathStatus Status = AdvanceRoute(Body, Work, Obstacles, Terrain, MaxDistance, bRouted);
    if (Status == EPathStatus::Searching)
    {
        Work.Timer -= Step;
        return;
    }

    Work.bOnSegment = bRouted;
    Actor.RouteFlags = bRouted ? Actor.RouteFlags | RouteFollowing | RouteMovementAllowed : Actor.RouteFlags & ~RouteMovementAllowed;
    Work.bSearching = false;
    Work.Timer += Work.Interval;
}
}

void StepActors(FCombat* const Combat, const std::span<FObjectBody> Bodies, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain,
    const FLevel& Level, const FPlayerState* const Player)
{
    const int32_t Step = int32_t(StepMilliseconds);
    std::optional<std::vector<FPathObstacle>> Obstacles;
    for (FObjectBody& Body : Bodies)
    {
        // A wreck's behaviour is plain Fixed, which neither moves nor looks for a target.
        if (!Body.Actor || Level.Objects[Body.ObjectIndex].bRemoved || Level.Objects[Body.ObjectIndex].bWrecked)
        {
            continue;
        }

        // EngineUpdateObjects (004406e0) clears the command and the segment pull before the behaviour updates.
        FActor& Actor = *Body.Actor;
        Actor.Command = {};
        Actor.bTrigger = false;
        if (Actor.Seeker)
        {
            UpdateSeeker(Combat, Body, *Actor.Seeker, Player, Obstacles, Terrain, Step);
        }

        // The movement behaviour pauses while the seeker follows its own route.
        const bool bSeekerRoute = (Actor.RouteFlags & RouteFollowing) != 0 && (Actor.RouteFlags & RouteMovementAllowed) == 0;
        if (Actor.Patrol && !bSeekerRoute)
        {
            FPatrol& Patrol = *Actor.Patrol;
            UpdateMover(Body, Patrol.Work, BehaviourPatrol, 0.f, [&] { return StartPatrolLeg(Patrol, Body, Level); }, Obstacles, Terrain, Step);
        }
        else if (Actor.RandomMove && Combat && !bSeekerRoute)
        {
            FRandomMove& Move = *Actor.RandomMove;
            UpdateMover(Body, Move.Work, BehaviourRandom, Move.Range, [&] { return StartRandomLeg(Move, Body, Terrain, Combat->Random); }, Obstacles, Terrain, Step);
        }

        // SeekerUpdate leaves a spider without thrust; only its patrol moves it.
        if (Actor.bSpider && (Actor.Mode == ModeSeeker || Actor.Mode == ModeTracker))
        {
            Actor.Command.Thrust = EPlayerThrust::None;
        }

        // ObjectComputeDisplacement (00438b20) adds the segment pull only for patrol and random movers.
        if (Actor.Mode != BehaviourPatrol && Actor.Mode != BehaviourRandom)
        {
            Actor.Command.Correction = {};
        }
        else
        {
            Actor.Command.Correction = { .X = Actor.Command.Correction.X * Actor.CorrectionScale, .Y = Actor.Command.Correction.Y * Actor.CorrectionScale };
        }

        // A pushed actor drifts without thrust until it slows down.
        if (Body.State.bPushed)
        {
            Actor.Command.Thrust = EPlayerThrust::None;
        }

        // ObjectUpdateWeapon (00444ca0) fires once the trigger is pulled.
        if (Combat && Actor.bTrigger && Actor.Seeker && Actor.Seeker->Weapon)
        {
            FSeeker& Seeker = *Actor.Seeker;
            const FShooter Shooter{
                .Position = Body.State.Position,
                .Velocity = Body.State.Velocity,
                .Direction = AimDirection(Body),
                .Altitude = int(Body.Altitude),
                // ponytail: the muzzle range uses twice the bounding radius instead of the sprite bounds, as for turrets.
                .SpriteSize = Body.Class.Radius * 2.f,
                .ObjectIndex = Body.ObjectIndex,
            };
            FireWeapon(*Combat, Seeker.WeaponState, Shooter, &Seeker.Ammo);
        }

        StepPlayer(Body.State, Body.Class, Actor.Command, StepMilliseconds, Terrain, Level, Bodies, &Body);
    }
}

void StepTurrets(FCombat& Combat, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerState& Player)
{
    const int32_t Step = int32_t(StepMilliseconds);
    std::optional<std::vector<FPathObstacle>> Obstacles;
    for (FObjectBody& Body : Bodies)
    {
        if (!Body.Turret || Level.Objects[Body.ObjectIndex].bWrecked)
        {
            continue;
        }

        FTurret& Turret = *Body.Turret;
        const FWeaponClass& Weapon = Combat.Weapons[Turret.Weapon.Weapon];
        const FCollisionPoint ToTarget{ .X = float(double(Player.Center.X) - double(Body.State.Center.X)), .Y = float(double(Player.Center.Y) - double(Body.State.Center.Y)) };
        const float DistanceSquared = float(double(ToTarget.X) * double(ToTarget.X) + double(ToTarget.Y) * double(ToTarget.Y));
        Turret.bTrigger = false;
        bool bEngaged = false;
        if (Turret.bSearching)
        {
            // SeekerUpdate at 0042c5a0 advances searches every 17 ms; fixed seekers stop on the first detour.
            constexpr int32_t SearchPeriodMilliseconds = 17;
            Turret.SearchTimer -= Step;
            if (Turret.SearchTimer <= 0)
            {
                Turret.SearchTimer += SearchPeriodMilliseconds;
                if (!Obstacles)
                {
                    Obstacles = TerrainPathObstacles(Terrain);
                }

                const FCollisionPoint HalfExtent{ .X = Body.Class.Radius, .Y = Body.Class.Radius };
                const EPathStatus Status = Turret.Search.Advance(*Obstacles, HalfExtent, Turret.SensorRange);
                if (Status == EPathStatus::Found || Status == EPathStatus::Finished)
                {
                    const std::optional<FPathRoute> Route = Turret.Search.SelectFinishedRoute(*Obstacles, HalfExtent);
                    if (Route && Route->Points.size() == 2)
                    {
                        const FCollisionPoint Segment{ .X = Route->Points[1].X - Route->Points[0].X, .Y = Route->Points[1].Y - Route->Points[0].Y };
                        const int Direction = FindDirection(Segment);
                        // SeekerValidateRoute at 0042ca30 accepts only the four collision-axis sectors for turrets.
                        constexpr int AxisDirectionStep = DirectionCount / 4;
                        Turret.bLocked = Direction != NoDirection && Direction % AxisDirectionStep == 0;
                        Turret.AimDirection = Direction;
                    }

                }

                Turret.ScanTimer += Turret.ScanDelay;
                Turret.bSearching = false;
            }
        }
        else if (!Turret.bLocked)
        {
            // SeekerFindTarget (0042da80) looks for the target within Sensor-Range and the vision cone around the base's facing; PathStartSearch (0040f6f0) routes to it.
            if (Turret.ScanTimer < 1)
            {
                Turret.ScanTimer += Turret.ScanDelay;
                const bool bInRange = DistanceSquared <= float(int(Turret.SensorRange) * int(Turret.SensorRange));
                const bool bInView = DirectionDistance(FindDirection(ToTarget), Turret.BaseDirection) <= Turret.VisionDirections;
                if (bInRange && bInView && !Player.bInvisible && Turret.Search.Start(Body.State.Center, Player.Center))
                {
                    Turret.bSearching = true;
                    Turret.SearchTimer = 0;
                    Turret.LockedTarget = Player.Position;
                }
            }
            else
            {
                Turret.ScanTimer -= Step;
            }
        }
        else if (Turret.ScanTimer < 1)
        {
            // At every scan a moved target breaks the lock; the next step scans again.
            if (Player.Position.X != Turret.LockedTarget.X || Player.Position.Y != Turret.LockedTarget.Y)
            {
                Turret.bLocked = false;
            }
            else
            {
                Turret.ScanTimer += Turret.ScanDelay;
            }
        }
        else
        {
            Turret.ScanTimer -= Step;
            // SeekerEngage (0042caa0): within Min-distance the top turns toward the line to the target (the previous step's direction), then the line is refreshed.
            const float MinDistance = float(uint32_t(int(Turret.MinDistance) * int(Turret.MinDistance)));
            bEngaged = DistanceSquared <= MinDistance;
            if (bEngaged)
            {
                TurnTurretTop(Turret, StepMilliseconds);
                const int Direction = FindDirection(ToTarget);
                if (Direction != NoDirection)
                {
                    Turret.AimDirection = Direction;
                }

                // SeekerAimAndFire (0042d2d0): inside Fire-Range, with the weapon ready and the top on target, the trigger is pulled.
                const float FireRange = float(int(Weapon.FireRange));
                if (uint32_t(int64_t(DistanceSquared)) <= uint32_t(int64_t(FireRange * FireRange)))
                {
                    Turret.bTrigger = WeaponReady(Combat, Turret.Weapon) && Direction == Turret.Direction;
                    UpdateSlope(Turret.Weapon, Weapon, true);
                }
                else
                {
                    bEngaged = false;
                }
            }
        }

        if (!bEngaged)
        {
            UpdateSlope(Turret.Weapon, Weapon, false);
        }

        if (Turret.bTrigger)
        {
            const FShooter Shooter{
                .Position = Body.State.Position,
                .Direction = Turret.Direction,
                .Altitude = int(Body.Altitude),
                // ponytail: the muzzle range uses twice the bounding radius instead of the turret's sprite bounds.
                .SpriteSize = Body.Class.Radius * 2.f,
                .ObjectIndex = Body.ObjectIndex,
            };
            FireWeapon(Combat, Turret.Weapon, Shooter, &Turret.Ammo);
        }
    }
}

namespace
{
struct FDrop
{
    std::string ClassName;
    float X = 0.f;
    float Y = 0.f;
};

// ObjectDropContents at 00437f80: a destroyed object leaves its class's HAVE objects, then each RANDOM one whose draw RandomRange(1, Out-of)
// does not exceed its Chance. The class loader prepends both lists, so the last declared, and the derived class's before its parent's, come
// first. The first object lands on the wreck and the others around it, alternately left and right and then in rows 10 pixels apart.
// ponytail: objects carried at object +0x11c are not dropped, and a demo recording or replay would skip the RANDOM draws.
void DropContents(FRandom& Random, const FLevelObject& Object, std::vector<FDrop>& Drops)
{
    constexpr int32_t FirstRing = 0x20;
    constexpr int32_t RingStep = 10;
    struct FEntry
    {
        std::string ClassName;
        int Chance = 0;
        int OutOf = 0;
    };
    std::vector<FEntry> Have;
    std::vector<FEntry> Chances;
    for (const auto& [Key, Value] : Object.Properties.Fields | std::views::reverse)
    {
        std::istringstream Words{ std::string(Key) };
        std::string Directive;
        FEntry Entry;
        Words >> Directive >> Entry.ClassName;
        if (ToLowerName(Directive) == "have" && !Entry.ClassName.empty())
        {
            Have.push_back(std::move(Entry));
        }
        else if (ToLowerName(Directive) == "random" && Words >> Entry.Chance >> Entry.OutOf)
        {
            Entry.Chance = std::abs(Entry.Chance);
            Entry.OutOf = std::abs(Entry.OutOf);
            Chances.push_back(std::move(Entry));
        }
    }

    const int32_t BaseX = TruncateLow(Object.X);
    const int32_t BaseY = TruncateLow(Object.Y);
    bool bFirst = true;
    int32_t Ring = FirstRing;
    int32_t Previous = 0;
    int32_t RowY = 0;
    const auto Place = [&](int32_t& Left, const std::string& ClassName)
    {
        int32_t OffsetX = 0;
        if (bFirst)
        {
            bFirst = false;
        }
        else
        {
            OffsetX = Left;
            if (Previous < 0)
            {
                OffsetX = -Previous;
            }
            else if (Previous > 0)
            {
                Ring += RingStep;
                Left -= RingStep;
                OffsetX = 0;
                if (RowY == 0)
                {
                    RowY = -RingStep;
                }
                else if (RowY < 0)
                {
                    RowY = -RowY;
                }
                else
                {
                    RowY = 0;
                    OffsetX = Left;
                }
            }

            Previous = OffsetX;
        }

        Drops.push_back({ .ClassName = ClassName, .X = float(BaseX + OffsetX), .Y = float(BaseY + RowY) });
    };

    int32_t Left = -Ring;
    for (const FEntry& Entry : Have)
    {
        Place(Left, Entry.ClassName);
    }

    Left = -Ring;
    for (const FEntry& Entry : Chances)
    {
        if (Random.Range(1, Entry.OutOf) <= Entry.Chance)
        {
            Place(Left, Entry.ClassName);
        }
    }
}
}

void StepProjectiles(FCombat& Combat, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerTarget& Player, FMessages& Messages)
{
    const float TimeScale = float(ReferenceStepMilliseconds / double(StepMilliseconds));
    constexpr FPlayerCommand Straight{ .Thrust = EPlayerThrust::Forward };
    std::erase_if(Combat.Projectiles, [&](FProjectile& Shot)
    {
        const FAmmoClass& Ammo = Combat.AmmoClasses[Shot.AmmoIndex];
        // Target is the object struck, bPlayer the player struck; bSurface marks a wall or other terrain contact.
        const auto Detonate = [&](FObjectBody* const Target, const bool bPlayer, const bool bSurface)
        {
            // AreaDamage and AmmoHit add every object a player's shot finishes off to the player's kills.
            std::vector<bool> WasAlive;
            if (Shot.Owner == Player.ObjectIndex)
            {
                std::ranges::transform(Bodies, std::back_inserter(WasAlive), [](const FObjectBody& Body) { return 0.f < Body.Energy; });
            }

            // AmmoHit at 0043d7d0 blasts around the shot's last committed centre, then AmmoImpact (0043d350) shoves and damages what it struck.
            AreaDamage(Shot.State.Center, CellOf(Shot.State.Position), Ammo.Damage, Ammo.DamageZone, Bodies, Player);
            if (Target)
            {
                constexpr double ImpactPush = 40.;
                const FCollisionPoint Facing = FacingVector(Shot.State.Direction);
                Target->State.Velocity.X = float(double(Facing.X) * ImpactPush / double(Target->Class.Weight) + double(Target->State.Velocity.X));
                Target->State.Velocity.Y = float(double(Facing.Y) * ImpactPush / double(Target->Class.Weight) + double(Target->State.Velocity.Y));
                const auto Absorption = std::ranges::find(Target->Absorptions, Ammo.Name, &FAbsorption::AmmoClass);
                ApplyDamage(*Target, Absorption == Target->Absorptions.end() ? Ammo.Damage : float(double(Ammo.Damage) * double(Absorption->Coeff)));
            }

            // ponytail: the player's own shove and absorption blocks are not ported.
            if (bPlayer)
            {
                // ApplyDamage spares an invisible player and an invulnerable object (flags 8 and 0x800000).
                if (!Player.State.bInvisible && !Player.Inventory.Surprises.bInvulnerable)
                {
                    ApplyDamage(Player.Inventory.Energy, Player.Inventory.Shield, Ammo.Damage);
                }
            }

            for (size_t Index = 0; Index < WasAlive.size(); ++Index)
            {
                Player.Inventory.Kills += WasAlive[Index] && Bodies[Index].Energy <= 0.f ? 1 : 0;
            }

            // AmmoImpact puts the Hit-fx at the contact point; a shot that sank to the ground leaves a ground impact instead.
            // ponytail: water splashes and the check that the spot is on walkable terrain are not ported.
            if (Target || bPlayer || bSurface)
            {
                const FCollisionPoint Spot{
                    .X = std::clamp(Shot.State.Position.X, 0.f, float(Level.WorldExtentX) - 1.f),
                    .Y = std::clamp(Shot.State.Position.Y, 0.f, float(Level.WorldExtentY) - 1.f),
                };
                SpawnEffect(Combat, Ammo.HitEffect, Spot, float(Shot.Altitude));
            }
            else if (Shot.Altitude < 1)
            {
                SpawnEffect(Combat, GroundImpactEffect, Shot.State.Position, 0.f);
            }

            return true;
        };

        // Sloped bullets sink by sin(slope) of their altitude each step and burst on the ground (the Ammo branch of EngineUpdateObjects).
        if (Shot.bSloped)
        {
            Shot.AltitudeAccumulator = float(std::sin(double(Shot.Slope)) * double(Shot.Altitude) + double(Shot.AltitudeAccumulator));
            Shot.Altitude = int(Shot.AltitudeAccumulator);
            if (Shot.Altitude < 1)
            {
                Shot.Altitude = 0;
                return Detonate(nullptr, false, false);
            }
        }

        if (Shot.DamageTimer != 0)
        {
            if (Shot.DamageTimer <= StepMilliseconds)
            {
                return Detonate(nullptr, false, false);
            }

            Shot.DamageTimer -= StepMilliseconds;
        }

        const FCollisionPoint Delta = Displacement(Shot.State, Ammo.Movement, Straight, TimeScale);
        if (IsZero(Delta))
        {
            return false;
        }

        const FCollisionPoint Target{ .X = float(double(Delta.X) + double(Shot.State.Center.X)), .Y = float(double(Delta.Y) + double(Shot.State.Center.Y)) };
        Shot.State.PreviousPosition = Shot.State.Position;
        Shot.State.Position = WorldFromCollision(Target);
        // Objects other than the player are dropped once they leave the level.
        constexpr float LowerLimit = 1.f;
        if (Shot.State.Position.X < LowerLimit || float(Level.WorldExtentX) <= Shot.State.Position.X || Shot.State.Position.Y < LowerLimit ||
            float(Level.WorldExtentY) <= Shot.State.Position.Y)
        {
            return true;
        }

        const FCollisionPoint Candidate = CollisionFromWorld(Shot.State.Position);
        const FCollisionCircle Moved{ .Center = Candidate, .Radius = Ammo.Movement.Radius };
        const std::optional<FTerrainHit> TerrainHit = QueryTerrainCollision(Terrain, { .Center = Shot.State.Center, .Radius = Ammo.Movement.Radius }, Moved);
        std::optional<FCollisionPoint> Contact = TerrainHit ? std::optional(TerrainHit->Point) : std::nullopt;
        // ponytail: bouncing ammo (on-hit = 0: grenades, saws, pulsars) bursts on first contact too.
        FObjectBody* const Struck = FindStruckBody(Bodies, Moved, Shot.Owner, Contact);
        bool bPlayerStruck = false;
        if (Shot.Owner != Player.ObjectIndex)
        {
            const std::optional<FCollisionPoint> PlayerContact = IntersectCircles(Moved, { .Center = Player.State.Center, .Radius = Player.Radius });
            if (PlayerContact && (!Contact || PrefersContact(*Contact, *PlayerContact, Candidate)))
            {
                Contact = PlayerContact;
                bPlayerStruck = true;
            }
        }

        if (!Contact)
        {
            Shot.State.Center = Candidate;
            return false;
        }

        Shot.State.Position = WorldFromCollision(*Contact);
        return Detonate(bPlayerStruck ? nullptr : Struck, bPlayerStruck, !bPlayerStruck && Struck == nullptr);
    });

    // The FX branch of ObjectUpdateWeapon (00444ca0): before half its animation an explosion waits out its delay, then the blast goes off once.
    std::erase_if(Combat.Effects, [&](FEffect& Effect)
    {
        const FEffectClass& Class = Combat.EffectClasses[Effect.ClassIndex];
        if (Effect.Damage != 0.f)
        {
            bool bBlast = true;
            if (Effect.Frame < uint32_t(Class.FrameDurations.size()) / 2)
            {
                if (Effect.BlastTimer < 1)
                {
                    bBlast = false;
                }
                else if (int32_t(StepMilliseconds) < Effect.BlastTimer)
                {
                    Effect.BlastTimer -= int32_t(StepMilliseconds);
                    bBlast = false;
                }
            }

            if (bBlast)
            {
                Effect.BlastTimer = -1;
                AreaDamage(CollisionFromWorld(Effect.Position), CellOf(Effect.Position), Effect.Damage, Effect.Zone, Bodies, Player);
                Effect.Damage = 0.f;
            }
        }

        AdvanceEffect(Effect, Class, StepMilliseconds);
        return Effect.bFinished;
    });

    // Session +0x2094 counts the hunters down as they die; the last one announces the empty level.
    const auto IsHunter = [](const FObjectBody& Body) { return Body.Turret || (Body.Actor && Body.Actor->Seeker) || (Body.Generator && Body.Generator->bProductHunts); };
    int Hunters = int(std::ranges::count_if(Bodies, [&](const FObjectBody& Body) { return IsHunter(Body) && !Level.Objects[Body.ObjectIndex].bRemoved; }));
    // The dropped objects join the level once the destroyed ones are gone, so no object reference outlives the list growing.
    std::vector<FDrop> Drops;
    std::erase_if(Bodies, [&](FObjectBody& Body)
    {
        // Objects die in their next update once damage has taken their energy to zero.
        if (!Body.bDamaged || 0.f < Body.Energy)
        {
            return false;
        }

        FLevelObject& Object = Level.Objects[Body.ObjectIndex];
        // A Two-pieces object's first death keeps it as a wreck (ObjectDestroy with its keep flag): it explodes and pays its reward, but drops
        // nothing and still counts as an enemy.
        const bool bFirstOfTwo = !Object.bWrecked && ParseLevelFloat(Object.Properties.Find("Two-pieces")).value_or(0.f) != 0.f;
        Object.bRemoved = !bFirstOfTwo;
        if (!bFirstOfTwo)
        {
            DropContents(Combat.Random, Object, Drops);
            if (Body.Parent)
            {
                const auto Generator = std::ranges::find(Bodies, *Body.Parent, &FObjectBody::ObjectIndex);
                if (Generator != Bodies.end() && Generator->Generator && Generator->Generator->bStablePopulation && !Level.Objects[*Body.Parent].bRemoved)
                {
                    --Generator->Generator->Births;
                }
            }
        }

        if (!bFirstOfTwo && IsHunter(Body) && --Hunters == 0)
        {
            // The player's name at session +0x2754 starts as Jack. ponytail: the 100000 bonus is only printed here.
            constexpr int32_t LastKillX = 0x46;
            constexpr int32_t LastKillY = (MessageViewHeight >> 1) - 0x66;
            constexpr uint32_t LastKillLifeMilliseconds = 10000;
            constexpr int RealKillerBonus = 100000;
            constexpr uint32_t Font = 6;
            Messages.Add({
                .Text = std::format("You are so professional Jack !\nNo more enemy in this level !\n  Real Killer Bonus : +{} $", RealKillerBonus),
                .X = float(LastKillX),
                .Y = float(LastKillY),
                .LifeMilliseconds = LastKillLifeMilliseconds,
                .Font = Font,
            }, EMessagePlacement::Replace);
        }

        // ObjectDestroy (00437d40) hands each TYPE Explosion block to a new Boom-fx object at the same spot.
        // ponytail: two-piece wrecks that stay behind and surprise drops are not ported.
        for (const FExplosionBlock& Explosion : Body.Explosions)
        {
            SpawnEffect(Combat, Explosion.BoomEffect, Body.State.Position, Body.Altitude, Explosion.Damage, Explosion.Zone);
        }

        if (0.f < Body.Reward && PaysReward(Object.SubType))
        {
            Player.Inventory.Money += Body.Reward;
            // The reward rises from the wreck in font 2 and the object's name follows it in font 6.
            constexpr float RewardVelocity = -1.5f;
            constexpr float NameVelocity = -1.f;
            constexpr int32_t NameOffset = -0x20;
            constexpr uint32_t LifeMilliseconds = 2000;
            constexpr uint32_t RewardFont = 2;
            constexpr uint32_t NameFont = 6;
            constexpr uint32_t RewardColor = 0xf0ff10;
            const int32_t X = TruncateLow(Body.State.Position.X);
            const int32_t Y = TruncateLow(Body.State.Position.Y);
            Messages.Add({
                .Text = std::format("{} $", int32_t(Body.Reward)),
                .X = float(X),
                .Y = float(Y),
                .VelocityY = RewardVelocity,
                .LifeMilliseconds = LifeMilliseconds,
                .bWorld = true,
                .bTypewriter = true,
                .Font = RewardFont,
                .Color = { .Diffuse = RewardColor },
            }, EMessagePlacement::Replace);
            const std::string_view Name = Object.Properties.Find("Name");
            Messages.Add({
                .Text = std::string(Name.empty() ? std::string_view(Object.ClassName) : Name),
                .X = float(X + NameOffset),
                .Y = float(Y + NameOffset),
                .VelocityY = NameVelocity,
                .LifeMilliseconds = LifeMilliseconds,
                .bWorld = true,
                .bTypewriter = true,
                .Font = NameFont,
            }, EMessagePlacement::Replace);
        }

        if (bFirstOfTwo)
        {
            // The wreck starts over with the class's initial energy and shield.
            const auto Initial = [&](const std::string_view Kind)
            {
                const FLevelPropertyBlock* const Block = FindBlock(Object.Properties, Kind);
                return Block ? ParseLevelFloat(Block->Find("Initial")).value_or(0.f) : 0.f;
            };
            Object.bWrecked = true;
            Body.Energy = Initial("Energy");
            Body.Shield = Initial("Shield");
            Body.bDamaged = false;
            return false;
        }

        return true;
    });

    for (const FDrop& Drop : Drops)
    {
        if (const std::optional<size_t> Index = CreateLevelObject(Level, Drop.ClassName, Drop.X, Drop.Y))
        {
            Combat.CreatedObjects.push_back(*Index);
        }
    }
}

std::expected<std::string, std::string> VerifyPatrols(FResourceArchive& Archive)
{
    // Every level's cops patrol for half a minute; each one with a path must reach at least its second patrol point.
    constexpr uint32_t StepMilliseconds = 14;
    constexpr uint32_t DurationMilliseconds = 30000;
    size_t Patrollers = 0;
    size_t Advanced = 0;
    size_t Wanderers = 0;
    size_t Destined = 0;
    size_t Wandered = 0;
    std::vector<std::string> Stuck;
    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".3do"))
        {
            continue;
        }

        std::expected<FLevel, std::string> Level = LoadLevel(Archive, Name.substr(0, Name.size() - 4));
        const std::expected<FTerrainCollision, std::string> Terrain = Level ? BuildTerrainCollision(*Level) : std::unexpected(Level.error());
        if (!Terrain)
        {
            return std::unexpected(std::format("{}: {}", Name, Terrain.error()));
        }

        std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
        std::vector<int> FirstPoints(Bodies.size(), -1);
        std::vector<size_t> Visits(Bodies.size(), 0);
        // Random movers draw their destinations from the level's generator; nothing here has a weapon.
        FCombat Moves;
        for (uint32_t Time = 0; Time < DurationMilliseconds; Time += StepMilliseconds)
        {
            StepActors(&Moves, Bodies, StepMilliseconds, *Terrain, *Level, nullptr);
            for (size_t Index = 0; Index < Bodies.size(); ++Index)
            {
                const std::optional<FPatrol>& Patrol = Bodies[Index].Actor ? Bodies[Index].Actor->Patrol : std::nullopt;
                if (Patrol && Patrol->bStarted && FirstPoints[Index] != Patrol->Point)
                {
                    FirstPoints[Index] = FirstPoints[Index] < 0 ? Patrol->Point : FirstPoints[Index];
                    Visits[Index] += 1;
                }
            }
        }

        for (size_t Index = 0; Index < Bodies.size(); ++Index)
        {
            const FObjectBody& Body = Bodies[Index];
            if (Body.Actor && Body.Actor->RandomMove)
            {
                const FLevelObject& Object = Level->Objects[Body.ObjectIndex];
                const float X = Body.State.Position.X - Object.X;
                const float Y = Body.State.Position.Y - Object.Y;
                constexpr float CellDistance = float(BlockWidth);
                // FUN_0042e040 only accepts a destination inside open ground; corridor cops often find none and wait for a target.
                const bool bHasGoal = Body.Actor->RandomMove->Goal.X != 0.f;
                ++Wanderers;
                Destined += bHasGoal ? 1 : 0;
                Wandered += bHasGoal && X * X + Y * Y >= CellDistance * CellDistance ? 1 : 0;
            }

            if (!Body.Actor || !Body.Actor->Patrol || !Body.Actor->Patrol->Path)
            {
                continue;
            }

            ++Patrollers;
            if (Visits[Index] > 1 || Body.Actor->Patrol->Point != FirstPoints[Index])
            {
                ++Advanced;
            }
            else
            {
                Stuck.push_back(std::format("{} {} at {:.0f},{:.0f}", Name, Level->Objects[Body.ObjectIndex].ClassName, Body.State.Position.X, Body.State.Position.Y));
            }
        }
    }

    const std::string Summary = std::format("{} of {} patrolling cops moved on to their next point, {} of {} random movers found open ground and {} of those left their cell",
        Advanced, Patrollers, Destined, Wanderers, Wandered);

    if (Patrollers == 0 || Advanced * 2 < Patrollers || Destined == 0 || Wandered * 2 < Destined)
    {
        return std::unexpected(std::format("{}; stuck: {}", Summary, Stuck.empty() ? std::string() : Stuck.front()));
    }

    return Summary;
}

std::expected<std::string, std::string> VerifyDoors(FResourceArchive& Archive)
{
    // The check ignores the floating messages it raises.
    FMessages Messages;
    constexpr uint32_t StepMilliseconds = 14;
    constexpr uint32_t SettleMilliseconds = 12000;
    constexpr uint32_t RideMilliseconds = 30000;
    size_t Gates = 0;
    size_t Closed = 0;
    size_t Opened = 0;
    size_t Lifts = 0;
    size_t Ridden = 0;
    std::vector<std::string> Failures;
    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".3do"))
        {
            continue;
        }

        std::expected<FLevel, std::string> Level = LoadLevel(Archive, Name.substr(0, Name.size() - 4));
        if (!Level)
        {
            return std::unexpected(std::format("{}: {}", Name, Level.error()));
        }

        const auto PlayerObject = std::ranges::find(Level->Objects, std::string_view("Player"), &FLevelObject::Behaviour);
        const std::expected<FMovementClass, std::string> PlayerClass = PlayerObject != Level->Objects.end() ? ReadMovementClass(*PlayerObject)
                                                                                                           : std::unexpected(std::string("no player"));
        if (!PlayerClass)
        {
            continue;
        }

        const size_t PlayerIndex = size_t(PlayerObject - Level->Objects.begin());
        const auto Run = [&](std::vector<FDoor>& Doors, std::vector<FObjectBody>& Bodies, const FCollisionPoint At, FPlayerInventory& Inventory, const uint32_t Duration)
        {
            FPlayerState Player = SpawnPlayer(*PlayerObject, *PlayerClass);
            Player.Position = At;
            Player.Center = CollisionFromWorld(At);
            for (uint32_t Time = 0; Time < Duration; Time += StepMilliseconds)
            {
                if (const FDoor* const Lift = StepDoors(Doors, *Level, Bodies, { .State = Player, .Radius = PlayerClass->Radius, .Inventory = Inventory,
                                                                                    .ObjectIndex = PlayerIndex }, StepMilliseconds, Messages))
                {
                    return std::optional(Lift->NextLevel);
                }
            }

            return std::optional<std::string>();
        };

        // With the player far away, sensing gates close once nothing stands in them; then the player opens each closed one alone.
        constexpr FCollisionPoint FarAway{ .X = -100000.f, .Y = -100000.f };
        std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
        std::expected<std::vector<FDoor>, std::string> Doors = CreateDoors(*Level, Archive, Bodies);
        if (!Doors)
        {
            return std::unexpected(std::format("{}: {}", Name, Doors.error()));
        }

        FPlayerInventory Inventory;
        Run(*Doors, Bodies, FarAway, Inventory, SettleMilliseconds);
        for (size_t Index = 0; Index < Doors->size(); ++Index)
        {
            const FDoor Settled = (*Doors)[Index];
            if (Settled.bLift || (Settled.Flags & DoorSensor) == 0)
            {
                continue;
            }

            ++Gates;
            if ((Settled.Flags & DoorOpen) != 0)
            {
                continue;
            }

            ++Closed;
            const FLevelObject& Gate = Level->Objects[Settled.ObjectIndex];
            std::vector<FDoor> Single = { Settled };
            Inventory.Keys = { FCarriedKey{ .Id = Settled.Id } };
            Run(Single, Bodies, { .X = Gate.X, .Y = Gate.Y }, Inventory, SettleMilliseconds);
            Opened += (Single.front().Flags & DoorOpen) != 0 ? 1 : 0;
            if ((Single.front().Flags & DoorOpen) == 0)
            {
                Failures.push_back(std::format("{} gate {} at {:.0f},{:.0f}", Name, Gate.ClassName, Gate.X, Gate.Y));
            }
        }

        // The player waits on each usable lift with its key until it leaves.
        for (const FDoor& Door : *Doors)
        {
            if (!Door.bLift || (Door.Flags & DoorActive) == 0 || Door.UsageCount < 1)
            {
                continue;
            }

            ++Lifts;
            std::vector<FObjectBody> LiftBodies = CreateObjectBodies(*Level);
            std::expected<std::vector<FDoor>, std::string> Fresh = CreateDoors(*Level, Archive, LiftBodies);
            const auto Lift = std::ranges::find(*Fresh, Door.ObjectIndex, &FDoor::ObjectIndex);
            std::vector<FDoor> Single = { *Lift };
            const FLevelObject& Object = Level->Objects[Door.ObjectIndex];
            FPlayerInventory Rider{ .Keys = { FCarriedKey{ .Id = Door.Id } } };
            const std::string LevelPath = Name.substr(0, Name.size() - 4);
            if (const std::optional<std::string> Next = Run(Single, LiftBodies, { .X = Object.X, .Y = Object.Y }, Rider, RideMilliseconds); Next == Door.NextLevel)
            {
                ++Ridden;
                if (!Door.NextLevel.starts_with('*') && !LoadLevel(Archive, NextLevelPath(LevelPath, Door)))
                {
                    Failures.push_back(std::format("{} lift leads to missing {}", Name, NextLevelPath(LevelPath, Door)));
                    --Ridden;
                }
            }
            else
            {
                Failures.push_back(std::format("{} lift at {:.0f},{:.0f}", Name, Object.X, Object.Y));
            }
        }
    }

    const std::string Summary = std::format("{} of {} sensing gates closed and {} of those opened for the player, {} of {} lifts carried the player off",
        Closed, Gates, Opened, Lifts, Ridden);
    if (Gates == 0 || Opened != Closed || Lifts == 0 || Ridden != Lifts)
    {
        return std::unexpected(std::format("{}; first failure: {}", Summary, Failures.empty() ? std::string() : Failures.front()));
    }

    return Summary;
}

int CountEnemies(const FLevel& Level, const std::span<const FObjectBody> Bodies)
{
    return int(std::ranges::count_if(Bodies, [&](const FObjectBody& Body)
    {
        const bool bHunter = Body.Turret || (Body.Actor && Body.Actor->Seeker) || (Body.Generator && Body.Generator->bProductHunts);
        return bHunter && 0.f < Body.Energy && !Level.Objects[Body.ObjectIndex].bRemoved;
    }));
}

std::expected<std::string, std::string> VerifyCombat(FResourceArchive& Archive)
{
    // The check ignores the floating messages it raises.
    FMessages Messages;
    // Every level's player spins on the spot for a few seconds with the trigger held, while the turrets that see it shoot back; each level must
    // load its weapons and the player must fire.
    constexpr uint32_t StepMilliseconds = 14;
    constexpr uint32_t DurationMilliseconds = 6000;
    constexpr float SpriteSize = 64.f;
    size_t Levels = 0;
    size_t Shots = 0;
    size_t TurretShots = 0;
    size_t Duels = 0;
    size_t DuelShots = 0;
    float DamageTaken = 0.f;
    size_t CopDuels = 0;
    size_t CopShots = 0;
    std::set<std::string> CopNames;
    size_t Destroyed = 0;
    size_t Dropped = 0;
    size_t Wrecks = 0;
    size_t Generators = 0;
    size_t GeneratorsBirthing = 0;
    size_t Effects = 0;
    // A direct cardinal route locks, a diagonal route is rejected, and an overlapping target aborts; each attempt refills the scan timer.
    const std::array<FCollisionPoint, 3> SearchTargets = {
        FCollisionPoint{ .X = 100.f, .Y = 200.f },
        FCollisionPoint{ .X = 200.f, .Y = 200.f },
        FCollisionPoint{ .X = 100.f, .Y = 100.f },
    };
    for (size_t Index = 0; Index < SearchTargets.size(); ++Index)
    {
        constexpr int32_t ScanDelay = 100;
        FCombat Check{ .Weapons = { FWeaponClass{} } };
        std::vector<FObjectBody> Bodies{ FObjectBody{
            .Class = { .Radius = DefaultCollisionRadius },
            .State = { .Center = { .X = 100.f, .Y = 100.f } },
            .Turret = FTurret{ .VisionDirections = DirectionCount / 2, .SensorRange = 1000.f, .ScanDelay = ScanDelay },
        } };
        const FPlayerState Target{ .Center = SearchTargets[Index] };
        const FTerrainCollision Terrain;
        // The turret's own level object.
        FLevel Level;
        Level.Objects.resize(1);
        StepTurrets(Check, StepMilliseconds, Terrain, Level, Bodies, Target);
        if (!Bodies.front().Turret->bSearching || Bodies.front().Turret->bLocked || Bodies.front().Turret->ScanTimer != ScanDelay)
        {
            return std::unexpected("Turret did not enter its pending route search");
        }

        StepTurrets(Check, StepMilliseconds, Terrain, Level, Bodies, Target);
        if (Bodies.front().Turret->bSearching || Bodies.front().Turret->bLocked != (Index == 0) || Bodies.front().Turret->ScanTimer != ScanDelay * 2)
        {
            return std::unexpected("Turret route acceptance or retry timer mismatch");
        }
    }

    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".3do"))
        {
            continue;
        }

        std::expected<FLevel, std::string> Level = LoadLevel(Archive, Name.substr(0, Name.size() - 4));
        if (!Level)
        {
            return std::unexpected(Level.error());
        }

        const auto PlayerObject = std::ranges::find_if(Level->Objects, [](const FLevelObject& Object) { return Object.Behaviour == "Player"; });
        if (PlayerObject == Level->Objects.end())
        {
            continue;
        }

        const size_t PlayerIndex = size_t(PlayerObject - Level->Objects.begin());
        std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
        const std::expected<FMovementClass, std::string> Class = ReadMovementClass(*PlayerObject);
        const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
        std::expected<FCombat, std::string> Combat = CreateCombat(*Level, *PlayerObject, Archive, Bodies);
        if (!Class || !Terrain || !Combat)
        {
            return std::unexpected(std::format("{}: {}", Name, !Class ? Class.error() : !Terrain ? Terrain.error() : Combat.error()));
        }

        FPlayerState Player = SpawnPlayer(*PlayerObject, *Class);
        FPlayerInventory Inventory = CreateInventory(*PlayerObject);
        const size_t BodyCount = Bodies.size();
        size_t LevelShots = 0;
        const FPlayerCommand Spin{ .Turn = EPlayerTurn::Left };
        for (uint32_t Time = 0; Time < DurationMilliseconds; Time += StepMilliseconds)
        {
            StepPlayer(Player, *Class, Spin, StepMilliseconds, *Terrain, *Level, Bodies);
            Combat->Clock += StepMilliseconds;
            const size_t Before = Combat->Projectiles.size();
            StepPlayerWeapon(*Combat, true, Player, *PlayerObject, PlayerIndex, SpriteSize, Inventory, Messages);
            LevelShots += Combat->Projectiles.size() - Before;
            const size_t TurretBefore = Combat->Projectiles.size();
            StepTurrets(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Player);
            TurretShots += Combat->Projectiles.size() - TurretBefore;
            const size_t EffectsBefore = Combat->Effects.size();
            StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex }, Messages);
            Effects += Combat->Effects.size() > EffectsBefore ? Combat->Effects.size() - EffectsBefore : 0;
        }

        if (LevelShots == 0)
        {
            return std::unexpected(std::format("{}: the player never fired", Name));
        }

        // Then the player waits in clear view of the first turret that has such a spot inside its weapon's range, and must draw fire.
        constexpr std::array<float, 2> DuelDistances = { 150.f, 250.f };
        constexpr uint32_t DuelMilliseconds = 3000;
        const std::vector<FPathObstacle> Obstacles = TerrainPathObstacles(*Terrain);
        std::optional<FCollisionPoint> Spot;
        for (const FObjectBody& Body : Bodies)
        {
            for (const float Distance : DuelDistances)
            {
                constexpr int AxisDirectionStep = DirectionCount / 4;
                for (int Direction = 0; Body.Turret && !Spot && Direction < DirectionCount; Direction += AxisDirectionStep)
                {
                    // Facing vectors are a tenth of a unit long.
                    constexpr float TenthsPerUnit = 10.f;
                    const FCollisionPoint Facing = FacingVector(Direction);
                    const float Scale = Distance * TenthsPerUnit;
                    const FCollisionPoint Candidate{ .X = Body.State.Center.X + Facing.X * Scale, .Y = Body.State.Center.Y + Facing.Y * Scale };
                    const FCollisionPoint World = WorldFromCollision(Candidate);
                    FPathSearch Search;
                    Search.Start(Body.State.Center, Candidate);
                    const EPathStatus Status = Search.Advance(Obstacles, { .X = Body.Class.Radius, .Y = Body.Class.Radius }, Body.Turret->SensorRange);
                    if (World.X > 1.f && World.Y > 1.f && World.X < float(Level->WorldExtentX) && World.Y < float(Level->WorldExtentY) &&
                        (Status == EPathStatus::Found || Status == EPathStatus::Finished) && DirectionDistance(Direction, Body.Turret->BaseDirection) <= Body.Turret->VisionDirections)
                    {
                        Spot = Candidate;
                    }
                }
            }
        }

        if (Spot)
        {
            Player.Center = *Spot;
            Player.Position = Player.PreviousPosition = WorldFromCollision(*Spot);
            Player.Velocity = {};
            const float EnergyBefore = Inventory.Energy + Inventory.Shield;
            for (uint32_t Time = 0; Time < DuelMilliseconds; Time += StepMilliseconds)
            {
                Combat->Clock += StepMilliseconds;
                const size_t TurretBefore = Combat->Projectiles.size();
                StepTurrets(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Player);
                DuelShots += Combat->Projectiles.size() - TurretBefore;
                StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex }, Messages);
            }

            ++Duels;
            DamageTaken += EnergyBefore - (Inventory.Energy + Inventory.Shield);
        }

        // A cop with a weapon must spot the player waiting in its line of fire inside Min-distance, route to it, turn and shoot.
        Spot.reset();
        const FObjectBody* Cop = nullptr;
        for (const FObjectBody& Body : Bodies)
        {
            const FSeeker* const Seeker = Body.Actor && Body.Actor->Seeker && Body.Actor->Seeker->Weapon ? &*Body.Actor->Seeker : nullptr;
            constexpr float InsideMinDistance = 0.75f;
            const float Distance = Seeker ? float(Seeker->Work.Radius) * InsideMinDistance : 0.f;
            for (int Direction = 0; Seeker && !Spot && Direction < DirectionCount; ++Direction)
            {
                constexpr float TenthsPerUnit = 10.f;
                const FCollisionPoint Facing = FacingVector(Direction);
                const FCollisionPoint Candidate{ .X = Body.State.Center.X + Facing.X * Distance * TenthsPerUnit, .Y = Body.State.Center.Y + Facing.Y * Distance * TenthsPerUnit };
                const FCollisionPoint World = WorldFromCollision(Candidate);
                FPathSearch Search;
                Search.Start(Body.State.Center, Candidate);
                const EPathStatus Status = Search.Advance(Obstacles, { .X = Body.Class.Radius, .Y = Body.Class.Radius }, Seeker->SensorRange);
                if (World.X > 1.f && World.Y > 1.f && World.X < float(Level->WorldExtentX) && World.Y < float(Level->WorldExtentY) &&
                    (Status == EPathStatus::Found || Status == EPathStatus::Finished))
                {
                    Spot = Candidate;
                    Cop = &Body;
                }
            }
        }

        if (Spot)
        {
            Player.Center = *Spot;
            Player.Position = Player.PreviousPosition = WorldFromCollision(*Spot);
            Player.Velocity = {};
            for (uint32_t Time = 0; Time < DuelMilliseconds; Time += StepMilliseconds)
            {
                Combat->Clock += StepMilliseconds;
                const size_t Before = Combat->Projectiles.size();
                StepActors(&*Combat, Bodies, StepMilliseconds, *Terrain, *Level, &Player);
                CopShots += Combat->Projectiles.size() - Before;
                StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex }, Messages);
            }

            ++CopDuels;
            CopNames.insert(Level->Objects[Cop->ObjectIndex].ClassName);
        }

        ++Levels;
        Shots += LevelShots;
        Destroyed += BodyCount - Bodies.size();

        // The first live turret is finished off twice: the first death must leave a wreck that stays, the second must remove it.
        const auto Turret = std::ranges::find_if(Bodies, [&](const FObjectBody& Body) { return Body.Turret && !Level->Objects[Body.ObjectIndex].bWrecked; });
        if (Turret != Bodies.end() && ParseLevelFloat(Level->Objects[Turret->ObjectIndex].Properties.Find("Two-pieces")).value_or(0.f) != 0.f)
        {
            const size_t TurretIndex = Turret->ObjectIndex;
            const FPlayerTarget Target{ .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
            const auto Kill = [&]
            {
                const auto Body = std::ranges::find(Bodies, TurretIndex, &FObjectBody::ObjectIndex);
                if (Body != Bodies.end())
                {
                    Body->Energy = 0.f;
                    Body->bDamaged = true;
                }

                StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Target, Messages);
            };
            Kill();
            if (!Level->Objects[TurretIndex].bWrecked || Level->Objects[TurretIndex].bRemoved)
            {
                return std::unexpected(std::format("{}: a destroyed turret left no wreck", Name));
            }

            ++Wrecks;
            Kill();
            if (!Level->Objects[TurretIndex].bRemoved)
            {
                return std::unexpected(std::format("{}: a turret wreck survived its second death", Name));
            }
        }

        Dropped += Combat->CreatedObjects.size();
        Combat->CreatedObjects.clear();

        // Nine in ten generators must give birth within 20 seconds while the player waits at the start; the others have something standing on
        // them, which holds a birth back (ObjectOverlapsSolid).
        constexpr uint32_t GeneratorMilliseconds = 20000;
        std::set<size_t> Parents;
        const FPlayerState Waiting = SpawnPlayer(*PlayerObject, *Class);
        const FPlayerTarget Target{ .State = Waiting, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
        Generators += size_t(std::ranges::count_if(Bodies, [&](const FObjectBody& Body) { return Body.Generator && !Level->Objects[Body.ObjectIndex].bRemoved; }));
        for (uint32_t Time = 0; Time < GeneratorMilliseconds; Time += StepMilliseconds)
        {
            StepGenerators(*Combat, *Level, Bodies, Target, StepMilliseconds);
        }

        for (const FObjectBody& Body : Bodies)
        {
            if (Body.Parent)
            {
                Parents.insert(*Body.Parent);
            }
        }

        GeneratorsBirthing += Parents.size();
    }

    if (Dropped == 0 || Wrecks == 0)
    {
        return std::unexpected(std::format("{} objects dropped, {} wrecks", Dropped, Wrecks));
    }

    // Every surprise starts after its half-second delay and is gone once its time is up; an invisible player takes no damage.
    {
        FCombat Check;
        FLevel Level;
        std::vector<FObjectBody> Bodies;
        FMessages SurpriseMessages;
        constexpr uint32_t SurpriseStep = 10;
        constexpr uint32_t StartMilliseconds = 600;
        constexpr int32_t EndMilliseconds = 21000;
        for (int Type = int(ESurprise::ReverseCommands); Type < int(ESurprise::Nop); ++Type)
        {
            FPlayerState Player;
            FMovementClass Class;
            FPlayerInventory Surprised{ .Energy = 100.f, .Shield = 100.f };
            CollectSurprise(Check, Surprised, ESurprise(Type), SurpriseMessages);
            for (uint32_t Time = 0; Time < StartMilliseconds; Time += SurpriseStep)
            {
                StepSurprises(Check, Level, Bodies, Player, Class, Surprised, SurpriseMessages, SurpriseStep);
            }

            // Trap and Destroy-around last one millisecond and are over by now.
            constexpr int32_t Momentary = 1;
            if (SurpriseTimeout(ESurprise(Type)) > Momentary && Surprised.Surprises.Active != ESurprise(Type))
            {
                return std::unexpected(std::format("surprise {} did not start", Type));
            }

            if (ESurprise(Type) == ESurprise::Invisible && !Player.bInvisible)
            {
                return std::unexpected("the invisible surprise left the player visible");
            }

            for (int32_t Time = 0; Time < EndMilliseconds; Time += int32_t(SurpriseStep))
            {
                StepSurprises(Check, Level, Bodies, Player, Class, Surprised, SurpriseMessages, SurpriseStep);
            }

            if (Surprised.Surprises.Active != ESurprise::None || !Surprised.Surprises.Carried.empty() || Player.bInvisible)
            {
                return std::unexpected(std::format("surprise {} did not end", Type));
            }
        }
    }

    if (GeneratorsBirthing * 10 < Generators * 9)
    {
        return std::unexpected(std::format("{} of {} generators gave birth", GeneratorsBirthing, Generators));
    }

    if (Duels != 0 && DuelShots == 0)
    {
        return std::unexpected(std::format("no turret fired in {} duels", Duels));
    }

    if (CopDuels == 0 || CopShots == 0)
    {
        return std::unexpected(std::format("no cop fired in {} duels", CopDuels));
    }

    return std::format("{} levels, {} player shots, {} turret shots while spinning, {} objects destroyed, {} turrets wrecked, {} objects dropped, at least {} effects; {} turret duels: {} shots, {:.0f} damage taken; "
        "{} cop duels ({} classes): {} shots; {} of {} generators gave birth",
        Levels, Shots, TurretShots, Destroyed, Wrecks, Dropped, Effects, Duels, DuelShots, DamageTaken, CopDuels, CopNames.size(), CopShots, GeneratorsBirthing, Generators);
}
