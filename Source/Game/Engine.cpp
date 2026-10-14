#include "Game/Engine.h"

#include "Game/Audio.h"
#include "Game/Language.h"
#include "Game/Level.h"
#include "Game/Loop.h"
#include "Pg/Binary.h"
#include "Pg/Gaf.h"
#include "Pg/ResourceArchive.h"
#include "Pg/Sound.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

// Exact bits of the world-space table at 0050d440 that WeaponFire (00443bd0) offsets the muzzle with: x, y per direction.
// It is the world image of each facing vector, but rounded differently from what WorldFromCollision produces.
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

}

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

namespace
{

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

// The class library (keyword 0x8a, FUN_00407e00's table) reads Vision-Angle as an integer and keeps (angle % 361) / 30: the directions either side of the facing that the cone takes in, half a direction per 30 degrees.
// The data uses 360 (every direction) and 60.
int VisionDirections(const float Degrees)
{
    constexpr int FullCircle = 361;
    constexpr int DegreesPerStep = 30;
    return int(Degrees) % FullCircle / DegreesPerStep;
}

// EngineUpdateObjects picks an item up only when its TYPE Item sets Only-for-player (keyword 0xdb, item attribute +0x10 bit 1); grilles, stains, smoke, saving plots and the empty standby carry item blocks without it.
bool IsPickup(const FLevelObject& Object)
{
    const FLevelPropertyBlock* const Item = Object.SubType == "item" ? FindItemBlock(Object) : nullptr;
    return Item && ParseLevelFloat(Item->Find("Only-for-player")).value_or(0.f) != 0.f;
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
    // Above each speed band's limit (2, 1 and 0.5 units per step) the counter climbs while at or below 24, 20 or 10 and otherwise drops, except in the fastest band where it holds; slower than that it settles around 5.
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

EPlayerTurn TurnTowardSight(const FPlayerState& Player, const FCollisionPoint ScreenVector)
{
    if (ScreenVector.X == 0.f && ScreenVector.Y == 0.f)
    {
        return EPlayerTurn::None;
    }

    // DirectionFromScreen at 00407630 turns the screen vector into collision axes before FindDirection.
    const int Target = FindDirection(CollisionFromWorld(ScreenVector));
    const int Current = Player.Direction;
    const bool bMoving = Player.Position.X != Player.PreviousPosition.X || Player.Position.Y != Player.PreviousPosition.Y;
    constexpr int SteadyDistance = 2;
    if ((bMoving && DirectionDistance(Target, Current) < SteadyDistance) || Target == Current || Target == NoDirection || Current == NoDirection)
    {
        return EPlayerTurn::None;
    }

    // Left takes the ring's next node; a target half way round reads as backward, which does not turn.
    constexpr int HalfTurn = DirectionCount / 2;
    const int Ahead = Wrap(Target - Current);
    return Ahead < HalfTurn ? EPlayerTurn::Left : Ahead == HalfTurn ? EPlayerTurn::None : EPlayerTurn::Right;
}

uint32_t AbsoluteKeyInput(const FPlayerState& Player, const FCollisionPoint ScreenVector)
{
    if (ScreenVector.X == 0.f && ScreenVector.Y == 0.f)
    {
        return 0;
    }

    // DirectionTurnCommand gives no turn for the same direction or a missing one, which reads as forward; twelve nodes on reads as backward.
    const int Target = FindDirection(CollisionFromWorld(ScreenVector));
    const int Current = Player.Direction;
    if (Target == Current || Target == NoDirection || Current == NoDirection)
    {
        return PlayerInputForward;
    }

    constexpr int HalfTurn = DirectionCount / 2;
    const int Ahead = Wrap(Target - Current);
    if (Ahead == HalfTurn)
    {
        return PlayerInputBackward;
    }

    // A deliberate change: the original only turns until it faces the target, so a new direction first stops the ship.
    // The port thrusts while it turns, forward while the target lies within a quarter turn of the facing and backward beyond, so it always heads the target's way.
    constexpr int QuarterTurn = DirectionCount / 4;
    const int Off = std::min(Ahead, DirectionCount - Ahead);
    const uint32_t Thrust = Off < QuarterTurn ? PlayerInputForward : Off > QuarterTurn ? PlayerInputBackward : 0;
    return (Ahead < HalfTurn ? PlayerInputTurnLeft : PlayerInputTurnRight) | Thrust;
}

uint32_t StickMoveInput(const FPlayerState& Player, const FCollisionPoint ScreenVector)
{
    const int Target = FindDirection(CollisionFromWorld(ScreenVector));
    const int Current = Player.Direction;
    if (Target == NoDirection || Current == NoDirection)
    {
        return 0;
    }

    // A stick held between two directions would make the ship twitch between them, so the facing's neighbours count as on course.
    constexpr int OnCourse = 1;
    const int Ahead = Wrap(Target - Current);
    const int Off = std::min(Ahead, DirectionCount - Ahead);
    if (Off <= OnCourse)
    {
        return PlayerInputForward;
    }

    // The ship turns the short way round, straight back included, and thrusts only while the target lies within a quarter turn, so it never backs off facing away.
    constexpr int HalfTurn = DirectionCount / 2;
    constexpr int QuarterTurn = DirectionCount / 4;
    return (Ahead <= HalfTurn ? PlayerInputTurnLeft : PlayerInputTurnRight) | (Off < QuarterTurn ? PlayerInputForward : 0);
}

uint32_t FacingMoveInput(const FPlayerState& Player, const FCollisionPoint ScreenVector)
{
    const FCollisionPoint Facing = WorldFacingVector(Player.Direction);
    const float FacingLength = std::hypot(Facing.X, Facing.Y);
    const float Length = std::hypot(ScreenVector.X, ScreenVector.Y);
    if (FacingLength == 0.f || Length == 0.f)
    {
        return 0;
    }

    // Along the facing and to its right on screen (y down), each as a share of the stick's push; within an eighth of a turn of an axis counts for it.
    constexpr float AxisShare = 0.383f;
    const float Ahead = (ScreenVector.X * Facing.X + ScreenVector.Y * Facing.Y) / (Length * FacingLength);
    const float Right = (ScreenVector.Y * Facing.X - ScreenVector.X * Facing.Y) / (Length * FacingLength);
    uint32_t Input = Ahead > AxisShare ? PlayerInputForward : Ahead < -AxisShare ? PlayerInputBackward : 0;
    Input |= Right > AxisShare ? PlayerInputStrafeRight : Right < -AxisShare ? PlayerInputStrafeLeft : 0;
    return Input;
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
// Weight divides a thrust push and a shove; a body without one is an error, but thrown debris, which only coasts, has thing.3dc's default of zero.
std::expected<FMovementClass, std::string> ReadMovementProperties(const FLevelPropertyBlock& Properties, const std::string_view ClassName, const int BoundingBoxSize,
    const bool bNeedsWeight = true)
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

    if (Class.Weight == 0.f && bNeedsWeight)
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

namespace
{
// LevelLoad3do (00413d80) gives an object created from a session the velocity (Inertia), the speed (Thrust) and the body pose and its target (Inter-frame, only when both are set) it was saved with.
void ApplySavedMotion(const FLevelPropertyBlock& Properties, FPlayerState& State)
{
    if (const std::optional<FCollisionPoint> Inertia = ParsePair(Properties.Find("Inertia")))
    {
        State.Velocity = *Inertia;
    }

    State.Speed = ParseLevelFloat(Properties.Find("Thrust")).value_or(State.Speed);
    if (const std::optional<FCollisionPoint> Poses = ParsePair(Properties.Find("Inter-frame")); Poses && Poses->X != 0.f && Poses->Y != 0.f)
    {
        State.Pose = int(Poses->X);
        State.TargetPose = int(Poses->Y);
    }
}
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
    ApplySavedMotion(Object.Properties, Player);
    return Player;
}

namespace
{
// ApplyDamage at 0043ca30: the shield absorbs first and, once it breaks, takes back the class's Shield.Min (class +0x6c); whatever is left comes off the energy.
// Every class's minimum is zero but four of underground\item.3dc's, whose shields refill whole each time they break.
void ApplyDamage(float& Energy, float& Shield, const float ShieldMin, float Amount)
{
    if (0.f < Shield)
    {
        Shield = float(double(Shield) - double(Amount));
        if (0.f <= Shield)
        {
            return;
        }

        Amount = -Shield;
        Shield = ShieldMin;
    }

    Energy = float(double(Energy) - double(Amount));
}

void ApplyDamage(FPlayerInventory& Inventory, const float Amount)
{
    ApplyDamage(Inventory.Energy, Inventory.Shield, Inventory.ShieldMin, Amount);
}

void ApplyDamage(FObjectBody& Body, const float Amount)
{
    if (Body.bIndestructible || Body.bInvulnerable)
    {
        return;
    }

    Body.bDamaged = true;
    ApplyDamage(Body.Energy, Body.Shield, Body.ShieldMin, Amount);
}

// EngineUpdateObjects (004406e0) on a knock: a mover that travelled more than 2 units this step hurts itself and what it hit by half its speed times its weight, or itself alone by a tenth of that against the terrain.
// The player never hurts itself this way, and a Player-direct-fire-only class (flag 0x40 at +0x20) is hurt only in knocks with the player.
// A Final-object killed this way starts the finale like any other death of one (FUN_0043db70).
// A knock counts once the mover has speed and weight and travelled more than 2 units this step (DAT_00509518).
bool IsHardKnock(const FPlayerState& Mover, const FMovementClass& Class, const FCollisionPoint Delta)
{
    constexpr float HardKnockTravel = 2.f;
    return Mover.SpeedMagnitude * Class.Weight != 0.f && HardKnockTravel < std::abs(Delta.X) + std::abs(Delta.Y);
}

void KnockDamage(const FPlayerState& Mover, const FMovementClass& Class, const FCollisionPoint Delta, FObjectBody* const Self, FObjectBody* const HitBody, FPlayerKnock* const PlayerHit)
{
    const float Knock = Mover.SpeedMagnitude * Class.Weight;
    if (!IsHardKnock(Mover, Class, Delta))
    {
        return;
    }

    const bool bAgainstPlayer = PlayerHit && PlayerHit->bHit;
    if (!HitBody && !bAgainstPlayer)
    {
        constexpr double TerrainKnockScale = 0.1f;
        if (Self && !Self->bPlayerDirectFireOnly)
        {
            ApplyDamage(*Self, float(double(Knock) * TerrainKnockScale));
        }

        return;
    }

    constexpr double ObjectKnockScale = 0.5f;
    const float Damage = float(double(Knock) * ObjectKnockScale);
    if (Self && (!Self->bPlayerDirectFireOnly || bAgainstPlayer))
    {
        ApplyDamage(*Self, Damage);
    }

    if (HitBody && (!HitBody->bPlayerDirectFireOnly || !Self))
    {
        ApplyDamage(*HitBody, Damage);
    }

    if (bAgainstPlayer)
    {
        PlayerHit->Damage += Damage;
    }
}
}

namespace
{
bool HuntsPlayer(const FObjectBody& Body);
}

const FObjectBody* StepPlayer(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, const uint32_t StepMilliseconds,
    const FTerrainCollision& Terrain, const FLevel& Level, const std::span<FObjectBody> Bodies, FObjectBody* const Self, FPlayerKnock* const PlayerHit)
{
    Player.KnockPoint.reset();
    // Actors turn their body in their behaviour update instead (SeekerTurnBody at 0042cfa0).
    if (!Self || !Self->Actor)
    {
        TurnPlayer(Player, Class, Command.Turn, StepMilliseconds);
    }

    constexpr uint32_t ReactorGlowMilliseconds = 1000;
    Player.ReactorMilliseconds = Command.Thrust == EPlayerThrust::Forward ? ReactorGlowMilliseconds
                                                                          : Player.ReactorMilliseconds - std::min(Player.ReactorMilliseconds, StepMilliseconds);
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

    // ponytail: objects are scanned after the terrain in object order instead of interleaved through the sweep endpoints; the nearest contact still wins, so only exact ties between a wall and an object can resolve differently.
    std::optional<FCollisionPoint> Contact = TerrainHit ? std::optional(TerrainHit->Point) : std::nullopt;
    FObjectBody* HitBody = nullptr;
    for (FObjectBody& Body : Bodies)
    {
        // Ignore-same-class-objects lets a class pass through its own kind, as the Particle-of-Death strings on one patrol path do.
        if (&Body == Self || Body.bPassable || Body.Class.Radius <= 0.f || (Self && Self->bIgnoreSameClass && Level.Objects[Body.ObjectIndex].ClassName == Level.Objects[Self->ObjectIndex].ClassName))
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

    // An actor runs into the player too; the original pushes the player like any object, the port only bounces off it.
    if (PlayerHit)
    {
        PlayerHit->bHit = false;
        const std::optional<FCollisionPoint> PlayerContact = IntersectCircles({ .Center = Candidate, .Radius = Class.Radius }, PlayerHit->Circle);
        if (PlayerContact && (!Contact || PrefersContact(*Contact, *PlayerContact, Candidate)))
        {
            Contact = PlayerContact;
            HitBody = nullptr;
            PlayerHit->bHit = true;
        }
    }

    if (!Contact)
    {
        Player.Velocity = Class.bDropsVelocity ? FCollisionPoint{} : FCollisionPoint{ .X = float(double(TimeScale) * Delta.X), .Y = float(double(TimeScale) * Delta.Y) };
        Player.Center = Candidate;
        return nullptr;
    }

    // AmmoHit (0043d7d0) moves a mine to the contact point, and AmmoImpact (0043d350) pushes what it struck by 40 / Weight along the mine's facing.
    // ponytail: a mine's own TYPE Ammo damage and blast, zero for every mine class, and Ignore-owner, off for both, are left out.
    constexpr double ImpactPush = 40.;
    const auto Impact = [&](const FObjectBody& Mine, FPlayerState& Struck, const float Weight)
    {
        const FCollisionPoint Facing = FacingVector(Mine.State.Direction);
        Struck.Velocity.X = float(double(Facing.X) * ImpactPush / double(Weight) + double(Struck.Velocity.X));
        Struck.Velocity.Y = float(double(Facing.Y) * ImpactPush / double(Weight) + double(Struck.Velocity.Y));
    };
    const auto Count = [](FMine& Mine, const bool bHunter) { ++(bHunter ? Mine.Hits : Mine.Misses); };

    // A mover that is not ammo itself sets off the mine it runs into (EngineUpdateObjects calls AmmoHit with its destroy flag). The mover's update ends there:
    // its collision centre stays and its position is not put back.
    const bool bAmmoMover = Self && Self->Mine.has_value();
    if (HitBody && HitBody->Mine && !HitBody->Mine->bStruck && !bAmmoMover)
    {
        HitBody->Mine->bStruck = true;
        Count(*HitBody->Mine, Self && HuntsPlayer(*Self));
        HitBody->State.Position = WorldFromCollision(*Contact);
        HitBody->bPassable = true;
        HitBody->bDamaged = true;
        HitBody->Energy = 0.f;
        Impact(*HitBody, Player, Class.Weight);
        return HitBody;
    }

    // A pushed mine (ammo with neither On-hit nor On-object-hit) goes off on the terrain. Against an object AmmoHit moves it and pushes the object without destroying it,
    // then the object's shield and energy wear the mine's shield and its energy, both by the whole amount once the shield holds it; a mine with energy left goes on through.
    // Against the player it pushes the player too; the player's shield and energy outlast any mine's, so it goes off.
    if (Self && Self->Mine && !Self->Mine->bStruck)
    {
        Self->State.Position = WorldFromCollision(*Contact);
        if (!HitBody && PlayerHit && PlayerHit->bHit)
        {
            const FCollisionPoint Facing = FacingVector(Self->State.Direction);
            PlayerHit->Push = { .X = float(double(Facing.X) * ImpactPush), .Y = float(double(Facing.Y) * ImpactPush) };
        }

        if (!HitBody)
        {
            Self->Mine->bStruck = true;
            Count(*Self->Mine, false);
            Self->bPassable = true;
            Self->bDamaged = true;
            Self->Energy = 0.f;
            return nullptr;
        }

        Count(*Self->Mine, HuntsPlayer(*HitBody));
        Impact(*Self, HitBody->State, HitBody->Class.Weight);
        float Wear = HitBody->Shield + HitBody->Energy;
        if (0.f < Wear)
        {
            if (Self->Shield < Wear)
            {
                if (0.f < Self->Shield)
                {
                    Wear -= Self->Shield;
                    Self->Shield = 0.f;
                }
            }
            else
            {
                Self->Shield -= Wear;
            }

            if (0.f < Wear)
            {
                if (Self->Energy < Wear)
                {
                    Self->Energy = 0.f < Self->Energy ? 0.f : Self->Energy;
                }
                else
                {
                    Self->Energy -= Wear;
                }
            }
        }

        if (Self->Energy <= 0.f)
        {
            Self->bDamaged = true;
            return HitBody;
        }

        Player.Velocity = { .X = float(double(TimeScale) * Delta.X), .Y = float(double(TimeScale) * Delta.Y) };
        Player.Center = Candidate;
        return HitBody;
    }

    // ObjectRestorePosition at 004449f0 undoes the move before the bounce is resolved.
    Player.Position = Player.PreviousPosition;
    // Touch-and-destroy (object flag 0x400000): a mover with it takes the energy of what it runs into, and one that runs into it loses its own, unless that one is Indestructible.
    // The move ends there, with no knock and no push.
    if (HitBody && Player.bTouchAndDestroy && !HitBody->bIndestructible)
    {
        HitBody->Energy = 0.f;
        HitBody->bDamaged = true;
        return HitBody;
    }

    if (PlayerHit && PlayerHit->bHit && PlayerHit->bTouchAndDestroy && Self && !Self->bIndestructible)
    {
        Self->Energy = 0.f;
        Self->bDamaged = true;
        return nullptr;
    }

    KnockDamage(Player, Class, Delta, Self, HitBody, PlayerHit);
    if (IsHardKnock(Player, Class, Delta))
    {
        Player.KnockPoint = WorldFromCollision(*Contact);
    }

    const EBounce Result = HitBody ? Push(Player, Class, Command, *HitBody, *Contact) : Bounce(Player, Class, Command, *Contact);
    if (Result == EBounce::Accept)
    {
        // Moving away from the contact commits the collision centre; the world position catches up on the next step.
        Player.Center = Candidate;
    }

    return HitBody;
}

namespace
{
// The TYPE Absorption blocks: the coefficient that scales a matching ammo class's damage.
std::vector<FAbsorption> ReadAbsorptions(const FLevelPropertyBlock& Properties)
{
    std::vector<FAbsorption> Result;
    for (const FLevelPropertyBlock& Child : Properties.Children)
    {
        if (Child.Kind == "TYPE" && Child.Name == "Absorption")
        {
            Result.push_back({ .AmmoClass = ToLowerName(Child.Find("Class-name")), .Coeff = ParseLevelFloat(Child.Find("Coeff")).value_or(0.f) });
        }
    }

    return Result;
}
}

std::optional<FObjectBody> CreateObjectBody(const FLevel& Level, const size_t Index)
{
    const FLevelObject& Object = Level.Objects[Index];
    // Kinds come from the class Sub-type or the GAF folder (CreateObject at 00413800); pickups are collected instead of blocking.
    constexpr std::array<std::string_view, 9> SolidKinds = { "item", "thing", "turret", "staticgen", "burner", "roller", "lift", "gate", "ammo" };
    constexpr std::array<std::string_view, 8> ActorKinds = { "cop", "jeep", "tank", "walker", "spider", "heli", "burner", "roller" };
    constexpr std::string_view CopKind = "cop";
    constexpr std::string_view BurnerKind = "burner";
    constexpr std::string_view TankKind = "tank";
    constexpr std::string_view WalkerKind = "walker";
    constexpr std::string_view SpiderKind = "spider";
    // Any other thing that patrols or wanders moves too, as the Particle-of-Death things do: EngineUpdateObjects runs every object's behaviour whatever its kind.
    constexpr std::string_view ThingKind = "thing";
    const std::optional<uint32_t> ThingBehaviour = Object.SubType == ThingKind ? ParseBehaviour(Object.Behaviour) : std::nullopt;
    const bool bMovingThing = ThingBehaviour && ((*ThingBehaviour & BehaviourMovementMask) == BehaviourPatrol || (*ThingBehaviour & BehaviourMovementMask) == BehaviourRandom);
    const bool bActor = std::ranges::contains(ActorKinds, Object.SubType) || bMovingThing;
    const FLevelPropertyBlock& Properties = Object.Properties;
    const auto Number = [](const FLevelPropertyBlock* Block, const std::string_view Field)
    {
        return Block ? ParseLevelFloat(Block->Find(Field)).value_or(0.f) : 0.f;
    };
    // An object without a bounding box never collides (object +0xb4 bit 1), but blasts and Destroy-around still wear it down when it has energy and is not Indestructible,
    // as Processors Chambers' weld seams (soudure), which set off their Small-Explosion in a chain.
    const bool bBoxless = Object.BoundingBoxSize <= 0;
    const bool bDestructible = 0.f < Number(FindBlock(Properties, "Energy"), "Initial") && Number(&Properties, "Indestructible") == 0.f;
    if ((bBoxless && !bDestructible) || (!bActor && !std::ranges::contains(SolidKinds, Object.SubType)) || IsPickup(Object))
    {
        return std::nullopt;
    }

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
        .bPlayerDirectFireOnly = Number(&Properties, "Player-direct-fire-only") != 0.f,
        .bIgnoreSameClass = Number(&Properties, "Ignore-same-class-objects") != 0.f,
        .bInvisible = Number(&Properties, "Invisible") != 0.f,
        .bVisibleWhenFiring = Number(&Properties, "Visible-when-firing") != 0.f,
        .bFinalObject = Number(&Properties, "Final-object") != 0.f,
        .ShieldMax = Number(FindBlock(Properties, "Shield"), "Max"),
        .ShieldUpStep = Number(&Properties, "Shield-up-step"),
        .ShieldMin = Number(FindBlock(Properties, "Shield"), "Min"),
    };
    Body.Absorptions = ReadAbsorptions(Properties);

    for (const FLevelPropertyBlock& Child : Properties.Children)
    {
        if (Child.Kind == "TYPE" && Child.Name == "Explosion")
        {
            Body.Explosions.push_back({ .BoomEffect = ToLowerName(Child.Find("Boom-fx")), .Damage = Number(&Child, "Damage"), .Zone = Number(&Child, "Damage-zone") });
        }
    }

    // ObjectInitializeClassProperties (00419d30) starts the on and off timers at Random-on-timeout's and Random-off-timeout's Initial; a session sets them with On-timeout and Off-timeout.
    if (const FLevelPropertyBlock* const Ammo = Object.SubType == "ammo" ? FindTypeBlock(Properties, "Ammo") : nullptr)
    {
        const FLevelPropertyBlock* const On = FindBlock(Properties, "Random-on-timeout");
        const FLevelPropertyBlock* const Off = FindBlock(Properties, "Random-off-timeout");
        Body.Mine = FMine{
            .DamageTimer = uint32_t(Number(Ammo, "Damage-after")),
            .OnTimer = int32_t(ParseLevelFloat(Properties.Find("On-timeout")).value_or(Number(On, "Initial"))),
            .OffTimer = int32_t(ParseLevelFloat(Properties.Find("Off-timeout")).value_or(Number(Off, "Initial"))),
            .OnMin = int32_t(Number(On, "Min")),
            .OnMax = int32_t(Number(On, "Max")),
            .OffMin = int32_t(Number(Off, "Min")),
            .OffMax = int32_t(Number(Off, "Max")),
            .bKeepBoxHidden = Number(&Properties, "Keep-bb-when-invisible") != 0.f,
        };
        Body.State.Direction = Object.Direction < DirectionCount ? Object.Direction : 0;
    }

    Body.Altitude = Object.Altitude;
    // .3DO instances can override the starting Energy and Shield with plain fields.
    Body.Energy = ParseLevelFloat(Properties.Find("Energy")).value_or(Body.Energy);
    Body.Shield = ParseLevelFloat(Properties.Find("Shield")).value_or(Body.Shield);
    Body.State.Position = Body.State.PreviousPosition = { .X = Object.X, .Y = Object.Y };
    Body.State.Center = CollisionFromWorld(Body.State.Position);
    ApplySavedMotion(Properties, Body.State);
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
        // PlayerAdvancePose (00438390) turns cops, burners and things through 48 poses, the other actors through 24.
        const int ActorPoses = Object.SubType == CopKind || Object.SubType == BurnerKind || Object.SubType == ThingKind ? PosesPerDirection : 1;
        Body.State.Pose = Body.State.TargetPose = Body.State.Direction * ActorPoses + 1;
        ApplySavedMotion(Properties, Body.State);
        constexpr float TankCorrectionScale = 0.01f;
        const auto ClassBlock = Level.Classes.find(ToLowerName(Object.ClassName));
        FActor& Actor = Body.Actor.emplace(FActor{
            .Behaviour = *Behaviour,
            .bTurnOnPlace = Number(&Properties, "Can-turn-on-place") != 0.f,
            .ClassTurnTimeout = int(Number(ClassBlock != Level.Classes.end() ? &ClassBlock->second : &Properties, "Turn-timeout")),
            .PosesPerDirection = ActorPoses,
            .bTwoPieces = Number(&Properties, "Two-pieces") != 0.f,
            .CorrectionScale = Object.SubType == TankKind || bLegged ? TankCorrectionScale : 1.f,
            .bSpider = Object.SubType == SpiderKind,
            .TopPose = Body.State.Pose,
            .TopTargetPose = Body.State.TargetPose,
            .TopTurnTimeout = int32_t(Number(&Properties, "Turret-turn-timeout")),
            .Tilt = int(Number(&Properties, "Banking-level")),
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
            Actor.Seeker = FSeeker{
                .bTracker = bTracker,
                .VisionDirections = VisionDirections(Number(Seeker, "Vision-Angle")),
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

FCollisionPoint StepObjectBodies(const std::span<FObjectBody> Bodies, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level,
    const FPlayerTarget* const Player)
{
    FCollisionPoint Push;
    for (FObjectBody& Body : Bodies)
    {
        // Actors move every step in StepActors.
        if (Body.Actor || (!Body.State.bPushed && IsZero(Body.State.Velocity)))
        {
            continue;
        }

        std::optional<FPlayerKnock> Knock;
        if (Player)
        {
            Knock = FPlayerKnock{ .Circle = { .Center = Player->State.Center, .Radius = Player->Radius }, .bTouchAndDestroy = Player->State.bTouchAndDestroy };
        }

        StepPlayer(Body.State, Body.Class, {}, StepMilliseconds, Terrain, Level, Bodies, &Body, Knock ? &*Knock : nullptr);
        if (!Knock)
        {
            continue;
        }

        // ApplyDamage spares an invisible player and an invulnerable one, as for shots.
        if (Knock->Damage != 0.f && !Player->State.bInvisible && !Player->Inventory.Surprises.bInvulnerable)
        {
            ApplyDamage(Player->Inventory, Knock->Damage);
        }

        Push.X = float(double(Knock->Push.X) / double(Player->Inventory.Weight) + double(Push.X));
        Push.Y = float(double(Knock->Push.Y) / double(Player->Inventory.Weight) + double(Push.Y));
    }

    return Push;
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

std::expected<std::vector<FDemoRecord>, std::string> LoadDemo(FResourceArchive& Archive)
{
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

    return Records;
}

std::vector<uint32_t> DemoStepLengths(const std::span<const FDemoRecord> Records, std::vector<bool>* const FrameEnds)
{
    // Record times are clock readings at each step start.
    // Recording caps steps at 10 ms, so a frame whose poll measured E ms produced ceil(E / 10) records with steps of 10 ms and a remainder; E is the gap since the previous frame's first record. ponytail: the 1 ms clock can misplace E by one millisecond, which accounts for most remaining coasting mismatches.
    std::vector<uint32_t> StepLengths(Records.size(), DemoStepMilliseconds);
    if (FrameEnds)
    {
        FrameEnds->assign(Records.size(), false);
    }

    if (Records.empty())
    {
        return StepLengths;
    }

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

        if (FrameEnds)
        {
            (*FrameEnds)[std::min(First + FrameSteps, Records.size()) - 1] = true;
        }

        PreviousFrameTime = Records[First].Time;
        First += FrameSteps;
    }

    return StepLengths;
}

std::expected<std::string, std::string> VerifyDemoReplay(FResourceArchive& Archive)
{
    // The check ignores the floating messages it raises.
    FMessages Messages;
    const std::expected<std::vector<FDemoRecord>, std::string> Demo = LoadDemo(Archive);
    if (!Demo)
    {
        return std::unexpected(Demo.error());
    }

    const std::vector<FDemoRecord>& Records = *Demo;

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

    // The recording build used Friction-threshold 0.001, the value commented out in the shipped OBJECT.3DC: with 0.01 the first recorded forward step would not move at all.
    constexpr float RecordingFrictionThreshold = 0.001f;
    Class->FrictionThreshold = { .X = RecordingFrictionThreshold, .Y = RecordingFrictionThreshold };

    const std::vector<uint32_t> StepLengths = DemoStepLengths(Records);

    constexpr float ExactTolerance = 0.001f;
    constexpr float CloseTolerance = 0.5f;
    size_t Exact = 0;
    size_t Close = 0;
    // ponytail: the replay steps the player against the terrain alone. The recording keeps only the player, so a simulated world drifts from the original's and blocks more steps than it explains; the misses come from the inferred step split and from enemy shots' shoves.
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

        // Each step starts from the recorded state: an accepted move stores velocity = (14 / step) * centre delta, and the facing comes from the next record so that the turn-timer phase, which predates the recording, cannot leak in.
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

// ConsumeItem at 00444660 announces a surprise by its name (SurpriseIsBonus at 0041bcf0): a Bonus line, a Malus line, or nothing for Nop.
void AnnounceSurprise(const ESurprise Type)
{
    constexpr uint32_t MalusDuration = 1000;
    constexpr uint32_t BonusDuration = 2000;
    switch (Type)
    {
    case ESurprise::ReverseCommands:
    case ESurprise::Trap:
    case ESurprise::WeaponJam:
    case ESurprise::Darken:
    case ESurprise::Extasy:
    case ESurprise::LowSpeed:
    case ESurprise::Pinball:
    case ESurprise::Banana:
    case ESurprise::BlockedMotor:
        QueueVoice(EVoice::Malus, 0, MalusDuration, 0);
        break;
    case ESurprise::DestroyAround:
    case ESurprise::TouchAndDestroy:
    case ESurprise::AutoShieldUp:
    case ESurprise::Invisible:
    case ESurprise::CircularFire:
    case ESurprise::TimeWarp:
    case ESurprise::ReloadWeapons:
    case ESurprise::Invulnerability:
    case ESurprise::TripleFire:
        QueueVoice(EVoice::Bonus, 0, BonusDuration, 0);
        break;
    default:
        break;
    }
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

std::string_view SurpriseEnglishText(const ESurprise Type)
{
    switch (Type)
    {
    case ESurprise::ReverseCommands: return "Reversed Commands !";
    case ESurprise::Trap: return "Trap !!!";
    case ESurprise::WeaponJam: return "Weapon Jam !";
    case ESurprise::Darken: return "No Light !!!";
    case ESurprise::Extasy: return "XTC !!!";
    case ESurprise::LowSpeed: return "Thrust loss !!!";
    case ESurprise::Pinball: return "Pinball Effect !";
    case ESurprise::Banana: return "No Inertia Control !";
    case ESurprise::BlockedMotor: return "Blocked Motor !!!";
    case ESurprise::DestroyAround: return "Destroy Around !!!";
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

// FUN_0041bd20: the text that announces a surprise.
std::string_view SurpriseText(const ESurprise Type)
{
    return Localize(SurpriseEnglishText(Type));
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

    // Auto Shield Up adds DAT_0050954c to the shield-up step for its time (SurpriseStart and SurpriseEnd).
    constexpr float AutoShieldUpStep = 10.f;
    switch (Type)
    {
    case ESurprise::ReverseCommands:
        State.bReversed = false;
        break;
    case ESurprise::WeaponJam:
        State.JammedWeapon.reset();
        break;
    case ESurprise::AutoShieldUp:
        Inventory.ShieldUpStep = std::max(Inventory.ShieldUpStep - AutoShieldUpStep, 0.f);
        break;
    case ESurprise::Extasy:
    case ESurprise::LowSpeed:
        Class.SpeedMax = State.SpeedMax;
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
    case ESurprise::TouchAndDestroy:
        Player.bTouchAndDestroy = false;
        break;
    case ESurprise::TimeWarp:
        State.WarpDivisor = 0;
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
        const std::optional<FCollisionPoint> WarnAt = ParsePair(Item->Find("Warn-message-position"));
        return FPickup{
            .ObjectIndex = Index,
            .Circle = { .Center = CollisionFromWorld({ .X = Object.X, .Y = Object.Y }), .Radius = Object.BoundingBoxSize > 0 ? float(Object.BoundingBoxSize) : DefaultCollisionRadius },
            .AddEnergy = Number("Add-NRJ"),
            .AddShield = Number("Add-Shield"),
            .AddMoney = Number("Add-Money"),
            .AmmoType = std::string(Item->Find("Ammo-Type")),
            .AddAmmo = Number("Add-Ammo"),
            .Key = Key ? std::optional(FCarriedKey{
                             .Id = KeyNumber("Id"),
                             .bCode = KeyNumber("Is-code") != 0,
                             .PartId = KeyNumber("Code-part-id"),
                             .PartCount = KeyNumber("Code-part-count"),
                             .CodeString = std::string(Key->Find("Code-String")),
                             .ObjectIndex = Index,
                             .bHasTimeout = Number("Has-timeout") != 0.f,
                             .Timeout = int32_t(Number("Timeout")),
                             .WarnTimeout = int32_t(Number("Warn-timeout")),
                             .WarnFormat = std::string(Item->Find("Warn-message-format")),
                             .WarnX = WarnAt ? int32_t(WarnAt->X) : 0,
                             .WarnY = WarnAt ? int32_t(WarnAt->Y) : 0,
                             .ReactivateAfter = int32_t(Number("Reactivate-after")),
                             .ClassName = Object.ClassName,
                             .X = Object.X,
                             .Y = Object.Y,
                         })
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
        .ShieldMin = Value("Shield", "Min"),
        .Absorptions = ReadAbsorptions(Properties),
        .Weight = Value("Weight", "Initial") != 0.f ? Value("Weight", "Initial") : 1.f,
        .ShieldUpStep = ParseLevelFloat(Properties.Find("Shield-up-step")).value_or(0.f),
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


// FUN_0041cf40 for a newly collected surprise: the surprise in effect ends, the new one waits half a second, and its text and blinking icon appear for as long as it lasts.
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

    AnnounceSurprise(Type);

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

void CarrySurprise(FCombat& Combat, FPlayerInventory& Inventory, const ESurprise Type, const FLevelObject& Item, FMessages& Messages)
{
    CollectSurprise(Combat, Inventory, Type, Messages);
    FCarriedSurprise& Carried = Inventory.Surprises.Carried.front();
    Carried.ClassName = Item.ClassName;
    Carried.X = Item.X;
    Carried.Y = Item.Y;
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
            AddPickupMessage(Messages, Object, OffsetY, Format("Energy +%d", int32_t(Pickup.AddEnergy)), EnergyColor);
            bUsed = true;
        }

        if (Pickup.AddMoney != 0.f)
        {
            Inventory.Money += Pickup.AddMoney;
            Inventory.Earned += Pickup.AddMoney;
            AddPickupMessage(Messages, Object, OffsetY, Format("Money +%d", int32_t(Pickup.AddMoney)), MoneyColor);
            bUsed = true;
            // EngineUpdateObjects cheers six times in sixteen by the game clock.
            constexpr std::array MoneyVoices = { EVoice::Laugh, EVoice::Yeepee, EVoice::Yeah, EVoice::Hey, EVoice::Yo, EVoice::GiveMe };
            constexpr uint32_t MoneyVoiceMask = 0xf;
            constexpr uint32_t MoneyVoiceDuration = 1000;
            if (const uint32_t Pick = GetVoiceClock() & MoneyVoiceMask; Pick < MoneyVoices.size())
            {
                QueueVoice(MoneyVoices[Pick], 0, MoneyVoiceDuration, 0);
            }
        }

        if (Pickup.AddShield != 0.f && Inventory.Shield < Inventory.ShieldMax)
        {
            OffsetY = bUsed ? LineStep : OffsetY;
            Inventory.Shield = std::min(Inventory.ShieldMax, Inventory.Shield + Pickup.AddShield);
            AddPickupMessage(Messages, Object, OffsetY, Format("Shield +%d", int32_t(Pickup.AddShield)), ShieldColor);
            bUsed = true;
        }

        // PlayerAddAmmo (00444540): only when one of the player's weapons has a TYPE Weapon block for the item's ammo, the first stock of it that is not full fills by the item's amount up to its maximum.
        // Under Reload Weapons it looks in the saved copy (session +0x2208), which holds the stocks but no weapon, so nothing is added.
        const auto TakesAmmo = [&](const size_t Weapon) { return std::ranges::contains(Combat->Weapons[Weapon].AmmoChoices, ToLowerName(Pickup.AmmoType)); };
        if (!Pickup.AmmoType.empty() && Pickup.AddAmmo != 0.f && (!Combat || std::ranges::any_of(Combat->PlayerWeapons, TakesAmmo)) &&
            Inventory.Surprises.Active != ESurprise::ReloadWeapons)
        {
            const auto Stock = std::ranges::find_if(Inventory.Ammo, [&](const FAmmoStock& Entry) { return ToLowerName(Entry.Name) == ToLowerName(Pickup.AmmoType) && Entry.Count < Entry.Max; });
            if (Stock != Inventory.Ammo.end())
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
                const std::string Text = !Key.bCode                ? Format("Key #%d !", Key.Id)
                                         : CarriesKey(Inventory.Keys, Key.Id) ? Format("Code #%d complete !", Key.Id)
                                                                              : Format("Code #%d : part %d !", Key.Id, Key.PartId);
                AddCentreMessage(Messages, Text, KeyColor);
                constexpr uint32_t KeyVoiceDuration = 1000;
                QueueVoice(Key.bCode ? EVoice::Code : EVoice::Key, 0, KeyVoiceDuration, 0);
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
            CarrySurprise(*Combat, Inventory, Pickup.Surprise, Object, Messages);
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

FWorldCell CellOf(const FCollisionPoint World)
{
    return { .X = int(uint32_t(int64_t(World.X)) / BlockWidth), .Y = int(uint32_t(int64_t(World.Y)) / BlockHeight) };
}

std::optional<size_t> FindPlayerObject(const FLevel& Level)
{
    std::optional<size_t> Found;
    std::tuple<int, int, size_t> Best{};
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        if (Object.Behaviour != "Player" || Object.bRemoved)
        {
            continue;
        }

        const FWorldCell Cell = CellOf({ .X = Object.X, .Y = Object.Y });
        if (const std::tuple Rank(Cell.Y, Cell.X, Index); !Found || Rank < Best)
        {
            Found = Index;
            Best = Rank;
        }
    }

    return Found;
}

namespace
{

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

// ObjectCarriesKey at 0043dcb0: the newest carried key with the id decides; a code also needs all of its parts (ObjectFindCompleteCode at 00439270), counted as the first part says and including part 1.
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

// DoorFindHolder at 00443430: something that may pass within two cells of the door. A key or code can only be carried by the player, and invisible objects (flag 8) are left out.
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
            !(BodyOf[Index] && BodyOf[Index]->bInvisible) &&
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

// ObjectOverlapsSolid at 00405df0: whether a solid object in the body's cell or the cells around it overlaps the body's box.
// Lying items are left out.
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

// The key part of the gate and lift updates: Destroy-key takes every key or code part with the id from the holder (ObjectDestroyKeys at 0043dd20), which DoorAnnounceKeyUse (00443500) announces when it is the player, and without Key-always-needed the door forgets its id.
void UseKey(FDoor& Door, FPlayerInventory& Inventory, const EHolder Holder, FMessages& Messages)
{
    constexpr uint32_t PlainColor = 0xffffff;
    if ((Door.Flags & DoorDestroyKey) != 0 && Holder == EHolder::Player && std::erase_if(Inventory.Keys, [&](const FCarriedKey& Key) { return Key.Id == Door.Id; }) != 0)
    {
        AddCentreMessage(Messages, Format((Door.Flags & DoorNeedsCode) != 0 ? "Using Code #%d !" : "Using Key #%d !", Door.Id), PlainColor);
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
    Door.Playback = StartAnimation(Door.Animations[Animation], &Door.Emitter);
    ShowFrame(Door.Playback, Door.Animations[Animation], FirstMovingFrame, &Door.Emitter);
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

// ObjectUpdateLevelExit at 00442bf0.
// A lift leaves when nothing stands on it and comes back after its delay; the player rides it once fully inside its box, which returns true.
// A negative Reward is its fare.
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
// A Player-direct-fire-only object only takes the blasts of the player's own shots (FUN_0043c990 on the blast's source).
void AreaDamage(FCombat& Combat, const FCollisionPoint Center, const FWorldCell Cell, const float Damage, const float Zone, std::vector<FObjectBody>& Bodies,
    const FPlayerTarget& Player, const bool bByPlayer)
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
        if (0.f < Body.Energy && (!Body.bPlayerDirectFireOnly || bByPlayer))
        {
            if (const std::optional<float> Amount = Falloff(Body.State.Position, Body.State.Center))
            {
                ApplyDamage(Body, *Amount);
            }
        }
    }

    // The scan leaves out an invulnerable player (object flag 0x800000) altogether, so it neither flashes nor shakes.
    if (0.f < Player.Inventory.Energy && !Player.Inventory.Surprises.bInvulnerable)
    {
        if (const std::optional<float> Amount = Falloff(Player.State.Position, Player.State.Center))
        {
            if (!Player.State.bInvisible)
            {
                ApplyDamage(Player.Inventory, *Amount);
                Player.Inventory.bHit = true;
            }

            // The damage that reaches the player, not the blast's, decides the shake, flash and cry.
            // 200 or more shakes the screen by RandomRange(-n, n) twice from the Misc stream (StartScreenShake at 0043d100 into session +0x2200), n = damage / 100 + 8 up to 32, except during the finale.
            constexpr float ShakeDamage = 200.f;
            constexpr int ShakeBase = 8;
            constexpr int ShakeDivisor = 100;
            constexpr int ShakeLimit = 0x20;
            if (ShakeDamage <= *Amount && !Combat.bFinale)
            {
                const int Range = std::min(int(*Amount) / ShakeDivisor + ShakeBase, ShakeLimit);
                Player.Inventory.Surprises.ShakeX = Combat.MiscRandom.Range(-Range, Range);
                Player.Inventory.Surprises.ShakeY = Combat.MiscRandom.Range(-Range, Range);
            }

            // 300 or more flashes the screen (60, or 30 below 1000) and makes a living player cry out three times in eight by the game clock.
            constexpr float CryDamage = 300.f;
            constexpr float StrongBlast = 1000.f;
            constexpr int32_t StrongFlash = 60;
            constexpr int32_t WeakFlash = 30;
            constexpr int32_t BlastFlashLimit = 100;
            if (CryDamage <= *Amount && 0.f < Player.Inventory.Energy)
            {
                PushGamma(Player.Inventory.Surprises.Gamma, *Amount < StrongBlast ? WeakFlash : StrongFlash, BlastFlashLimit);
            }

            constexpr std::array Cries = { EVoice::Ooh, EVoice::Looser, EVoice::SadMan };
            constexpr uint32_t CryMask = 7;
            if (CryDamage <= *Amount && 0.f < Player.Inventory.Energy)
            {
                if (const uint32_t Pick = GetVoiceClock() & CryMask; Pick < Cries.size())
                {
                    constexpr uint32_t CryDelay = 500;
                    constexpr uint32_t CryDuration = 2000;
                    constexpr uint32_t CryHold = 5000;
                    QueueVoice(Cries[Pick], CryDelay, CryDuration, CryHold);
                }
            }
        }
    }
}

FObjectBody* FindStruckBody(std::vector<FObjectBody>& Bodies, const FCollisionCircle Shot, const size_t Owner, std::optional<FCollisionPoint>& Contact)
{
    FObjectBody* Struck = nullptr;
    for (FObjectBody& Body : Bodies)
    {
        if (Body.ObjectIndex == Owner || Body.bPassable || Body.Class.Radius <= 0.f)
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
constexpr std::string_view WaterSplashEffect = "water-splash";
// An exploding object's blast goes off this long after its Boom-fx appears, or at half its animation if that comes first.
constexpr int32_t BlastDelayMilliseconds = 500;

// UpdateAnimation at 0045e7e0: each frame is held for its duration; a one-shot animation ends after its last frame.
void AdvanceEffect(FEffect& Effect, const FEffectClass& Class, const uint32_t StepMilliseconds, const bool bAlwaysLoop = false)
{
    if (Effect.bFinished)
    {
        return;
    }

    const FSoundEmitter Emitter{
        .X = Effect.Position.X,
        .Y = Effect.Position.Y,
        .Altitude = Effect.Altitude,
        .ChannelMask = Class.ChannelMask,
        .Priority = Class.Priority,
    };
    const uint32_t FrameCount = uint32_t(Class.Animation.Frames.size());
    Effect.FrameTimer -= int32_t(StepMilliseconds);
    // A looping animation of zero-length frames would never leave this loop; one pass through it is enough.
    for (uint32_t Advanced = 0; Effect.FrameTimer <= 0 && Advanced <= FrameCount; ++Advanced)
    {
        ++Effect.Frame;
        if (Effect.Frame > FrameCount)
        {
            Effect.Frame = Class.bLoop || bAlwaysLoop ? std::max(Class.LoopStart, 1u) : 0;
        }

        if (Effect.Frame == 0 || Effect.Frame > FrameCount)
        {
            Effect.Frame = 0;
            Effect.bFinished = true;
            return;
        }

        Effect.FrameTimer += Class.Animation.Frames[Effect.Frame - 1].Duration * AnimationPeriodMilliseconds;
        PlayFrameSound(Class.Animation, Class.Animation.Frames[Effect.Frame - 1], Effect.Sound, &Emitter);
    }

    FollowFrameSound(Effect.Sound, &Emitter);
}

std::vector<std::optional<uint32_t>> FirstCreateTimes(const std::span<const FCreateBlock> Creates)
{
    std::vector<std::optional<uint32_t>> Times;
    for (const FCreateBlock& Create : Creates)
    {
        Times.emplace_back(Create.Delay);
    }

    return Times;
}

// Spark-on-hit-point (keyword 0x3f) with Bounce-fx, which sets all four sides, then Bounce-fx-up-left and the others one each (keywords 0x10 to 0x14).
std::array<std::string, 4> ReadBounceEffects(const FLevelPropertyBlock& Class)
{
    std::array<std::string, 4> Result;
    if (ParseLevelFloat(Class.Find("Spark-on-hit-point")).value_or(0.f) == 0.f)
    {
        return Result;
    }

    Result.fill(ToLowerName(Class.Find("Bounce-fx")));
    constexpr std::array<std::string_view, 4> Sides = { "Bounce-fx-up-left", "Bounce-fx-up-right", "Bounce-fx-down-left", "Bounce-fx-down-right" };
    for (size_t Side = 0; Side < Sides.size(); ++Side)
    {
        if (const std::string_view Name = Class.Find(Sides[Side]); !Name.empty())
        {
            Result[Side] = ToLowerName(Name);
        }
    }

    return Result;
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
        .NextCreates = FirstCreateTimes(Class->Creates),
    };
    // ObjectInitialize shows the first frame straight away.
    AdvanceEffect(Effect, *Class, 0);
    Combat.Effects.push_back(Effect);
}

// FUN_0043d140 for the player under Pinball (object +0xd4 8): the Flipper-Bong FX of fx.3dc, whatever the player's class says.
constexpr std::array<std::string_view, 4> PinballSparks = { "flipper-bong-up-left", "flipper-bong-up-right", "flipper-bong-down-left", "flipper-bong-down-right" };

// FUN_00436690: a point within a screen of the view each way, which the view, centred on the player, gives as within a screen and a half of the player.
// ponytail: measured from the player rather than the clamped camera; at the level's edges the margin is a half screen wider on that side.
bool IsNearView(const FCollisionPoint Point, const FCollisionPoint Viewer)
{
    constexpr float NearX = 640.f + 320.f;
    constexpr float NearY = 480.f + 240.f;
    return std::abs(Point.X - Viewer.X) < NearX && std::abs(Point.Y - Viewer.Y) < NearY;
}

// FUN_0043d140: a hard knock throws off one of four FX by the side the mover struck from, when the mover stands near the view (FUN_00436690: within a screen of it each way).
// Up-left of the struck point, the spark is the down-right one, and so on; it appears 8 pixels above the point, at whole pixels, unless one of its class already stands in that map cell.
void ThrowBounceSpark(FCombat& Combat, const std::array<std::string, 4>& Sides, const FCollisionPoint Mover, const FCollisionPoint Point, const FCollisionPoint Viewer)
{
    if (!IsNearView(Mover, Viewer))
    {
        return;
    }

    enum ESide : size_t
    {
        UpLeft,
        UpRight,
        DownLeft,
        DownRight,
    };
    const size_t Side = Mover.X < Point.X ? (Mover.Y < Point.Y ? DownRight : UpRight) : (Mover.Y < Point.Y ? DownLeft : UpLeft);
    const auto Class = std::ranges::find(Combat.EffectClasses, Sides[Side], &FEffectClass::Name);
    if (Sides[Side].empty() || Class == Combat.EffectClasses.end())
    {
        return;
    }

    constexpr float SparkRise = 8.f;
    const FCollisionPoint Spot{ .X = float(int32_t(Point.X)), .Y = float(int32_t(Point.Y - SparkRise)) };
    constexpr int CellWidthShift = 6;
    constexpr int CellHeightShift = 5;
    const auto Cell = [](const FCollisionPoint At) { return std::pair(uint32_t(int32_t(At.X)) >> CellWidthShift, uint32_t(int32_t(At.Y)) >> CellHeightShift); };
    const size_t ClassIndex = size_t(Class - Combat.EffectClasses.begin());
    if (std::ranges::any_of(Combat.Effects, [&](const FEffect& Effect) { return Effect.ClassIndex == ClassIndex && !Effect.bFinished && Cell(Effect.Position) == Cell(Spot); }))
    {
        return;
    }

    SpawnEffect(Combat, Sides[Side], Spot, 0.f);
}

// FUN_0041c180: each class in turn, from the last named, makes Count FX objects at the host, Phase spread over a full turn from the last copy back,
// at the class's altitude plus the host's.
void AttachOrbiters(FCombat& Combat, const std::optional<size_t> Host, const FCollisionPoint HostPosition, const float HostAltitude, const std::span<const std::string_view> ClassNames)
{
    constexpr float FullTurn = 6.2831855f;
    for (const std::string_view Name : std::views::reverse(ClassNames))
    {
        const auto Class = std::ranges::find(Combat.EffectClasses, Name, &FEffectClass::Name);
        if (Class == Combat.EffectClasses.end() || Class->Count <= 0)
        {
            continue;
        }

        const float Spacing = float(double(FullTurn) / double(float(Class->Count)));
        for (int Copy = Class->Count - 1; Copy >= 0; --Copy)
        {
            FOrbiter Orbiter{
                .Effect = {
                    .ClassIndex = size_t(Class - Combat.EffectClasses.begin()),
                    .Position = HostPosition,
                    .Altitude = float(Class->Altitude + int(HostAltitude)),
                },
                .Host = Host,
                .Phase = float(double(Copy) * double(Spacing) + double(Class->Phase)),
                .OnTimer = Class->OnTimeout,
                .OffTimer = Class->OffTimeout,
            };
            for (const FCreateBlock& Create : Class->Creates)
            {
                Orbiter.NextCreates.push_back(Create.Delay);
            }

            AdvanceEffect(Orbiter.Effect, *Class, 0, true);
            Combat.Orbiters.push_back(std::move(Orbiter));
        }
    }
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

// A CREATE block of a class (attribute kind 0x11). Distance (keyword 0xeb, +0x2c) is read but nothing uses it.
FCreateBlock ReadCreateBlock(const FLevelPropertyBlock& Child)
{
    const auto Number = [&](const std::string_view Field) { return ParseLevelFloat(Child.Find(Field)).value_or(0.f); };
    return {
        .Name = ToLowerName(Child.Find("Name")),
        .Offset = ParsePair(Child.Find("Offset")).value_or(FCollisionPoint{}),
        .Delay = uint32_t(Number("Delay")),
        .Repeat = uint32_t(Number("Repeat-timeout")),
        .bRandomDirection = Number("Random-dir") != 0.f,
        .bSetSpeed = Number("Set-speed") != 0.f,
        .Speed = Number("Speed"),
    };
}

// ObjectUpdateCreates (00437b00): each block whose time the object's age has reached makes its product at the object's place plus the block's offset, at most one per update,
// then waits its Repeat-timeout, or never again without one. The products are Fixed things that live their Life-Time; a launched one flies off (FUN_00413740).
void RunCreates(FCombat& Combat, const FLevel& Level, const std::span<const FCreateBlock> Creates, std::vector<std::optional<uint32_t>>& NextCreates, const uint32_t Age,
    const FCollisionPoint Position, const float Altitude, std::vector<FTrail>& Out)
{
    for (size_t Index = 0; Index < Creates.size() && Index < NextCreates.size(); ++Index)
    {
        std::optional<uint32_t>& Next = NextCreates[Index];
        const FCreateBlock& Create = Creates[Index];
        if (!Next || Age < *Next)
        {
            continue;
        }

        Next = Create.Repeat != 0 ? std::optional(*Next + Create.Repeat) : std::nullopt;
        const FLevelPropertyBlock* const Class = FindClass(Level, Create.Name);
        FTrail Trail{
            .ClassName = Create.Name,
            .Position = { .X = Position.X + Create.Offset.X, .Y = Position.Y + Create.Offset.Y },
            .Altitude = Altitude,
            .LifeTime = Class ? uint32_t(ClassNumber(*Class, "Life-Time")) : 0,
        };
        // The class as the level resolved it, with the library's DEFAULT-PROPERTY and Derived-From values.
        const auto Template = Level.ClassObjects.find(Create.Name);
        if (Template != Level.ClassObjects.end() && (Create.bRandomDirection || Create.bSetSpeed))
        {
            const FLevelPropertyBlock& Resolved = Template->second.Properties;
            // The direction comes from the Misc stream, swapped in around the draw (FUN_00428e20 and FUN_00428eb0 with session +0x259c), so the shared one is left alone.
            constexpr int LastDirection = DirectionCount - 1;
            const int Direction = Create.bRandomDirection ? Combat.MiscRandom.Range(0, LastDirection) : 0;
            std::expected<FMovementClass, std::string> Movement = ReadMovementProperties(Resolved, Create.Name, 0, false);
            if (Movement)
            {
                FTrail::FFlight Flight{ .Class = *Movement };
                if (Create.bSetSpeed)
                {
                    Flight.Class.SpeedMax = Create.Speed;
                }

                const float Thrust = Create.bSetSpeed ? Create.Speed : Movement->ThrustInitial;
                const FCollisionPoint Facing = FacingVector(Direction);
                Flight.State = {
                    .Position = Trail.Position,
                    .PreviousPosition = Trail.Position,
                    .Center = CollisionFromWorld(Trail.Position),
                    .Velocity = { .X = float(double(Thrust) * double(Facing.X)), .Y = float(double(Thrust) * double(Facing.Y)) },
                    .Speed = Thrust,
                    .Direction = Direction,
                };
                for (const FLevelPropertyBlock& Child : Resolved.Children)
                {
                    if (Child.Kind == "CREATE")
                    {
                        Flight.Creates.push_back(ReadCreateBlock(Child));
                    }
                }

                Flight.NextCreates = FirstCreateTimes(Flight.Creates);
                Trail.Flight = std::move(Flight);
            }
        }

        Out.push_back(std::move(Trail));
    }
}

// Adds an ammo class to the combat tables once.
std::expected<void, std::string> LoadAmmoClass(FCombat& Combat, const FLevel& Level, const std::string& AmmoName)
{
    if (std::ranges::contains(Combat.AmmoClasses, AmmoName, &FAmmoClass::Name))
    {
        return {};
    }

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

    const FLevelPropertyBlock* const EnergyBlock = FindBlock(*Ammo, "Energy");
    const std::optional<uint32_t> Behaviour = ParseBehaviour(Ammo->Find("Behaviour"));
    Combat.AmmoClasses.push_back({
        .Name = AmmoName,
        .Movement = Movement,
        .Damage = ClassNumber(*AmmoBlock, "Damage"),
        .DamageZone = ClassNumber(*AmmoBlock, "Damage-zone"),
        .DamageAfter = uint32_t(ClassNumber(*AmmoBlock, "Damage-after")),
        .HitEffect = ToLowerName(AmmoBlock->Find("Hit-fx")),
        .BounceEffects = ReadBounceEffects(*Ammo),
        .ListedDamage = ListedDamage,
        .ListedArea = ListedArea,
        .bUsableWhenInvisible = ClassNumber(*AmmoBlock, "Usable-when-invisible") != 0.f,
        .bBouncesOffTerrain = ParseLevelFloat(AmmoBlock->Find("On-hit")).value_or(1.f) == 0.f && ClassNumber(*AmmoBlock, "On-object-hit") != 0.f,
        .Energy = EnergyBlock ? ClassNumber(*EnergyBlock, "Initial") : 0.f,
        .bIndestructible = ClassNumber(*Ammo, "Indestructible") != 0.f,
        .bDropped = Behaviour && (*Behaviour & BehaviourMovementMask) == BehaviourFixed,
        .ActiveAfter = int32_t(ClassNumber(*AmmoBlock, "Active-after")),
    });
    for (const FLevelPropertyBlock& Child : Ammo->Children)
    {
        if (Child.Kind == "TYPE" && Child.Name == "Explosion")
        {
            Combat.AmmoClasses.back().Explosions.push_back({
                .BoomEffect = ToLowerName(Child.Find("Boom-fx")),
                .Damage = ClassNumber(Child, "Damage"),
                .Zone = ClassNumber(Child, "Damage-zone"),
            });
        }

        if (Child.Kind == "CREATE")
        {
            Combat.AmmoClasses.back().Creates.push_back(ReadCreateBlock(Child));
        }
    }

    return {};
}

// Adds a weapon class and the ammo classes of its blocks to the combat tables once; returns the weapon's index.
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
    if (const std::expected<void, std::string> Loaded = LoadAmmoClass(Combat, Level, AmmoName); !Loaded)
    {
        return std::unexpected(Loaded.error());
    }

    // The later blocks' ammo only widens what the weapon takes; one this level cannot load is left out.
    std::vector<std::string> AmmoChoices;
    for (const FLevelPropertyBlock& Child : Class->Children)
    {
        if (const std::string Choice = ToLowerName(Child.Find("Ammo")); Child.Kind == "TYPE" && Child.Name == "Weapon" &&
            (Choice == AmmoName || LoadAmmoClass(Combat, Level, Choice)))
        {
            AmmoChoices.push_back(Choice);
        }
    }

    const FSoundEmitter Channels = MakeSoundEmitter(Class->Find("Sample-channel-range"), Class->Find("Sample-channel-priority"), 0.f, 0.f, 0.f);
    Combat.Weapons.push_back({
        .Name = LowerName,
        .ClassName = std::string(Class->Find("Name")),
        .Ammo = AmmoName,
        .AmmoChoices = std::move(AmmoChoices),
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
        .FireSound = ToLowerName(Block->Find("Fire-sound")),
        .FireStartSound = ToLowerName(Block->Find("Fire-start-sound")),
        .FireEndSound = ToLowerName(Block->Find("Fire-end-sound")),
        .NoAmmoSound = ToLowerName(Block->Find("No-ammo-sound")),
        .bFireSoundLoops = ClassNumber(*Block, "Fire-sound-loop") != 0.f,
        .SoundChannelMask = Channels.ChannelMask,
        .SoundPriority = Channels.Priority,
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
    const FSoundEmitter Channels = MakeSoundEmitter(Class->Find("Sample-channel-range"), Class->Find("Sample-channel-priority"), 0.f, 0.f, 0.f);
    FEffectClass EffectClass{
        .Name = Name,
        .GafPath = GafPath->second,
        .GafIndex = GafIndex,
        .Animation = Animation,
        .ChannelMask = Channels.ChannelMask,
        .Priority = Channels.Priority,
        .LoopStart = Animation.LoopStart,
        .bLoop = ClassNumber(*Effect, "Loop") != 0.f,
    };
    EffectClass.bUniqAtXy = ClassNumber(*Class, "Uniq-at-xy") != 0.f;
    if (const float Stretch = ClassNumber(*Effect, "Stretch-ratio"); Stretch != 0.f)
    {
        EffectClass.Stretch = Stretch;
    }

    constexpr std::array<std::pair<std::string_view, EFxEvent>, 4> Events = { {
        { "bonus-invulnerability", EFxEvent::Invulnerability },
        { "bonus-touch-and-destroy", EFxEvent::TouchAndDestroy },
        { "malus-banana", EFxEvent::Banana },
        { "do-fx", EFxEvent::Always },
    } };
    const std::string EventName = ToLowerName(Effect->Find("Check-event"));
    const auto Event = std::ranges::find(Events, EventName, &std::pair<std::string_view, EFxEvent>::first);
    EffectClass.Count = int(ClassNumber(*Effect, "Count"));
    EffectClass.bCircular = ClassNumber(*Effect, "Circular") != 0.f;
    EffectClass.AngularSpeed = ClassNumber(*Effect, "Angular-speed");
    EffectClass.Ray = ClassNumber(*Effect, "Ray");
    EffectClass.Phase = ClassNumber(*Effect, "Phase");
    EffectClass.VerticalAmplitude = ClassNumber(*Effect, "Vertical-amplitude");
    EffectClass.VerticalPulsation = ClassNumber(*Effect, "Vertical-pulsation");
    EffectClass.VerticalPhase = ClassNumber(*Effect, "Vertical-phase");
    EffectClass.Event = Event != Events.end() ? Event->second : EFxEvent::None;
    EffectClass.OnTimeout = int32_t(ClassNumber(*Effect, "On-timeout"));
    EffectClass.OffTimeout = int32_t(ClassNumber(*Effect, "Off-timeout"));
    EffectClass.Altitude = int(ClassNumber(*Class, "Altitude"));
    for (const FLevelPropertyBlock& Child : Class->Children)
    {
        if (Child.Kind == "CREATE")
        {
            EffectClass.Creates.push_back(ReadCreateBlock(Child));
        }
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
    // Object +0x114: Triple Fire's spread, which makes each shot three.
    int32_t Spread = 0;
};

enum class EWeaponFire : uint8_t
{
    NotReady,
    Fired,
    // WeaponFire's failure (no ammo), which also stops Circular Fire's turning (object +0x104).
    Failed,
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
    const uint32_t Delay = State.Delay.value_or(Combat.Weapons[State.Weapon].Delay);
    return State.LastFireTime == 0 || State.LastFireTime == Combat.Clock || Combat.Clock - State.LastFireTime >= Delay;
}

}

FCollisionPoint WorldFacingOffset(const int Direction, const float Distance)
{
    const FCollisionPoint Vector = WorldFacingVector(Direction);
    return { .X = float(double(Distance) * double(Vector.X)), .Y = float(double(Distance) * double(Vector.Y)) };
}

namespace
{

// WeaponFire at 00443bd0.
EWeaponFire FireWeapon(FCombat& Combat, FWeaponState& State, const FShooter& Shooter, FAmmoStock* const Stock)
{
    State.bSoundTrigger = true;
    if (State.LastFireTime == 0)
    {
        State.LastFireTime = Combat.Clock;
    }

    if (!WeaponReady(Combat, State))
    {
        return EWeaponFire::NotReady;
    }

    State.LastFireTime = Combat.Clock;
    const FWeaponClass& Weapon = Combat.Weapons[State.Weapon];
    // The shot is the bound stock's ammo (object +0x250), which may be any of the weapon's blocks'; a stock of an unloaded class shoots the first block's.
    const auto StockAmmo = Stock ? std::ranges::find(Combat.AmmoClasses, ToLowerName(Stock->Name), &FAmmoClass::Name) : Combat.AmmoClasses.end();
    const auto Ammo = StockAmmo != Combat.AmmoClasses.end() ? StockAmmo : std::ranges::find(Combat.AmmoClasses, Weapon.Ammo, &FAmmoClass::Name);
    // The player's weapon switches when it runs dry (WeaponHasAmmo at 00444450, in StepPlayerWeapon); the original would switch the player's weapon for any shooter, but only the player ever runs out.
    if (!Stock || Stock->Count < 1.f || Ammo == Combat.AmmoClasses.end() || Shooter.Direction == NoDirection)
    {
        return EWeaponFire::Failed;
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
    bool bRoundTaken = false;
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

        // With a spread each side fires three shots: Spread out along the facing six directions round one way, then the other, then the middle one.
        constexpr int SpreadDirections = 6;
        constexpr std::array<int, 3> SpreadTurns = { SpreadDirections, -SpreadDirections, 0 };
        const FCollisionPoint Direction = WorldFacingVector(Shooter.Direction);
        bool bEmpty = false;
        for (size_t Turn = Shooter.Spread != 0 ? 0 : SpreadTurns.size() - 1; Turn < SpreadTurns.size() && !bEmpty; ++Turn)
        {
            FCollisionPoint Base = Shooter.Position;
            if (SpreadTurns[Turn] != 0)
            {
                const FCollisionPoint Across = WorldFacingVector(Wrap(Shooter.Direction + SpreadTurns[Turn]));
                Base = {
                    .X = float(double(float(double(Shooter.Spread) * double(Across.X))) + double(Shooter.Position.X)),
                    .Y = float(double(float(double(Shooter.Spread) * double(Across.Y))) + double(Shooter.Position.Y)),
                };
            }

            FCollisionPoint Muzzle{
                .X = float(double(float(double(Distance) * double(Direction.X))) + double(Base.X)),
                .Y = float(double(float(double(Distance) * double(Direction.Y))) + double(Base.Y)),
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
                .Energy = Ammo->Energy,
            };
            for (const FCreateBlock& Create : Ammo->Creates)
            {
                Shot.NextCreates.push_back(Create.Delay);
            }

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

            if (Ammo->bDropped)
            {
                Combat.DroppedMines.push_back({ .AmmoIndex = Shot.AmmoIndex, .Position = Muzzle, .Direction = Shooter.Direction, .Owner = Shooter.ObjectIndex });
            }
            else
            {
                Combat.Projectiles.push_back(Shot);
            }

            if (!Combat.bOneRoundPerVolley || !bRoundTaken)
            {
                Stock->Count -= 1.f;
                bRoundTaken = true;
            }

            bEmpty = Stock->Count < 1.f;
        }

        if (bEmpty)
        {
            break;
        }
    }

    return EWeaponFire::Fired;
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

namespace
{
Sound::FMixer* AnimationMixer = nullptr;
// SoundSetListener's position, which the positional sounds measure from (DAT_00519430).
FSoundEmitter Listener;

// SoundDistanceVolume at 0045e4e0: full volume while the emitter lies within the descriptor's inner box (+0x28) on every axis, nothing beyond its outer box (+0x34).
// In between the original scales by the distance between two radii (+0x40, +0x44) that AnimStampSoundChannels (00417a20) never sets, so the division by zero leaves nothing there either.
// The boxes are DAT_0050d420 (400 on each axis) and DAT_0050d430 (800).
int PositionalVolume(const FSoundEmitter& Emitter, const int Volume)
{
    constexpr float InnerBox = 400.f;
    const bool bInside = std::abs(Listener.X - Emitter.X) <= InnerBox && std::abs(Listener.Y - Emitter.Y) <= InnerBox &&
        std::abs(Listener.Altitude - Emitter.Altitude) <= InnerBox;
    return bInside ? Volume : 0;
}

// SoundScreenPan at 0045e630: the emitter's offset right of the listener over half the 640-pixel screen, as a full pan at the edge.
int PositionalPan(const FSoundEmitter& Emitter)
{
    constexpr float HalfScreen = 320.f;
    constexpr float FullPan = float(Sound::MaxPan);
    const float Offset = Listener.X - Emitter.X;
    if (Offset < 0.f)
    {
        return -Offset < HalfScreen ? TruncateLow(double(Offset) * double(-FullPan) / double(HalfScreen)) : Sound::MaxPan;
    }

    return Offset < HalfScreen ? TruncateLow(double(Offset) * double(-FullPan) / double(HalfScreen)) : -Sound::MaxPan;
}
}

void SetAnimationMixer(Sound::FMixer* const Mixer)
{
    AnimationMixer = Mixer;
}

void SetSoundListener(const float X, const float Y, const float Altitude)
{
    Listener = { .X = X, .Y = Y, .Altitude = Altitude };
}

FSoundEmitter MakeSoundEmitter(const std::string_view ChannelRange, const std::string_view Priority, const float X, const float Y, const float Altitude)
{
    FSoundEmitter Emitter{ .X = X, .Y = Y, .Altitude = Altitude };
    int First = 0;
    int Last = 0;
    if (std::sscanf(std::string(ChannelRange).c_str(), "%d , %d", &First, &Last) == 2)
    {
        if (Last < First)
        {
            std::swap(First, Last);
        }

        for (int Channel = std::max(First, 1); Channel <= std::min(Last, int(Sound::FMixer::ChannelCount)); ++Channel)
        {
            Emitter.ChannelMask |= 1u << (Channel - 1);
        }
    }

    Emitter.Priority = uint32_t(std::clamp(std::atoi(std::string(Priority).c_str()), 0, 0xff));
    return Emitter;
}

// AnimationFrameSound (0045ea70) picks a frame's sample by event flag 1 and its event value, and AnimationPlaySound (0045e6e0) plays it.
// The descriptor's volume is the sound-fx channel volume when the class was set up (SoundGetVolume(2) in AnimStampSoundChannels (00417a20)).
void PlayFrameSound(const Gaf::FAnimation& Animation, const Gaf::FAnimationFrame& Frame, FSoundTrack& Track, const FSoundEmitter* const Emitter)
{
    constexpr uint32_t SoundEventFlag = 1;
    constexpr uint32_t AnywherePriority = 0x7f;
    constexpr uint32_t SoundFxChannel = 2;
    if (!AnimationMixer || (Frame.EventFlags & SoundEventFlag) == 0 || !Animation.Samples || Frame.EventValue == 0 || Frame.EventValue > Animation.Samples->size())
    {
        return;
    }

    const std::shared_ptr<const Sound::FSample> Sample(Animation.Samples, &(*Animation.Samples)[Frame.EventValue - 1]);
    if (!Emitter || Emitter->ChannelMask == 0)
    {
        AnimationMixer->Play(Sample, false, AnywherePriority, Sound::FMixer::AllChannels);
        return;
    }

    const int Volume = PositionalVolume(*Emitter, AnimationMixer->GetVolume(SoundFxChannel));
    if (Volume <= 0)
    {
        return;
    }

    const uint32_t Channel = AnimationMixer->Play(Sample, false, Emitter->Priority, Emitter->ChannelMask, Volume, PositionalPan(*Emitter));
    Track = { .Channel = Channel, .Serial = Channel == 0 ? 0 : AnimationMixer->GetSerial(Channel) };
}

// UpdateAnimation keeps a positional sound's volume and pan on its emitter while the sound plays, and stops it once out of range.
void FollowFrameSound(FSoundTrack& Track, const FSoundEmitter* const Emitter)
{
    if (!AnimationMixer || Track.Channel == 0)
    {
        return;
    }

    constexpr uint32_t SoundFxChannel = 2;
    if (Emitter && AnimationMixer->GetSerial(Track.Channel) == Track.Serial)
    {
        if (const int Volume = PositionalVolume(*Emitter, AnimationMixer->GetVolume(SoundFxChannel)); Volume > 0)
        {
            AnimationMixer->SetVolume(Track.Channel, Volume);
            AnimationMixer->SetPan(Track.Channel, PositionalPan(*Emitter));
            return;
        }

        AnimationMixer->Stop(1u << (Track.Channel - 1));
    }

    Track = {};
}

void UpdateAnimation(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, const int32_t Milliseconds, const FSoundEmitter* const Emitter)
{
    constexpr uint32_t LoopFlag = 2;
    if (Playback.bEnded)
    {
        return;
    }

    Playback.Timer -= Milliseconds;
    if (Playback.Timer > 0)
    {
        FollowFrameSound(Playback.Sound, Emitter);
    }

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
        PlayFrameSound(Animation, Animation.Frames[Playback.Frame - 1], Playback.Sound, Emitter);
    }

    FollowFrameSound(Playback.Sound, Emitter);
}

FAnimationPlayback StartAnimation(const Gaf::FAnimation& Animation, const FSoundEmitter* const Emitter)
{
    FAnimationPlayback Playback{ .bEnded = Animation.Frames.empty() };
    UpdateAnimation(Playback, Animation, 0, Emitter);
    return Playback;
}

void ShowFrame(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, const uint32_t Frame, const FSoundEmitter* const Emitter)
{
    if (Frame == 0 || Frame > Animation.Frames.size())
    {
        Playback = { .bEnded = true };
        return;
    }

    Playback.Frame = Frame - 1;
    UpdateAnimation(Playback, Animation, Playback.Timer, Emitter);
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
            .Emitter = MakeSoundEmitter(Object.Properties.Find("Sample-channel-range"), Object.Properties.Find("Sample-channel-priority"), Object.X, Object.Y,
                Object.Altitude),
        };
        // AnimInitializeGraphics shows the last frame of the animation for the activation state, and the timer starts full.
        // Its fresh animation instance has no position yet (ObjectDraw gives it one), so a sound on that frame plays from the origin.
        const bool bOpen = (Door.Flags & DoorOpen) != 0;
        FSoundEmitter Fresh = Door.Emitter;
        Fresh.X = Fresh.Y = Fresh.Altitude = 0.f;
        Door.Animation = bOpen ? OpenAnimation : ClosedAnimation;
        Door.Playback = StartAnimation(Door.Animations[Door.Animation], &Fresh);
        ShowFrame(Door.Playback, Door.Animations[Door.Animation], uint32_t(Door.Animations[Door.Animation].Frames.size()), &Fresh);
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
        UpdateAnimation(Door.Playback, Animation, Step, &Door.Emitter);
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
            .Text = std::string(Localize(Door->bLift ? "Cannot take lift while invisible !" : "Cannot open gates while invisible !")),
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
        Text = Format(!bNeedsKey ? "Wait, lift will come." : bCode ? "Lift needs full code !" : "Lift needs key #%d !", Door->Id);
    }
    else if (bNeedsKey)
    {
        // The gate also says what it needs, held for 5 seconds.
        Text = Format(bCode ? "Gate needs full code #%d !" : "Gate needs key #%d !", Door->Id);
        constexpr uint32_t NeedDuration = 1000;
        constexpr uint32_t NeedHold = 5000;
        QueueVoice(bCode ? EVoice::NeedCode : EVoice::NeedKey, 0, NeedDuration, NeedHold);
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
void StartSurprise(FCombat& Combat, const FLevel& Level, std::vector<FObjectBody>& Bodies, FPlayerState& Player, FMovementClass& Class, FPlayerInventory& Inventory,
    const ESurprise Type)
{
    FSurpriseState& State = Inventory.Surprises;
    if (State.Active != ESurprise::None)
    {
        return;
    }

    // FUN_0041c340 attaches the orbiting FX of the tables at 0050d628, 0050d62c and 0050d638 to the player.
    const std::optional<size_t> PlayerFound = FindPlayerObject(Level);
    const float PlayerAltitude = PlayerFound ? Level.Objects[*PlayerFound].Altitude : 0.f;
    const auto Attach = [&](const std::span<const std::string_view> Names) { AttachOrbiters(Combat, std::nullopt, Player.Position, PlayerAltitude, Names); };
    constexpr std::array<std::string_view, 1> BananaFx = { "no-inertia-control-fx-1" };
    constexpr std::array<std::string_view, 3> TouchAndDestroyFx = { "touch-and-destroy-fx-1", "touch-and-destroy-fx-2", "touch-and-destroy-fx-3" };
    constexpr std::array<std::string_view, 5> InvulnerabilityFx = {
        "invulnerability-fx-1", "invulnerability-fx-2", "invulnerability-fx-3", "invulnerability-fx-4", "invulnerability-fx-5",
    };
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
        constexpr int32_t TrapFlash = 100;
        constexpr int32_t SurpriseFlashLimit = 150;
        PushGamma(State.Gamma, TrapFlash, SurpriseFlashLimit);
        break;
    }
    case ESurprise::WeaponJam:
        State.JammedWeapon = Combat.PlayerWeapon.Weapon;
        break;
    case ESurprise::Extasy:
    {
        // SurpriseStart cheers Ariba or Yeepee by the game clock, three seconds in.
        constexpr uint32_t ExtasyVoiceMask = 3;
        constexpr uint32_t FirstYeepee = 2;
        constexpr uint32_t ExtasyVoiceDelay = 3000;
        constexpr uint32_t ExtasyVoiceDuration = 2000;
        QueueVoice((GetVoiceClock() & ExtasyVoiceMask) < FirstYeepee ? EVoice::Ariba : EVoice::Yeepee, ExtasyVoiceDelay, ExtasyVoiceDuration, 0);
        State.SpeedMax = Class.SpeedMax;
        Class.SpeedMax = Class.SpeedMax + Class.SpeedMax;
        break;
    }
    case ESurprise::AutoShieldUp:
    {
        constexpr float AutoShieldUpStep = 10.f;
        Inventory.ShieldUpStep += AutoShieldUpStep;
        break;
    }
    case ESurprise::LowSpeed:
    {
        // DAT_00509568.
        constexpr float LowSpeedScale = 0.25f;
        State.SpeedMax = Class.SpeedMax;
        Class.SpeedMax *= LowSpeedScale;
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
        Attach(BananaFx);
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
        constexpr int32_t DestroyFlash = 200;
        constexpr int32_t SurpriseFlashLimit = 150;
        PushGamma(State.Gamma, DestroyFlash, SurpriseFlashLimit);
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
        if (!State.bInvulnerable)
        {
            State.bInvulnerable = true;
            Attach(InvulnerabilityFx);
        }

        break;
    case ESurprise::TripleFire:
        State.bTripleFire = true;
        break;
    case ESurprise::TouchAndDestroy:
        Player.bTouchAndDestroy = true;
        Attach(TouchAndDestroyFx);
        break;
    case ESurprise::TimeWarp:
    {
        constexpr int32_t WarpDivisor = 3;
        State.WarpDivisor = WarpDivisor;
        State.WarpCounter = 0;
        break;
    }
    default:
        break;
    }

    State.Active = Type;
}
}

std::vector<FSavingPlot> FindSavingPlots(const FLevel& Level)
{
    const auto Flag = [&](const FLevelObject& Object, const std::string_view Field) { return ParseLevelFloat(Object.Properties.Find(Field)).value_or(0.f) != 0.f; };
    std::vector<FSavingPlot> Plots;
    for (size_t Zone = 0; Zone < Level.Objects.size(); ++Zone)
    {
        const FLevelObject& Object = Level.Objects[Zone];
        if (!Flag(Object, "Saving-zone"))
        {
            continue;
        }

        FSavingPlot& Saving = Plots.emplace_back(FSavingPlot{ .Zone = Zone });
        for (size_t Plot = 0; Plot < Level.Objects.size(); ++Plot)
        {
            const FLevelObject& Candidate = Level.Objects[Plot];
            if (Plot != Zone && Flag(Candidate, "Saving-plot") && Candidate.X == Object.X && Candidate.Y == Object.Y)
            {
                Saving.Plot = Plot;
                break;
            }
        }
    }

    return Plots;
}

bool StepSavingPlots(FSavingState& State, const std::span<const FSavingPlot> Plots, const FLevel& Level, const FPlayerState& Player, const float PlayerRadius,
    FPlayerInventory& Inventory, FMessages& Messages, const uint32_t NowMilliseconds, const std::string_view PlayerName)
{
    // PgClockNow time: 5.5 seconds of standing, counted down from six.
    constexpr int32_t SaveDelay = 0x157c;
    constexpr int32_t CountFrom = 6000;
    constexpr int32_t MillisecondsPerSecondSigned = int32_t(MillisecondsPerSecond);
    constexpr int32_t TextX = -0x96;
    constexpr int32_t TextY = -0x32;
    constexpr uint32_t CountLife = 500;
    constexpr uint32_t RefusedLife = 5000;
    constexpr uint32_t SavedLife = 3000;
    constexpr uint32_t LeftLife = 1000;
    constexpr uint32_t Font = 6;
    const auto Say = [&](const FLevelObject& Plot, std::string Text, const uint32_t Life)
    {
        Messages.Add({
            .Text = std::move(Text),
            .X = float(TruncateLow(Plot.X) + TextX),
            .Y = float(TruncateLow(Plot.Y) + TextY),
            .LifeMilliseconds = Life,
            .bWorld = true,
            .Font = Font,
        }, EMessagePlacement::Replace);
    };

    bool bContact = false;
    bool bSave = false;
    for (const FSavingPlot& Saving : Plots)
    {
        const FLevelObject& Zone = Level.Objects[Saving.Zone];
        const FCollisionCircle ZoneCircle{ .Center = CollisionFromWorld({ .X = Zone.X, .Y = Zone.Y }), .Radius = float(Zone.BoundingBoxSize) };
        if (Inventory.Energy <= 0.f || Zone.bRemoved || !IntersectCircles({ .Center = Player.Center, .Radius = PlayerRadius }, ZoneCircle))
        {
            continue;
        }

        bContact = true;
        if (!Saving.Plot)
        {
            continue;
        }

        const FLevelObject& Plot = Level.Objects[*Saving.Plot];
        State.Plot = Saving.Plot;
        if (!State.bCounting)
        {
            State.bCounting = true;
            State.Start = NowMilliseconds;
            State.bDone = false;
            continue;
        }

        const int32_t Elapsed = int32_t(NowMilliseconds - State.Start);
        if (Elapsed < SaveDelay)
        {
            const int32_t Seconds = (CountFrom - Elapsed) / MillisecondsPerSecondSigned;
            Say(Plot, Format("Saving in %d second%s", Seconds, Seconds > 1 ? "s" : ""), CountLife);
            continue;
        }

        if (State.bDone)
        {
            continue;
        }

        State.bDone = true;
        // The plot's Reward is its cost, paid from the player's cash.
        const int32_t Reward = int32_t(ParseLevelFloat(Plot.Properties.Find("Reward")).value_or(0.f));
        if (Reward < 0 && Inventory.Money < float(-Reward))
        {
            Say(Plot, Format("Not enough Cash %s !\n You need %d $\n  to save this game ...", PlayerName, -Reward), RefusedLife);
            constexpr uint32_t RefusedVoiceDuration = 1000;
            constexpr uint32_t RefusedVoiceHold = 5000;
            QueueVoice(EVoice::NeedCash, 0, RefusedVoiceDuration, RefusedVoiceHold);
            continue;
        }

        if (Reward < 0)
        {
            Inventory.Money += float(Reward);
        }

        Say(Plot, std::string(Localize("Saving Completed.")), SavedLife);
        constexpr uint32_t SavedVoiceDelay = 500;
        constexpr uint32_t SavedVoiceDuration = 1000;
        constexpr uint32_t SavedVoiceHold = 3000;
        QueueVoice(EVoice::GameSaved, SavedVoiceDelay, SavedVoiceDuration, SavedVoiceHold);
        bSave = true;
    }

    if (!bContact && State.bCounting)
    {
        if (!State.bDone && State.Plot)
        {
            const FLevelObject& Plot = Level.Objects[*State.Plot];
            Messages.RemoveAt(TruncateLow(Plot.X) + TextX, TruncateLow(Plot.Y));
            Say(Plot, std::string(Localize("No Saving.")), LeftLife);
        }

        State.bCounting = false;
        State.Plot.reset();
    }

    return bSave;
}

void StepCodeTimeouts(FPlayerInventory& Inventory, FLevel& Level, FMessages& Messages, const uint32_t StepMilliseconds)
{
    constexpr uint32_t LifeMilliseconds = 2000;
    constexpr uint32_t Font = 6;
    const int32_t Step = int32_t(StepMilliseconds);
    // ponytail: a Random-Location code (FUN_004396b0 mode 2) would return to a random open cell; no level places one.
    std::vector<int> Expired;
    std::vector<int> Returned;
    for (FCarriedKey& Key : Inventory.Keys | std::views::reverse)
    {
        if (!Key.bCode || Key.PartId != 1 || !CarriesKey(Inventory.Keys, Key.Id) || Key.Timeout < 1)
        {
            continue;
        }

        // FUN_00439410: the count and its warnings, one each time the seconds left are a multiple of the warning time's.
        Key.Timeout -= Step;
        const int32_t Seconds = Key.Timeout / int32_t(MillisecondsPerSecond);
        const int32_t WarnSeconds = Key.WarnTimeout / int32_t(MillisecondsPerSecond);
        if (Key.WarnTimeout != 0 && !Key.WarnFormat.empty() && Seconds != 0 && WarnSeconds != 0 && Seconds % WarnSeconds == 0)
        {
            std::string Text = Key.WarnFormat;
            if (const size_t Number = Text.find("%d"); Number != std::string::npos)
            {
                Text.replace(Number, 2, std::to_string(Seconds));
            }

            Messages.Add({ .Text = std::move(Text), .X = float(Key.WarnX), .Y = float(Key.WarnY), .LifeMilliseconds = LifeMilliseconds, .Font = Font },
                EMessagePlacement::Replace);
        }

        if (Key.Timeout < 1)
        {
            Key.Timeout = 0;
            if (Key.ReactivateAfter < 1)
            {
                Expired.push_back(Key.Id);
            }
            else if (Key.bHasTimeout)
            {
                Returned.push_back(Key.Id);
                Messages.Add({ .Text = Format("Code #%d regenerated !", Key.Id), .X = float(Key.WarnX), .Y = float(Key.WarnY),
                    .LifeMilliseconds = LifeMilliseconds, .Font = Font }, EMessagePlacement::Replace);
            }
        }
    }

    // FUN_004396b0 and FUN_00439870: an expired code's parts are destroyed, a returning one's lie again where they were picked up.
    std::erase_if(Inventory.Keys, [&](const FCarriedKey& Key)
    {
        if (std::ranges::contains(Returned, Key.Id))
        {
            Level.Objects[Key.ObjectIndex].bRemoved = false;
            return true;
        }

        return std::ranges::contains(Expired, Key.Id);
    });
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

void ActivateGenerators(const FLevel& Level, std::vector<FObjectBody>& Bodies, const FPlayerTarget& Player)
{
    for (FObjectBody& Body : Bodies)
    {
        if (!Body.Generator)
        {
            continue;
        }

        // The test runs with the generator as it stands, then sets flag 2 and the open bit (object +0x5c bit 1) one way or the other.
        const bool bBlocked = ObjectOverlapsSolid(Level, BodiesByObject(Level, Bodies), Player, Body);
        Body.Generator->bBirthing = !bBlocked;
        Body.bPassable = bBlocked;
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

namespace
{
// LevelStartMapAnimations at 0041f0b0, which VzLoadLevel runs after its reseed and before EngineActivateObjects: every .3DX descriptor whose GAF lies two folders below an Anim directory (Level3dxResolveEntry's kind 1) starts its named animation on RandomRange(1, frames).
// A name the GAF lacks drops the descriptor without a draw. The decor objects only take the descriptors' GAF and animation; their own animation starts at its first frame, so the start frames only keep the draws.
int32_t DecorDepthBias(const ETerrain Terrain)
{
    if (Terrain == ETerrain::Ground || Terrain == ETerrain::Water)
    {
        return 0;
    }

    return Terrain == ETerrain::Roof ? int32_t(2 * BlockHeight) : int32_t(BlockHeight);
}

std::vector<FDecorAnimation> StartMapAnimations(const FLevel& Level, FResourceArchive& Archive, FRandom& Random)
{
    std::unordered_map<int, std::vector<Gaf::FAnimation>> Loaded;
    // By descriptor, 1-based like the .3DI tile ids: the GAF and the animation index of a loaded Anim descriptor.
    std::vector<std::optional<std::pair<int, size_t>>> Descriptors(Level.TileReferences.size() + 1);
    for (size_t Descriptor = 1; Descriptor <= Level.TileReferences.size(); ++Descriptor)
    {
        const auto& [Gaf, Name] = Level.TileReferences[Descriptor - 1];
        if (Gaf < 1 || size_t(Gaf) > Level.AnimGafs.size())
        {
            continue;
        }

        const std::string& Path = Level.AnimGafs[size_t(Gaf - 1)];
        const size_t File = Path.rfind('\\');
        const size_t Folder = File == std::string::npos || File == 0 ? std::string::npos : Path.rfind('\\', File - 1);
        const size_t Kind = Folder == std::string::npos || Folder == 0 ? std::string::npos : Path.rfind('\\', Folder - 1);
        if (Kind == std::string::npos || ToLowerName(std::string_view(Path).substr(Kind + 1, Folder - Kind - 1)) != "anim")
        {
            continue;
        }

        auto [Animations, bNew] = Loaded.try_emplace(Gaf);
        if (bNew)
        {
            const std::optional<std::vector<uint8_t>> Data = Archive.Read(Path);
            const std::optional<std::vector<Gaf::FFrame>> Bitmaps = Data ? Gaf::LoadFrames(*Data) : std::nullopt;
            std::optional<std::vector<Gaf::FAnimation>> Decoded = Bitmaps ? Gaf::LoadAnimations(*Data, Bitmaps->size()) : std::nullopt;
            Animations->second = Decoded ? std::move(*Decoded) : std::vector<Gaf::FAnimation>{};
        }

        const auto Animation = std::ranges::find(Animations->second, Name, &Gaf::FAnimation::Name);
        if (Animation != Animations->second.end())
        {
            Random.Range(1, int(Animation->Frames.size()));
            Descriptors[Descriptor] = std::pair(Gaf, size_t(Animation - Animations->second.begin()));
        }
    }

    constexpr int StartDelayRange = 10000;
    const uint32_t Height = Level.GridHeight;
    std::vector<FDecorAnimation> Decor;
    for (size_t Cell = 0; Cell < Level.Cells.size(); ++Cell)
    {
        for (const uint16_t Tile : Level.Cells[Cell])
        {
            if (Tile == 0 || Tile >= Descriptors.size() || !Descriptors[Tile])
            {
                continue;
            }

            const auto [Gaf, Animation] = *Descriptors[Tile];
            const std::vector<Gaf::FAnimation>& Animations = Loaded[Gaf];
            Decor.push_back({
                .X = float(Cell / Height * BlockWidth),
                .Y = float(Cell % Height * BlockHeight),
                .GafPath = Level.AnimGafs[size_t(Gaf - 1)],
                .Animation = Animation,
                .DepthBias = DecorDepthBias(Level.TileTypes[Tile]),
                .StartMilliseconds = uint32_t(Random.Below(StartDelayRange)),
                .Playback = StartAnimation(Animations[Animation]),
            });
        }
    }

    return Decor;
}
}

std::expected<FCombat, std::string> CreateCombat(FLevel& Level, const FLevelObject& Player, FResourceArchive& Archive, std::vector<FObjectBody>& Bodies,
    const bool bDemo)
{
    FCombat Combat;
    Combat.DecorAnimations = StartMapAnimations(Level, Archive, Combat.Random);
    if (bDemo)
    {
        Combat.Random = FRandom{};
    }

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

        FTurret Turret{
            .VisionDirections = VisionDirections(ClassNumber(*Seeker, "Vision-Angle")),
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

    std::vector<std::string> EffectNames = { std::string(GroundImpactEffect), std::string(WaterSplashEffect) };
    EffectNames.insert(EffectNames.end(), PinballSparks.begin(), PinballSparks.end());
    for (const FAmmoClass& Ammo : Combat.AmmoClasses)
    {
        EffectNames.push_back(Ammo.HitEffect);
        for (const std::string& Spark : Ammo.BounceEffects)
        {
            if (!Spark.empty())
            {
                EffectNames.push_back(Spark);
            }
        }

        for (const FExplosionBlock& Explosion : Ammo.Explosions)
        {
            EffectNames.push_back(Explosion.BoomEffect);
        }
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
    // The orbiting FX: the surprises' (FUN_0041c340's tables) and each object's ADD FX.
    for (const std::string_view Name : { "no-inertia-control-fx-1", "touch-and-destroy-fx-1", "touch-and-destroy-fx-2", "touch-and-destroy-fx-3", "invulnerability-fx-1",
             "invulnerability-fx-2", "invulnerability-fx-3", "invulnerability-fx-4", "invulnerability-fx-5" })
    {
        EffectNames.emplace_back(Name);
    }

    std::vector<std::pair<const FObjectBody*, std::vector<std::string>>> AddedFx;
    for (const FObjectBody& Body : Bodies)
    {
        std::vector<std::string> Names;
        for (const FLevelPropertyBlock& Child : Level.Objects[Body.ObjectIndex].Properties.Children)
        {
            if (Child.Kind == "ADD" && Child.Name == "FX")
            {
                Names.push_back(ToLowerName(Child.Find("Name")));
                EffectNames.push_back(Names.back());
            }
        }

        if (!Names.empty())
        {
            AddedFx.emplace_back(&Body, std::move(Names));
        }
    }

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

    // CreateObject (00413800) attaches each ADD FX of the class as the object is made.
    for (const auto& [Body, Names] : AddedFx)
    {
        const std::vector<std::string_view> Views(Names.begin(), Names.end());
        AttachOrbiters(Combat, Body->ObjectIndex, Body->State.Position, Body->Altitude, Views);
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
// ObjectBindAmmo at 00437540: the first of the player's stocks, in their order, that is not empty and holds an ammo one of the weapon's blocks takes.
// When all are empty the first that a block takes stands in, so an empty weapon still knows its stock.
template <typename FInventory>
auto BoundAmmo(const FWeaponClass& Weapon, FInventory& Inventory) -> decltype(&Inventory.Ammo.front())
{
    const auto Takes = [&Weapon](const FAmmoStock& Entry) { return std::ranges::contains(Weapon.AmmoChoices, ToLowerName(Entry.Name)); };
    const auto Full = std::ranges::find_if(Inventory.Ammo, [&](const FAmmoStock& Entry) { return Entry.Count != 0.f && Takes(Entry); });
    const auto Any = Full != Inventory.Ammo.end() ? Full : std::ranges::find_if(Inventory.Ammo, Takes);
    return Any == Inventory.Ammo.end() ? nullptr : &*Any;
}

constexpr size_t WeaponSlotCount = 9;
}

const FAmmoStock* SlotAmmo(const FCombat& Combat, const size_t Slot, const FPlayerInventory& Inventory)
{
    if (Slot == 0 || Slot > Combat.PlayerWeapons.size())
    {
        return nullptr;
    }

    return BoundAmmo(Combat.Weapons[Combat.PlayerWeapons[Slot - 1]], Inventory);
}

void ClearLastShots(FCombat& Combat)
{
    Combat.PlayerWeapon.LastFireTime = 0;
    for (FWeaponState& State : Combat.PlayerSlotStates)
    {
        State.LastFireTime = 0;
    }
}

void ThrowPlayerBounceSpark(FCombat& Combat, const FPlayerState& Player, const ESurprise Active)
{
    if (Active == ESurprise::Pinball && Player.KnockPoint)
    {
        const std::array<std::string, 4> Sides = { std::string(PinballSparks[0]), std::string(PinballSparks[1]), std::string(PinballSparks[2]), std::string(PinballSparks[3]) };
        ThrowBounceSpark(Combat, Sides, Player.Position, *Player.KnockPoint, Player.Position);
    }
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

size_t PreviousPlayerWeaponSlot(const FCombat& Combat, const FPlayerInventory& Inventory)
{
    const size_t Current = Combat.PlayerSlot;
    if (Current == 0 || Current > WeaponSlotCount)
    {
        return Current;
    }

    for (size_t Slot = (Current + WeaponSlotCount - 2) % WeaponSlotCount + 1; Slot != Current; Slot = (Slot + WeaponSlotCount - 2) % WeaponSlotCount + 1)
    {
        const FAmmoStock* const Stock = SlotAmmo(Combat, Slot, Inventory);
        if (Stock && Stock->Count >= 1.f)
        {
            return Slot;
        }
    }

    return Current;
}

const FAmmoStock* PlayerSlotAmmo(const FCombat& Combat, const FPlayerInventory& Inventory)
{
    return SlotAmmo(Combat, Combat.PlayerSlot, Inventory);
}

void SelectPlayerWeapon(FCombat& Combat, const size_t Slot, const FPlayerInventory& Inventory, FMessages& Messages)
{
    // FUN_004375f0 keeps the weapon in hand's state with its ADD Weapon entry and clears the new one's last shot.
    if (Combat.PlayerSlot >= 1 && Combat.PlayerSlot <= Combat.PlayerSlotStates.size())
    {
        Combat.PlayerSlotStates[Combat.PlayerSlot - 1] = Combat.PlayerWeapon;
    }

    Combat.PlayerSlot = Slot;
    ++Combat.WeaponSwitches;
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
    // PlayerSelectWeapon lists the weapon only with HUD Green Info.
    if (!Combat.Details.bGreenInfo)
    {
        return;
    }

    const FWeaponClass& Weapon = Combat.Weapons[Combat.PlayerWeapon.Weapon];
    List(WeaponLines[0], Format("WEAPON      %s", Upper(Weapon.Name)));
    List(WeaponLines[1], Format("FIRE-RANGE  %d", int32_t(Weapon.FireRange)));
    List(WeaponLines[2], Format("FIRE-DELAY  %d", Weapon.Delay));

    // FUN_00437540 binds only a stock that is not empty.
    const FAmmoStock* const Stock = SlotAmmo(Combat, Slot, Inventory);
    const auto Ammo = std::ranges::find(Combat.AmmoClasses, Stock ? ToLowerName(Stock->Name) : Weapon.Ammo, &FAmmoClass::Name);
    if (Stock && Stock->Count != 0.f && Ammo != Combat.AmmoClasses.end())
    {
        List(AmmoLines[0], Format("AMMO        %s", Upper(Ammo->Name)));
        List(AmmoLines[1], Format("DAMAGE      %d", Ammo->ListedDamage));
        List(AmmoLines[2], Format("DAMAGE-AREA %d", Ammo->ListedArea));
    }
}

void StepPlayerWeapon(FCombat& Combat, const bool bFire, FPlayerState& Player, const FLevelObject& PlayerObject, const size_t PlayerIndex,
    const float SpriteSize, FPlayerInventory& Inventory, FMessages& Messages)
{
    // EngineUpdateObjects for the player, before its weapon: Triple Fire opens the spread while the trigger is held and closes it otherwise (FUN_00443b80),
    // and Circular Fire makes the next 24 shots turn the player one direction each. A volley under either takes one round.
    FSurpriseState& Surprises = Inventory.Surprises;
    if (Surprises.bTripleFire && bFire ? Surprises.TripleSpread == 0 : Surprises.TripleSpread != 0)
    {
        constexpr int32_t SpreadOpening = 100;
        constexpr int32_t SpreadOpenStep = 20;
        constexpr int32_t SpreadCloseStep = -50;
        if (Surprises.TripleSpread != 0)
        {
            Surprises.TripleSpreadStep = SpreadCloseStep;
        }
        else
        {
            Surprises.TripleSpread = SpreadOpening;
            Surprises.TripleSpreadStep = SpreadOpenStep;
        }
    }

    constexpr int32_t CircularShots = 24;
    if (Surprises.bCircularFire && bFire && !Player.bInvisible && Surprises.CircularShots == 0)
    {
        Surprises.CircularShots = CircularShots;
    }

    Combat.bOneRoundPerVolley = Surprises.bTripleFire || Surprises.bCircularFire;

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
    const FAmmoStock* const BoundStock = SlotAmmo(Combat, Combat.PlayerSlot, Inventory);
    const auto AmmoClass = std::ranges::find(Combat.AmmoClasses, BoundStock ? ToLowerName(BoundStock->Name) : Weapon.Ammo, &FAmmoClass::Name);
    if (Player.bInvisible && (AmmoClass == Combat.AmmoClasses.end() || !AmmoClass->bUsableWhenInvisible))
    {
        constexpr int32_t NoteX = 100;
        constexpr int32_t NoteY = 0x6e;
        constexpr uint32_t NoteLifeMilliseconds = 2000;
        constexpr uint32_t PanelScreenFont = 3;
        if (!Combat.Details.bGreenInfo)
        {
            return;
        }

        if (Messages.Exists(NoteX, NoteY))
        {
            Messages.SetLife(NoteX, NoteY, NoteLifeMilliseconds);
            return;
        }

        Messages.Add({
            .Text = std::string(Localize("NOTE : ONLY MINES CAN BE USED WHILE INVISIBLE")),
            .X = float(NoteX),
            .Y = float(NoteY),
            .LifeMilliseconds = NoteLifeMilliseconds,
            .bTypewriter = true,
            .Gaf = EMessageGaf::Panel,
            .Font = PanelScreenFont,
        }, EMessagePlacement::Replace);
        return;
    }

    FAmmoStock* const Stock = BoundAmmo(Weapon, Inventory);
    const FShooter Shooter{
        .Position = Player.Position,
        .Velocity = Player.Velocity,
        .Direction = Player.Direction,
        .Altitude = int(PlayerObject.Altitude),
        .SpriteSize = SpriteSize,
        .ObjectIndex = PlayerIndex,
        .Spread = Surprises.TripleSpread,
    };
    const float RoundsBefore = Stock ? Stock->Count : 0.f;
    // Each shot under Circular Fire turns the player to the next direction of its ring and its pose two on, without its target pose.
    if (const EWeaponFire Result = FireWeapon(Combat, State, Shooter, Stock); Result == EWeaponFire::Failed)
    {
        Surprises.CircularShots = 0;
    }
    else if (Result == EWeaponFire::Fired && Surprises.CircularShots != 0)
    {
        --Surprises.CircularShots;
        Player.Direction = Wrap(Player.Direction + 1);
        Player.Pose = Player.Pose + 2 > PlayerPoseCount ? 1 : Player.Pose + 2;
    }
    // WeaponFire calls WeaponHasAmmo again once a shot leaves the stock empty, so the next weapon is in hand at once.
    if (Stock && RoundsBefore >= 1.f && Stock->Count < 1.f)
    {
        SelectPlayerWeapon(Combat, NextPlayerWeaponSlot(Combat, Inventory), Inventory, Messages);
    }
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

// SeekerTurnBody at 0042cfa0: one pose toward the segment per turn timeout (object +0xec).
// A body that cannot turn on the spot, such as a jeep or a tank, turns only while moving at 1 or more (DAT_0050950c); with a class Turn-timeout its timeout becomes
// (1 - speed / Speed-max) times it, quicker the faster it goes, and the time already waited is cut to that.
// A two-piece body holds still while its top aims, and its top turns along with it.
void TurnActorBody(FPlayerState& State, FMovementClass& Class, FActor& Actor, const uint32_t Range, const int Direction, const int32_t Step)
{
    constexpr uint32_t Firing = 4;
    if (Actor.bTwoPieces && (Range & Firing) != 0)
    {
        return;
    }

    if (!Actor.bTurnOnPlace)
    {
        constexpr float TurnSpeedMin = 1.f;
        if (State.SpeedMagnitude < TurnSpeedMin)
        {
            return;
        }

        if (Actor.ClassTurnTimeout != 0)
        {
            Class.TurnTimeout = TruncateLow((1. - double(State.SpeedMagnitude) / double(Class.SpeedMax)) * double(Actor.ClassTurnTimeout));
            State.TurnTimer = std::min(State.TurnTimer, Class.TurnTimeout);
        }
    }

    const int PosesPerStep = Actor.PosesPerDirection;
    const int PoseCount = DirectionCount * PosesPerStep;
    // A spider stops at the nearest of the four axis directions.
    constexpr int SpiderStep = DirectionCount / 4;
    const int Target = Actor.bSpider ? (Direction + SpiderStep / 2) / SpiderStep * SpiderStep % DirectionCount : Direction;
    Actor.bTurnCommand = Actor.bTurnCommand || Target * PosesPerStep + 1 != State.Pose;
    State.TurnTimer -= Step;
    if (State.TurnTimer >= 1)
    {
        return;
    }

    State.TurnTimer = std::max(0, State.TurnTimer + Class.TurnTimeout);
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

// FixedIdleLookAround at 00444a40, which EngineUpdateObjects (004406e0) runs for a seeker or tracker that can turn on the spot and stands still without a turn or thrust command.
// Waiting at its pose, it draws RandomRange(1, 150) from the Misc stream, and a 75 picks a new target pose from 1 to 24, whatever its pose count; then it turns one pose toward it per Turn-timeout.
// ponytail: a two-piece body (FUN_00444b80) looks around with its top instead; tanks have no Can-turn-on-place, so it is left out.
void LookAround(FObjectBody& Body, FRandom& Misc, const int32_t Step)
{
    const FActor& Actor = *Body.Actor;
    FPlayerState& State = Body.State;
    const uint32_t Target = Actor.Behaviour & BehaviourTargetMask;
    const bool bIdle = Actor.Command.Thrust == EPlayerThrust::None && !Actor.bTurnCommand && State.Position.X == State.PreviousPosition.X &&
        State.Position.Y == State.PreviousPosition.Y;
    if (!Actor.bTurnOnPlace || Actor.bTwoPieces || (Target != BehaviourSeeker && Target != BehaviourTracker) || !bIdle || Body.Class.TurnTimeout <= 0)
    {
        return;
    }

    constexpr int LookChanceRange = 150;
    constexpr int LookChance = 75;
    constexpr int LookPoseCount = 24;
    const int PoseCount = DirectionCount * Actor.PosesPerDirection;
    if (State.Pose == State.TargetPose && Misc.Range(1, LookChanceRange) == LookChance)
    {
        State.TargetPose = Misc.Range(0, LookPoseCount - 1) + 1;
    }

    if (State.TurnTimer < 0)
    {
        State.Pose = AdvancePose(State.Pose, State.TargetPose, PoseCount);
    }

    // PlayerTurn at 00443990 with the turn FUN_00436520 derives from the target pose: one pose per Turn-timeout until it is reached.
    State.TurnTimer -= Step;
    if (State.TurnTimer < 1)
    {
        do
        {
            State.TurnTimer += Body.Class.TurnTimeout;
            if (State.Pose != State.TargetPose)
            {
                State.Pose = AdvancePose(State.Pose, State.TargetPose, PoseCount);
                if (State.Pose == State.TargetPose || State.TurnTimer >= 0)
                {
                    break;
                }
            }
        } while (State.TurnTimer < 0);
    }

    State.Direction = (State.Pose - 1) / Actor.PosesPerDirection;
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

// SeekerAimAndFire at 0042d2d0 for a body without a turret top: inside the weapon's Fire-Range, facing the target and not turning, the trigger is pulled once the weapon's Delay has passed.
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

// SeekerEngage at 0042caa0: turn toward the segment and drive along it; within Min-distance of the target, face it, back off when too close and fire.
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
    const bool bFiring = AimAndFire(*Combat, Body, Actor, *Seeker, *Target);
    if (Body.bVisibleWhenFiring)
    {
        Body.bInvisible = !bFiring;
    }

    if (bFiring)
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

}

void PushGamma(std::array<int32_t, 3>& Gamma, const int32_t Amount, const int32_t Limit)
{
    for (int32_t& Channel : Gamma)
    {
        Channel = std::clamp(Channel + Amount, -Limit, Limit);
    }
}

void StepDeathDrift(FDeathDrift& Drift, FPlayerState& Player, const FLevel& Level, const uint32_t StepMilliseconds)
{
    constexpr int32_t TurnIntervalStep = 30;
    constexpr float DriftSpeed = 3.f;
    const float X = Player.Position.X;
    const float Y = Player.Position.Y;
    const bool bOutside = X < 0.f || Y < 0.f || float(Level.WorldExtentX) <= X || float(Level.WorldExtentY) <= Y;
    if (!bOutside && TruncateLow(X) % Drift.TurnInterval != 0 && TruncateLow(Y) % Drift.TurnInterval != 0)
    {
        Player.Position = { .X = X + Drift.VelocityX, .Y = Y + Drift.VelocityY };
        Player.Center = CollisionFromWorld(Player.Position);
        return;
    }

    // The turn itself does not move; the speed is 3 per 14 ms step (session +0x2744 is 14 / step).
    const float Speed = DriftSpeed / float(ReferenceStepMilliseconds / double(StepMilliseconds));
    Drift.TurnInterval += TurnIntervalStep;
    if (Drift.VelocityX > 0.f)
    {
        Drift = { .VelocityX = 0.f, .VelocityY = -Speed, .TurnInterval = Drift.TurnInterval };
    }
    else if (Drift.VelocityX < 0.f)
    {
        Drift = { .VelocityX = 0.f, .VelocityY = Speed, .TurnInterval = Drift.TurnInterval };
    }
    else if (Drift.VelocityY > 0.f)
    {
        Drift = { .VelocityX = Speed, .VelocityY = 0.f, .TurnInterval = Drift.TurnInterval };
    }
    else if (Drift.VelocityY < 0.f)
    {
        Drift = { .VelocityX = -Speed, .VelocityY = 0.f, .TurnInterval = Drift.TurnInterval };
    }
}

namespace
{
// SeekerFindTarget at 0042da80 for the player: alive, within Sensor-Range and within the vision cone around the body's facing.
// ponytail: the grid-cell window around the seeker is replaced by the distance test it ends in; a player outside the window but in range is still found.
// A guardian searches its Guardian Range in every direction instead.
bool FindTarget(const FObjectBody& Body, const FSeeker& Seeker, const FPlayerState& Player)
{
    if (Player.bInvisible || Player.bDying)
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
            if (Combat && Target)
            {
                Combat->bPlayerLocked = true;
            }

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

// FUN_0042e040: a destination drawn at random from open ground in the window around the actor, retrying the last one once if its route did not reach its last segment. ponytail: the multiplayer reseed of the generator (session flags 0x18000) and the object check inside FUN_0042c130 are not ported.
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
    const FLevel& Level, const FPlayerTarget* const Target)
{
    const FPlayerState* const Player = Target ? &Target->State : nullptr;
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
        Actor.bTurnCommand = false;
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

        if (Combat)
        {
            LookAround(Body, Combat->MiscRandom, Step);
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

        std::optional<FPlayerKnock> Knock;
        if (Target)
        {
            Knock = FPlayerKnock{ .Circle = { .Center = Target->State.Center, .Radius = Target->Radius }, .bTouchAndDestroy = Target->State.bTouchAndDestroy };
        }

        StepPlayer(Body.State, Body.Class, Actor.Command, StepMilliseconds, Terrain, Level, Bodies, &Body, Knock ? &*Knock : nullptr);
        // ApplyDamage spares an invisible player and an invulnerable one, as for shots.
        if (Knock && Knock->Damage != 0.f && !Target->State.bInvisible && !Target->Inventory.Surprises.bInvulnerable)
        {
            ApplyDamage(Target->Inventory, Knock->Damage);
        }
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
                if (bInRange && bInView && !Player.bInvisible && !Player.bDying && Turret.Search.Start(Body.State.Center, Player.Center))
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
            Combat.bPlayerLocked = true;
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
                    // SeekerEngage shows the turret while SeekerAimAndFire has it aimed in range, whether or not the weapon is ready.
                    if (Body.bVisibleWhenFiring)
                    {
                        Body.bInvisible = Direction != Turret.Direction;
                    }
                }
                else
                {
                    bEngaged = false;
                    Body.bInvisible = Body.bInvisible || Body.bVisibleWhenFiring;
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

// ObjectDropContents at 00437f80: a destroyed object leaves its class's HAVE objects, then each RANDOM one whose draw RandomRange(1, Out-of) does not exceed its Chance.
// The class loader prepends both lists, so the last declared, and the derived class's before its parent's, come first.
// The first object lands on the wreck and the others around it, alternately left and right and then in rows 10 pixels apart. ponytail: objects carried at object +0x11c are not dropped, and a demo recording or replay would skip the RANDOM draws.
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

namespace
{
// ObjectHuntsPlayer at 004371b0: a seeker or tracker hunting the player, or a generator of one.
bool HuntsPlayer(const FObjectBody& Body)
{
    return Body.Turret || (Body.Actor && Body.Actor->Seeker) || (Body.Generator && Body.Generator->bProductHunts);
}
}

bool IsAutoAimTarget(const FObjectBody& Body, const FLevel& Level)
{
    return HuntsPlayer(Body) && 0.f < Body.Energy && !Body.bInvisible && !Level.Objects[Body.ObjectIndex].bRemoved && !Level.Objects[Body.ObjectIndex].bWrecked;
}

namespace
{
// The FX part of ObjectUpdateWeapon (00444ca0) for each orbiter. Its on and off timers switch the check-event on the host (FUN_004452c0) and take turns;
// it goes once its host is gone or the check-event no longer holds (FUN_0041c0b0), or waits hidden for its next on time when its class has one.
// A circular one stands Ray away from the host at clock * Angular-speed + Phase, through the collision-to-world scales, and bobs with the host's altitude.
// ponytail: Only-when-visible (FX flag 4) is not traced; no reader of it was found.
void StepOrbiters(FCombat& Combat, const FLevel& Level, const std::span<FObjectBody> Bodies, const FPlayerTarget& Player, const uint32_t StepMilliseconds)
{
    const int32_t Step = int32_t(StepMilliseconds);
    std::erase_if(Combat.Orbiters, [&](FOrbiter& Orbiter)
    {
        const FEffectClass& Class = Combat.EffectClasses[Orbiter.Effect.ClassIndex];
        FObjectBody* const HostBody = Orbiter.Host ? [&]() -> FObjectBody*
        {
            const auto Found = std::ranges::find(Bodies, *Orbiter.Host, &FObjectBody::ObjectIndex);
            return Found != Bodies.end() && !Level.Objects[*Orbiter.Host].bRemoved ? &*Found : nullptr;
        }()
                                                   : nullptr;
        if (Orbiter.Host && !HostBody)
        {
            return true;
        }

        const auto Switch = [&](const bool bOn)
        {
            if (HostBody && Class.Event == EFxEvent::Invulnerability)
            {
                HostBody->bInvulnerable = bOn;
            }
        };
        if (0 < Orbiter.OnTimer)
        {
            Switch(true);
            if ((Orbiter.OnTimer -= Step) <= 0)
            {
                Orbiter.OffTimer = Class.OffTimeout;
                Switch(false);
            }
        }
        else if (0 < Orbiter.OffTimer)
        {
            Switch(false);
            if ((Orbiter.OffTimer -= Step) <= 0)
            {
                Orbiter.OnTimer = Class.OnTimeout;
                Orbiter.bHidden = false;
                Switch(true);
            }
        }

        const FSurpriseState& Surprises = Player.Inventory.Surprises;
        bool bHolds = false;
        switch (Class.Event)
        {
        case EFxEvent::Invulnerability:
            bHolds = HostBody ? HostBody->bInvulnerable : Surprises.bInvulnerable;
            break;
        case EFxEvent::TouchAndDestroy:
            bHolds = !HostBody && Player.State.bTouchAndDestroy;
            break;
        case EFxEvent::Banana:
            bHolds = !HostBody && Surprises.Active == ESurprise::Banana;
            break;
        case EFxEvent::Always:
            bHolds = true;
            break;
        default:
            break;
        }

        if (!bHolds)
        {
            if (Class.OnTimeout == 0)
            {
                return true;
            }

            Orbiter.bHidden = true;
            return false;
        }

        AdvanceEffect(Orbiter.Effect, Class, StepMilliseconds, true);
        Orbiter.Age += StepMilliseconds;
        RunCreates(Combat, Level, Class.Creates, Orbiter.NextCreates, Orbiter.Age, Orbiter.Effect.Position, Orbiter.Effect.Altitude, Combat.Trails);

        if (!Class.bCircular)
        {
            return false;
        }

        const FCollisionPoint HostPosition = HostBody ? HostBody->State.Position : Player.State.Position;
        const int HostAltitude = int(HostBody ? HostBody->Altitude : Level.Objects[Player.ObjectIndex].Altitude);
        const double Clock = double(Combat.Clock);
        const double Angle = Clock * double(Class.AngularSpeed) + double(Orbiter.Phase);
        const float Sine = float(std::sin(Angle) * double(Class.Ray));
        const double Cosine = std::cos(Angle) * double(Class.Ray);
        constexpr double WorldXScale = double(0.8944272f);
        constexpr double WorldYScale = double(0.4472136f);
        const float OffsetX = float((double(Sine) - Cosine) * WorldXScale);
        const double OffsetY = (Cosine + double(Sine)) * WorldYScale;
        if (Class.VerticalAmplitude != 0.f)
        {
            const double Bob = std::sin(double(float(Clock)) * double(Class.VerticalPulsation) + double(Class.VerticalPhase)) * double(Class.VerticalAmplitude);
            Orbiter.Effect.Altitude = float(int(Bob + double(HostAltitude)));
        }

        // An orbit that would leave the level keeps its last place.
        const FCollisionPoint Next{ .X = float(double(OffsetX) + double(HostPosition.X)), .Y = float(OffsetY + double(HostPosition.Y)) };
        if (0.f <= Next.X && Next.X < float(Level.WorldExtentX) && 0.f <= Next.Y && Next.Y < float(Level.WorldExtentY))
        {
            Orbiter.Effect.Position = Next;
        }

        return false;
    });
}
}

void StepProjectiles(FCombat& Combat, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerTarget& Player, FMessages& Messages)
{
    StepOrbiters(Combat, Level, Bodies, Player, StepMilliseconds);
    const float TimeScale = float(ReferenceStepMilliseconds / double(StepMilliseconds));
    constexpr FPlayerCommand Straight{ .Thrust = EPlayerThrust::Forward };
    // The trails age first, so the ones made this step start at zero. Flying debris moves as any launched object does without a command (object flag 0x8000),
    // leaves its own trails, and goes once it leaves the level.
    std::vector<FTrail> Left;
    std::erase_if(Combat.Trails, [&](FTrail& Trail)
    {
        Trail.Age += StepMilliseconds;
        if (Trail.Age >= Trail.LifeTime)
        {
            return true;
        }

        if (!Trail.Flight)
        {
            return false;
        }

        FTrail::FFlight& Flight = *Trail.Flight;
        RunCreates(Combat, Level, Flight.Creates, Flight.NextCreates, Trail.Age, Trail.Position, Trail.Altitude, Left);
        FPlayerState& State = Flight.State;
        const FCollisionPoint Delta = Displacement(State, Flight.Class, {}, TimeScale);
        const FCollisionPoint Candidate{ .X = float(double(Delta.X) + double(State.Center.X)), .Y = float(double(Delta.Y) + double(State.Center.Y)) };
        State.PreviousPosition = State.Position;
        State.Position = WorldFromCollision(Candidate);
        State.Velocity = { .X = float(double(TimeScale) * Delta.X), .Y = float(double(TimeScale) * Delta.Y) };
        State.Center = Candidate;
        Trail.Position = State.Position;
        constexpr float LowerLimit = 1.f;
        return State.Position.X < LowerLimit || float(Level.WorldExtentX) <= State.Position.X || State.Position.Y < LowerLimit || float(Level.WorldExtentY) <= State.Position.Y;
    });
    Combat.Trails.insert(Combat.Trails.end(), std::make_move_iterator(Left.begin()), std::make_move_iterator(Left.end()));

    // The mines' part of EngineUpdateObjects (004406e0). While the off timer runs a mine is left out of its update; when it runs out ObjectShow (00439140) shows the mine and makes it solid,
    // and the on timer is drawn; when that one runs out ObjectHide (004391d0) hides it, solid only with Keep-bb-when-invisible, and the off timer is drawn.
    // Each draw reseeds the generator with the object's own seed (object +0x2a8 and +0x2ac), the level seed CreateObject gives every object, and puts the shared one back, so every draw is the same.
    for (FObjectBody& Body : Bodies)
    {
        if (!Body.Mine || Level.Objects[Body.ObjectIndex].bRemoved)
        {
            continue;
        }

        FMine& Mine = *Body.Mine;
        if (Mine.Owner == Player.ObjectIndex)
        {
            Player.Inventory.ShotsHit += Mine.Hits;
            Player.Inventory.ShotsMissed += Mine.Misses;
        }

        Mine.Hits = 0;
        Mine.Misses = 0;
        if (Mine.bStruck)
        {
            continue;
        }

        const int32_t Step = int32_t(StepMilliseconds);
        if (Mine.OffTimer != 0)
        {
            if (Mine.OffTimer < 0 || (Mine.OffTimer -= Step) > 0)
            {
                continue;
            }

            Mine.OnTimer = 0;
            Mine.OffTimer = 0;
            Body.bInvisible = false;
            Body.bPassable = false;
            if (Mine.OnMax != 0)
            {
                Mine.OnTimer = FRandom{}.Range(Mine.OnMin, Mine.OnMax);
            }
        }

        if (0 < Mine.OnTimer && (Mine.OnTimer -= Step) < 0)
        {
            Mine.OnTimer = 0;
            Mine.OffTimer = -1;
            Body.bInvisible = true;
            Body.bPassable = !Mine.bKeepBoxHidden;
            if (Mine.OffMax != 0)
            {
                Mine.OffTimer = FRandom{}.Range(Mine.OffMin, Mine.OffMax);
            }

            continue;
        }

        // Damage-after sets it off where it lies, through ObjectDestroy.
        if (Mine.DamageTimer != 0)
        {
            if (Mine.DamageTimer <= StepMilliseconds)
            {
                Body.bDamaged = true;
                Body.Energy = 0.f;
            }
            else
            {
                Mine.DamageTimer -= StepMilliseconds;
            }
        }
    }

    std::erase_if(Combat.Projectiles, [&](FProjectile& Shot)
    {
        Shot.Age += StepMilliseconds;
        const FAmmoClass& Ammo = Combat.AmmoClasses[Shot.AmmoIndex];
        RunCreates(Combat, Level, Ammo.Creates, Shot.NextCreates, Shot.Age, Shot.State.Position, float(Shot.Altitude), Combat.Trails);

        // Target is the object struck, bPlayer the player struck; bSurface marks a wall or other terrain contact.
        const auto Detonate = [&](FObjectBody* const Target, const bool bPlayer, const bool bSurface)
        {
            // AreaDamage and AmmoHit add every object a player's shot finishes off to the player's kills. Only AmmoHit, on a contact, counts the shot as a hit or a miss.
            std::vector<bool> WasAlive;
            if (Shot.Owner == Player.ObjectIndex)
            {
                if (Target || bPlayer || bSurface)
                {
                    const bool bHit = Target && (HuntsPlayer(*Target) || Level.Objects[Target->ObjectIndex].SubType == "item");
                    ++(bHit ? Player.Inventory.ShotsHit : Player.Inventory.ShotsMissed);
                }

                std::ranges::transform(Bodies, std::back_inserter(WasAlive), [](const FObjectBody& Body) { return 0.f < Body.Energy; });
            }

            // AmmoHit at 0043d7d0 blasts around the shot's last committed centre, then AmmoImpact (0043d350) shoves and damages what it struck.
            const bool bPlayerShot = Shot.Owner == Player.ObjectIndex;
            AreaDamage(Combat, Shot.State.Center, CellOf(Shot.State.Position), Ammo.Damage, Ammo.DamageZone, Bodies, Player, bPlayerShot);
            // AreaDamage opens a 500 ms window (session +0x2748) on the first hunter a player's blast finishes off and counts each one after it.
            constexpr int32_t FastKillWindowMilliseconds = 0x1f4;
            for (size_t Index = 0; Index < WasAlive.size(); ++Index)
            {
                if (WasAlive[Index] && Bodies[Index].Energy <= 0.f && HuntsPlayer(Bodies[Index]))
                {
                    if (Combat.FastKillTimer < 1)
                    {
                        Combat.FastKillCount = 1;
                        Combat.FastKillTimer = FastKillWindowMilliseconds;
                    }
                    else
                    {
                        ++Combat.FastKillCount;
                    }
                }
            }
            if (Target)
            {
                constexpr double ImpactPush = 40.;
                const FCollisionPoint Facing = FacingVector(Shot.State.Direction);
                Target->State.Velocity.X = float(double(Facing.X) * ImpactPush / double(Target->Class.Weight) + double(Target->State.Velocity.X));
                Target->State.Velocity.Y = float(double(Facing.Y) * ImpactPush / double(Target->Class.Weight) + double(Target->State.Velocity.Y));
                // AmmoImpact hurts a Player-direct-fire-only object only with the player's own shots.
                const auto Absorption = std::ranges::find(Target->Absorptions, Ammo.Name, &FAbsorption::AmmoClass);
                if (!Target->bPlayerDirectFireOnly || bPlayerShot)
                {
                    ApplyDamage(*Target, Absorption == Target->Absorptions.end() ? Ammo.Damage : float(double(Ammo.Damage) * double(Absorption->Coeff)));
                }
            }

            // The player is shoved and its absorptions scale the damage like any object's.
            if (bPlayer)
            {
                constexpr double ImpactPush = 40.;
                const FCollisionPoint Facing = FacingVector(Shot.State.Direction);
                Player.Inventory.Shove.X = float(double(Facing.X) * ImpactPush / double(Player.Inventory.Weight) + double(Player.Inventory.Shove.X));
                Player.Inventory.Shove.Y = float(double(Facing.Y) * ImpactPush / double(Player.Inventory.Weight) + double(Player.Inventory.Shove.Y));
                // ApplyDamage spares an invisible player and an invulnerable object (flags 8 and 0x800000).
                if (!Player.State.bInvisible && !Player.Inventory.Surprises.bInvulnerable)
                {
                    const auto Absorption = std::ranges::find(Player.Inventory.Absorptions, Ammo.Name, &FAbsorption::AmmoClass);
                    ApplyDamage(Player.Inventory, Absorption == Player.Inventory.Absorptions.end() ? Ammo.Damage : float(double(Ammo.Damage) * double(Absorption->Coeff)));
                }
            }

            const int32_t KillsBefore = Player.Inventory.Kills;
            for (size_t Index = 0; Index < WasAlive.size(); ++Index)
            {
                Player.Inventory.Kills += WasAlive[Index] && Bodies[Index].Energy <= 0.f ? 1 : 0;
            }

            // VoiceKillCheer (0043d070) cheers a kill seven times in sixteen by the game clock.
            constexpr std::array KillVoices = { EVoice::Yeah, EVoice::Hey, EVoice::Yo, EVoice::GiveMe, EVoice::TheBest, EVoice::WantMore, EVoice::Laugh };
            constexpr uint32_t KillVoiceMask = 0xf;
            if (const uint32_t Pick = GetVoiceClock() & KillVoiceMask; Player.Inventory.Kills != KillsBefore && Pick < KillVoices.size())
            {
                constexpr uint32_t KillVoiceDelay = 1000;
                constexpr uint32_t KillVoiceDuration = 1500;
                QueueVoice(KillVoices[Pick], KillVoiceDelay, KillVoiceDuration, 0);
            }

            // AmmoImpact (0043d350) puts the Hit-fx where the shot struck; a shot that sank to the ground leaves a Water-Splash on open water (its cell's terrain marker 0x100, FUN_0042c130)
            // and a Bullet-Ground-Impact elsewhere. Either goes at the spot clamped into the level, only near the view (FUN_00436690), and a Uniq-at-xy class not where one of its own stands.
            const bool bHit = Target || bPlayer || bSurface;
            if (bHit || Shot.Altitude < 1)
            {
                const FWorldCell Cell = CellOf(Shot.State.Position);
                const std::string_view Name = bHit ? std::string_view(Ammo.HitEffect) : Terrain.IsWater(Cell.X, Cell.Y) ? WaterSplashEffect : GroundImpactEffect;
                const FCollisionPoint Spot{
                    .X = std::clamp(Shot.State.Position.X, 0.f, float(Level.WorldExtentX) - 1.f),
                    .Y = std::clamp(Shot.State.Position.Y, 0.f, float(Level.WorldExtentY) - 1.f),
                };
                const auto Class = std::ranges::find(Combat.EffectClasses, Name, &FEffectClass::Name);
                const bool bTaken = Class != Combat.EffectClasses.end() && Class->bUniqAtXy &&
                    std::ranges::any_of(Combat.Effects, [&](const FEffect& Effect)
                    {
                        return Effect.ClassIndex == size_t(Class - Combat.EffectClasses.begin()) && !Effect.bFinished && Effect.Position.X == Spot.X && Effect.Position.Y == Spot.Y;
                    });
                if (!bTaken && IsNearView(Spot, Player.State.Position))
                {
                    SpawnEffect(Combat, Name, Spot, bHit ? float(Shot.Altitude) : 0.f);
                }
            }

            // Then ObjectDestroy (00437d40) hands each of the class's TYPE Explosion blocks to a Boom-fx, as it does for any object.
            for (const FExplosionBlock& Explosion : Ammo.Explosions)
            {
                SpawnEffect(Combat, Explosion.BoomEffect, Shot.State.Position, float(Shot.Altitude), Explosion.Damage, Explosion.Zone);
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

        // EngineUpdateObjects (004406e0) puts bouncing ammo that met the terrain back and bounces it (ObjectBounce at 004367d0), then turns it to its new heading.
        // A hard knock (more than 2 units of travel) takes a tenth of its speed times its weight from its energy, and an emptied shot bursts where it is.
        // Before the knock's damage, the hard knock throws off the class's bounce spark (FUN_0043d140).
        // ponytail: ObjectAdjustImpactPoint is left out, as for the player's bounce.
        if (Ammo.bBouncesOffTerrain && !Struck && !bPlayerStruck)
        {
            Shot.State.Position = Shot.State.PreviousPosition;
            constexpr double KnockDamageScale = 0.1f;
            const float Knock = Shot.State.SpeedMagnitude * Ammo.Movement.Weight;
            const bool bHardKnock = IsHardKnock(Shot.State, Ammo.Movement, Delta);
            if (bHardKnock)
            {
                ThrowBounceSpark(Combat, Ammo.BounceEffects, Shot.State.Position, WorldFromCollision(*Contact), Player.State.Position);
            }

            if (!Ammo.bIndestructible && bHardKnock)
            {
                const float Damage = float(double(Knock) * KnockDamageScale);
                Shot.Energy = Shot.Energy <= Damage ? 0.f : Shot.Energy - Damage;
                if (Shot.Energy <= 0.f)
                {
                    return Detonate(nullptr, false, false);
                }
            }

            Bounce(Shot.State, Ammo.Movement, Straight, *Contact);
            ++Shot.Bounces;
            if (const int Heading = FindDirection(Shot.State.Velocity); Heading != NoDirection)
            {
                Shot.State.Direction = Heading;
            }

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
            if (Effect.Frame < uint32_t(Class.Animation.Frames.size()) / 2)
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
                AreaDamage(Combat, CollisionFromWorld(Effect.Position), CellOf(Effect.Position), Effect.Damage, Effect.Zone, Bodies, Player, false);
                Effect.Damage = 0.f;
            }
        }

        Effect.Age += StepMilliseconds;
        RunCreates(Combat, Level, Class.Creates, Effect.NextCreates, Effect.Age, Effect.Position, Effect.Altitude, Combat.Trails);
        AdvanceEffect(Effect, Class, StepMilliseconds);
        return Effect.bFinished;
    });

    // ApplyDamage (0043ca30) reports a Final-object's death and its callers run FUN_0043db70: DestroyAround (0043d980) from the map's centre over the whole level takes every destructible object's energy and draws its shake, which is then stopped, and the finale starts.
    // ponytail: the original reacts at the damage itself, the port at the next destruction pass.
    if (!Combat.bFinale && std::ranges::any_of(Bodies, [](const FObjectBody& Body) { return Body.bFinalObject && Body.bDamaged && Body.Energy <= 0.f; }))
    {
        Combat.bFinale = true;
        for (FObjectBody& Body : Bodies)
        {
            if (!Body.bIndestructible && !Level.Objects[Body.ObjectIndex].bRemoved)
            {
                Body.Energy = 0.f;
                Body.bDamaged = true;
            }
        }

        constexpr int ShakeRange = 0x20;
        Combat.Random.Range(-ShakeRange, ShakeRange);
        Combat.Random.Range(-ShakeRange, ShakeRange);
        Player.Inventory.Surprises.ShakeX = 0;
        Player.Inventory.Surprises.ShakeY = 0;
    }

    // Session +0x2094 counts the hunters down as they die; the last one announces the empty level.
    int Hunters = int(std::ranges::count_if(Bodies, [&](const FObjectBody& Body) { return HuntsPlayer(Body) && !Level.Objects[Body.ObjectIndex].bRemoved; }));
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
        // A Two-pieces object's first death keeps it as a wreck (ObjectDestroy with its keep flag): it explodes and pays its reward, but drops nothing and still counts as an enemy.
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

        if (!bFirstOfTwo && HuntsPlayer(Body) && --Hunters == 0)
        {
            // The player's name at session +0x2754 starts as Jack.
            constexpr int32_t LastKillX = 0x46;
            constexpr int32_t LastKillY = (MessageViewHeight >> 1) - 0x66;
            constexpr uint32_t LastKillLifeMilliseconds = 10000;
            constexpr int RealKillerBonus = 100000;
            constexpr uint32_t Font = 6;
            // A deliberate fix: EngineUpdateObjects (004408ab) only prints the bonus and never pays it; the port pays it as the Fast Kill Bonus is paid.
            Player.Inventory.Money += float(RealKillerBonus);
            Player.Inventory.Earned += float(RealKillerBonus);
            Messages.Add({
                .Text = Format("You are so professional %s !\nNo more enemy in this level !\n  Real Killer Bonus : +%d $", "Jack", RealKillerBonus),
                .X = float(LastKillX),
                .Y = float(LastKillY),
                .LifeMilliseconds = LastKillLifeMilliseconds,
                .Font = Font,
            }, EMessagePlacement::Replace);
        }

        // ObjectDestroy (00437d40) hands each TYPE Explosion block to a new Boom-fx object at the same spot.
        for (const FExplosionBlock& Explosion : Body.Explosions)
        {
            SpawnEffect(Combat, Explosion.BoomEffect, Body.State.Position, Body.Altitude, Explosion.Damage, Explosion.Zone);
        }

        if (0.f < Body.Reward && PaysReward(Object.SubType))
        {
            Player.Inventory.Money += Body.Reward;
            Player.Inventory.Earned += Body.Reward;
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
            // The wreck starts over with the class's initial energy and shield, and its shield no longer builds up.
            const auto Initial = [&](const std::string_view Kind)
            {
                const FLevelPropertyBlock* const Block = FindBlock(Object.Properties, Kind);
                return Block ? ParseLevelFloat(Block->Find("Initial")).value_or(0.f) : 0.f;
            };
            Object.bWrecked = true;
            Body.Energy = Initial("Energy");
            Body.Shield = Initial("Shield");
            Body.ShieldUpStep = 0.f;
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

    // WeaponFire (00443bd0) makes a mine an object facing the shooter's way; with an active-after ObjectHide (004391d0) leaves it shown but passable, and its off timer runs that long.
    for (const FCombat::FDroppedMine& Dropped : Combat.DroppedMines)
    {
        const FAmmoClass& Ammo = Combat.AmmoClasses[Dropped.AmmoIndex];
        const std::optional<size_t> Index = CreateLevelObject(Level, Ammo.Name, Dropped.Position.X, Dropped.Position.Y);
        if (!Index)
        {
            continue;
        }

        Level.Objects[*Index].Direction = Dropped.Direction;
        std::optional<FObjectBody> Body = CreateObjectBody(Level, *Index);
        if (Body && Body->Mine)
        {
            Body->Mine->Owner = Dropped.Owner;
            if (Ammo.ActiveAfter != 0)
            {
                Body->bPassable = true;
                Body->Mine->OnTimer = 0;
                Body->Mine->OffTimer = Ammo.ActiveAfter;
            }

            Bodies.push_back(std::move(*Body));
        }

        Combat.CreatedObjects.push_back(*Index);
    }

    Combat.DroppedMines.clear();
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

namespace
{
// UNDERGROUND\T2U1's minefield: the player drives into a placed mine and sets it off, and a dropped Mine-Ammo lies passable until it arms, blinks, still collides while hidden and goes off by itself.
std::expected<std::string, std::string> VerifyMines(FResourceArchive& Archive)
{
    constexpr uint32_t StepMilliseconds = 14;
    FMessages Messages;
    std::expected<FLevel, std::string> Level = LoadLevel(Archive, "underground\\t2u1");
    const std::optional<size_t> PlayerIndex = Level ? FindPlayerObject(*Level) : std::nullopt;
    if (!PlayerIndex)
    {
        return std::unexpected(Level ? std::string("no player") : Level.error());
    }

    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const std::expected<FMovementClass, std::string> Class = ReadMovementClass(Level->Objects[*PlayerIndex]);
    const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
    std::expected<FCombat, std::string> Combat = CreateCombat(*Level, Level->Objects[*PlayerIndex], Archive, Bodies);
    if (!Class || !Terrain || !Combat)
    {
        return std::unexpected(!Class ? Class.error() : !Terrain ? Terrain.error() : Combat.error());
    }

    FPlayerInventory Inventory = CreateInventory(Level->Objects[*PlayerIndex]);
    const auto Placed = std::ranges::find_if(Bodies, [](const FObjectBody& Body) { return Body.Mine.has_value(); });
    if (Placed == Bodies.end())
    {
        return std::unexpected("no placed mine");
    }

    // The player comes at the mine from 60 units back along a facing the terrain leaves open, and thrusts.
    const size_t PlacedIndex = Placed->ObjectIndex;
    constexpr float Approach = 600.f;
    constexpr int ApproachSteps = 300;
    FPlayerState Player;
    for (int Direction = 0; Direction < DirectionCount && !Placed->Mine->bStruck; ++Direction)
    {
        Player = SpawnPlayer(Level->Objects[*PlayerIndex], *Class);
        Player.Direction = Direction;
        Player.Pose = Player.TargetPose = Direction * (PlayerPoseCount / DirectionCount) + 1;
        const FCollisionPoint Facing = FacingVector(Direction);
        Player.Center = { .X = Placed->State.Center.X - Facing.X * Approach, .Y = Placed->State.Center.Y - Facing.Y * Approach };
        Player.Position = Player.PreviousPosition = WorldFromCollision(Player.Center);
        for (int Step = 0; Step < ApproachSteps && !Placed->Mine->bStruck; ++Step)
        {
            StepPlayer(Player, *Class, { .Thrust = EPlayerThrust::Forward }, StepMilliseconds, *Terrain, *Level, Bodies);
        }
    }

    const size_t EffectsBefore = Combat->Effects.size();
    const FPlayerTarget Target{ .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = *PlayerIndex };
    StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Target, Messages);
    if (!Level->Objects[PlacedIndex].bRemoved || Combat->Effects.size() == EffectsBefore)
    {
        return std::unexpected("the player did not set off the placed mine");
    }

    // Another placed mine shoved along each facing in turn goes off on the terrain or is worn by the first object it meets.
    const auto Shoved = std::ranges::find_if(Bodies, [](const FObjectBody& Body) { return Body.Mine && !Body.Mine->bStruck; });
    if (Shoved == Bodies.end())
    {
        return std::unexpected("no second placed mine");
    }

    const size_t ShovedIndex = Shoved->ObjectIndex;
    const FObjectBody Resting = *Shoved;
    constexpr float ShoveSpeed = 20.f;
    constexpr int ShoveSteps = 500;
    bool bShovedOff = false;
    for (int Direction = 0; Direction < DirectionCount && !bShovedOff; ++Direction)
    {
        FObjectBody& Mine = *std::ranges::find(Bodies, ShovedIndex, &FObjectBody::ObjectIndex);
        Mine = Resting;
        const FCollisionPoint Facing = FacingVector(Direction);
        Mine.State.bPushed = true;
        Mine.State.Velocity = { .X = Facing.X * ShoveSpeed, .Y = Facing.Y * ShoveSpeed };
        for (int Step = 0; Step < ShoveSteps && !bShovedOff && Mine.State.bPushed; ++Step)
        {
            StepObjectBodies(Bodies, StepMilliseconds, *Terrain, *Level);
            bShovedOff = Mine.Mine->bStruck || Mine.Shield < Resting.Shield || Mine.Energy < Resting.Energy;
        }
    }

    if (!bShovedOff)
    {
        return std::unexpected("a shoved mine met nothing");
    }

    const std::string_view ShovedOutcome = std::ranges::find(Bodies, ShovedIndex, &FObjectBody::ObjectIndex)->Mine->bStruck ? "went off on the terrain" : "was worn by an object";

    // The same mine shoved at the player standing just ahead goes off against it and pushes it along the mine's facing.
    bool bPlayerPushed = false;
    for (int Direction = 0; Direction < DirectionCount && !bPlayerPushed; ++Direction)
    {
        FObjectBody& Mine = *std::ranges::find(Bodies, ShovedIndex, &FObjectBody::ObjectIndex);
        Mine = Resting;
        const FCollisionPoint Unscaled = FacingVector(Direction);
        const float Length = std::sqrt(Unscaled.X * Unscaled.X + Unscaled.Y * Unscaled.Y);
        const FCollisionPoint Facing{ .X = Unscaled.X / Length, .Y = Unscaled.Y / Length };
        Mine.State.Direction = Direction;
        Mine.State.bPushed = true;
        Mine.State.Velocity = { .X = Facing.X * ShoveSpeed, .Y = Facing.Y * ShoveSpeed };
        constexpr float AheadGap = 30.f;
        FPlayerState Blocker = SpawnPlayer(Level->Objects[*PlayerIndex], *Class);
        const float Ahead = Class->Radius + Mine.Class.Radius + AheadGap;
        Blocker.Center = { .X = Mine.State.Center.X + Facing.X * Ahead, .Y = Mine.State.Center.Y + Facing.Y * Ahead };
        const FPlayerTarget Struck{ .State = Blocker, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = *PlayerIndex };
        for (int Step = 0; Step < ShoveSteps && Mine.State.bPushed && !Mine.Mine->bStruck; ++Step)
        {
            const FCollisionPoint Push = StepObjectBodies(Bodies, StepMilliseconds, *Terrain, *Level, &Struck);
            bPlayerPushed = Mine.Mine->bStruck && Push.X * Facing.X + Push.Y * Facing.Y > 0.f;
        }
    }

    if (!bPlayerPushed)
    {
        return std::unexpected("a mine shoved at the player did not go off against it");
    }

    // A dropped mine away from the player.
    const auto Ammo = std::ranges::find(Combat->AmmoClasses, std::string("mine-ammo"), &FAmmoClass::Name);
    if (Ammo == Combat->AmmoClasses.end() || !Ammo->bDropped)
    {
        return std::unexpected("Mine-Ammo is not a dropped ammo");
    }

    Combat->DroppedMines.push_back({ .AmmoIndex = size_t(Ammo - Combat->AmmoClasses.begin()), .Position = { .X = Level->Objects[*PlayerIndex].X, .Y = Level->Objects[*PlayerIndex].Y }, .Owner = *PlayerIndex });
    Player.Center = { .X = -10000.f, .Y = -10000.f };
    StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Target, Messages);
    const size_t DroppedIndex = Level->Objects.size() - 1;
    uint32_t ArmedAt = 0;
    uint32_t HiddenAt = 0;
    bool bSolidWhileHidden = false;
    uint32_t GoneAt = 0;
    constexpr uint32_t LongestLife = 300000;
    for (uint32_t Time = StepMilliseconds; Time < LongestLife && GoneAt == 0; Time += StepMilliseconds)
    {
        StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Target, Messages);
        const auto Dropped = std::ranges::find(Bodies, DroppedIndex, &FObjectBody::ObjectIndex);
        if (Dropped == Bodies.end())
        {
            GoneAt = Time;
            break;
        }

        ArmedAt = ArmedAt == 0 && !Dropped->bPassable && !Dropped->bInvisible ? Time : ArmedAt;
        if (HiddenAt == 0 && Dropped->bInvisible)
        {
            HiddenAt = Time;
            bSolidWhileHidden = !Dropped->bPassable;
        }
    }

    if (ArmedAt < uint32_t(Ammo->ActiveAfter) || HiddenAt <= ArmedAt || !bSolidWhileHidden || GoneAt == 0)
    {
        return std::unexpected(std::format("the dropped mine armed at {} ms, hid at {} ms and went at {} ms", ArmedAt, HiddenAt, GoneAt));
    }

    return std::format("a placed mine set off by the player, a shoved one {} and another went off against the player, a dropped one armed at {} ms, hidden at {} ms, gone at {} ms", ShovedOutcome, ArmedAt, HiddenAt, GoneAt);
}
}

namespace
{
// UNDERGROUND\T1U1's player under the firing surprises: Triple Fire's volley is three shots for one round, Circular Fire turns the player a direction a shot,
// and with Touch-and-destroy the player takes the energy of the first destructible object it runs into.
std::expected<std::string, std::string> VerifyFireSurprises(FResourceArchive& Archive)
{
    constexpr uint32_t StepMilliseconds = 14;
    constexpr float SpriteSize = 64.f;
    FMessages Messages;
    std::expected<FLevel, std::string> Level = LoadLevel(Archive, "underground\\t1u1");
    const std::optional<size_t> PlayerIndex = Level ? FindPlayerObject(*Level) : std::nullopt;
    if (!PlayerIndex)
    {
        return std::unexpected(Level ? std::string("no player") : Level.error());
    }

    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const std::expected<FMovementClass, std::string> Class = ReadMovementClass(Level->Objects[*PlayerIndex]);
    const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
    std::expected<FCombat, std::string> Combat = CreateCombat(*Level, Level->Objects[*PlayerIndex], Archive, Bodies);
    if (!Class || !Terrain || !Combat)
    {
        return std::unexpected(!Class ? Class.error() : !Terrain ? Terrain.error() : Combat.error());
    }

    const auto Template = Level->ClassObjects.find(ToLowerName(Level->Objects[*PlayerIndex].ClassName));
    FPlayerInventory Inventory = CreateInventory(Template != Level->ClassObjects.end() ? Template->second : Level->Objects[*PlayerIndex]);
    FPlayerState Player = SpawnPlayer(Level->Objects[*PlayerIndex], *Class);
    const auto Fire = [&]
    {
        Combat->Clock += StepMilliseconds;
        StepPlayerWeapon(*Combat, true, Player, Level->Objects[*PlayerIndex], *PlayerIndex, SpriteSize, Inventory, Messages);
    };

    // A plain volley, then one under Triple Fire, which opens the spread and goes out three times as wide for a single round.
    const FAmmoStock* const Stock = SlotAmmo(*Combat, Combat->PlayerSlot, Inventory);
    const size_t PlainBefore = Combat->Projectiles.size();
    Fire();
    const size_t PlainShots = Combat->Projectiles.size() - PlainBefore;
    Combat->Clock += Combat->Weapons[Combat->PlayerWeapon.Weapon].Delay;
    Inventory.Surprises.bTripleFire = true;
    const float RoundsBefore = Stock ? Stock->Count : 0.f;
    const size_t ShotsBefore = Combat->Projectiles.size();
    Fire();
    const size_t TripleShots = Combat->Projectiles.size() - ShotsBefore;
    if (!Stock || PlainShots == 0 || TripleShots != 3 * PlainShots || Stock->Count != RoundsBefore - 1.f || Inventory.Surprises.TripleSpread == 0)
    {
        return std::unexpected(std::format("Triple Fire made {} shots for {} rounds", TripleShots, Stock ? RoundsBefore - Stock->Count : 0.f));
    }

    Inventory.Surprises.bTripleFire = false;
    Inventory.Surprises.TripleSpread = 0;

    // Under Circular Fire every volley turns the player on by a direction; the 24 turns start again while the trigger stays held.
    Inventory.Surprises.bCircularFire = true;
    const int DirectionBefore = Player.Direction;
    size_t CircularVolleys = 0;
    constexpr uint32_t CircularMilliseconds = 3000;
    for (uint32_t Time = 0; Time < CircularMilliseconds; Time += StepMilliseconds)
    {
        const size_t Before = Combat->Projectiles.size();
        Fire();
        CircularVolleys += Combat->Projectiles.size() != Before ? 1 : 0;
    }

    if (CircularVolleys == 0 || Player.Direction != Wrap(DirectionBefore + int(CircularVolleys % DirectionCount)))
    {
        return std::unexpected(std::format("Circular Fire turned the player from {} to {} in {} volleys", DirectionBefore, Player.Direction, CircularVolleys));
    }

    Inventory.Surprises.bCircularFire = false;

    // Touch-and-destroy: the player comes at a destructible object from each facing until it has run into one.
    const auto Target = std::ranges::find_if(Bodies, [](const FObjectBody& Body) { return !Body.bIndestructible && !Body.Actor && !Body.Mine && !Body.Generator && 0.f < Body.Energy; });
    if (Target == Bodies.end())
    {
        return std::unexpected("no destructible object");
    }

    Player.bTouchAndDestroy = true;
    constexpr float Approach = 600.f;
    constexpr int ApproachSteps = 300;
    for (int Direction = 0; Direction < DirectionCount && !Target->bDamaged; ++Direction)
    {
        Player = SpawnPlayer(Level->Objects[*PlayerIndex], *Class);
        Player.bTouchAndDestroy = true;
        Player.Direction = Direction;
        Player.Pose = Player.TargetPose = Direction * (PlayerPoseCount / DirectionCount) + 1;
        const FCollisionPoint Facing = FacingVector(Direction);
        Player.Center = { .X = Target->State.Center.X - Facing.X * Approach, .Y = Target->State.Center.Y - Facing.Y * Approach };
        Player.Position = Player.PreviousPosition = WorldFromCollision(Player.Center);
        for (int Step = 0; Step < ApproachSteps && !Target->bDamaged; ++Step)
        {
            StepPlayer(Player, *Class, { .Thrust = EPlayerThrust::Forward }, StepMilliseconds, *Terrain, *Level, Bodies);
        }
    }

    if (!Target->bDamaged || Target->Energy != 0.f)
    {
        return std::unexpected(std::format("Touch-and-destroy left {} with {} energy", Level->Objects[Target->ObjectIndex].ClassName, Target->Energy));
    }

    // Started as a surprise it puts fifteen stars round the player, which go with it.
    FMovementClass PlayerClass = *Class;
    Player.bTouchAndDestroy = false;
    const size_t OrbitersBefore = Combat->Orbiters.size();
    StartSurprise(*Combat, *Level, Bodies, Player, PlayerClass, Inventory, ESurprise::TouchAndDestroy);
    const size_t Stars = Combat->Orbiters.size() - OrbitersBefore;
    const FPlayerTarget Target2{ .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = *PlayerIndex };
    StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Target2, Messages);
    const size_t StarsHeld = size_t(std::ranges::count_if(Combat->Orbiters, [](const FOrbiter& Orbiter) { return !Orbiter.Host; }));
    EndSurprise(Inventory.Surprises, ESurprise::TouchAndDestroy, Player, PlayerClass, Inventory);
    StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Target2, Messages);
    const size_t StarsLeft = size_t(std::ranges::count_if(Combat->Orbiters, [](const FOrbiter& Orbiter) { return !Orbiter.Host; }));
    if (Stars != 15 || StarsHeld != Stars || StarsLeft != 0)
    {
        return std::unexpected(std::format("Touch-and-destroy put {} stars round the player, kept {} and left {}", Stars, StarsHeld, StarsLeft));
    }

    return std::format("Triple Fire put out {} shots for a round, Circular Fire turned the player through {} volleys, Touch-and-destroy emptied a {} and circled the player with {} stars",
        TripleShots, CircularVolleys, Level->Objects[Target->ObjectIndex].ClassName, Stars);
}
}

namespace
{
// CPU\T3P2's Final-PC carries five ADD FX of three orbiters each, whose timers keep it invulnerable for 10 seconds and then open it for 2, hiding them.
// A Big-Boom at UNDERGROUND\T1U1's player throws its particles: half a second on, its debris flies outward in several directions, leaving smoke.
std::expected<std::string, std::string> VerifyDebris(FResourceArchive& Archive)
{
    constexpr uint32_t StepMilliseconds = 14;
    constexpr uint32_t FlightMilliseconds = 500;
    FMessages Messages;
    std::expected<FLevel, std::string> Level = LoadLevel(Archive, "underground\\t1u1");
    const std::optional<size_t> PlayerIndex = Level ? FindPlayerObject(*Level) : std::nullopt;
    if (!PlayerIndex)
    {
        return std::unexpected(Level ? std::string("no player") : Level.error());
    }

    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
    std::expected<FCombat, std::string> Combat = CreateCombat(*Level, Level->Objects[*PlayerIndex], Archive, Bodies);
    const std::expected<FMovementClass, std::string> Class = ReadMovementClass(Level->Objects[*PlayerIndex]);
    if (!Terrain || !Combat || !Class)
    {
        return std::unexpected(!Terrain ? Terrain.error() : !Combat ? Combat.error() : Class.error());
    }

    constexpr std::string_view Boom = "big-boom";
    if (const std::expected<void, std::string> Loaded = LoadEffect(*Combat, *Level, Archive, std::string(Boom)); !Loaded)
    {
        return std::unexpected(Loaded.error());
    }

    FPlayerInventory Inventory = CreateInventory(Level->Objects[*PlayerIndex]);
    const FPlayerState Player = SpawnPlayer(Level->Objects[*PlayerIndex], *Class);
    SpawnEffect(*Combat, Boom, Player.Position, 0.f);
    for (uint32_t Time = 0; Time < FlightMilliseconds; Time += StepMilliseconds)
    {
        Combat->Clock += StepMilliseconds;
        StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = *PlayerIndex }, Messages);
    }

    constexpr float FarEnough = 20.f;
    std::set<int> Directions;
    size_t Flying = 0;
    for (const FTrail& Trail : Combat->Trails)
    {
        if (Trail.Flight && std::hypot(Trail.Position.X - Player.Position.X, Trail.Position.Y - Player.Position.Y) > FarEnough)
        {
            ++Flying;
            Directions.insert(Trail.Flight->State.Direction);
        }
    }

    const size_t Smoke = size_t(std::ranges::count_if(Combat->Trails, [](const FTrail& Trail) { return !Trail.Flight; }));
    constexpr size_t FewestDirections = 3;
    if (Directions.size() < FewestDirections || Smoke == 0)
    {
        return std::unexpected(std::format("{} pieces flew off in {} directions, {} smoke trails", Flying, Directions.size(), Smoke));
    }

    return std::format("a Big-Boom threw {} pieces in {} directions, with {} smoke trails", Flying, Directions.size(), Smoke);
}

// A weld seam set off by hand must take its neighbours with it: seams have no box, so only blasts reach them.
std::expected<std::string, std::string> VerifyWeldSeams(FResourceArchive& Archive)
{
    constexpr uint32_t StepMilliseconds = 14;
    constexpr uint32_t ChainMilliseconds = 3000;
    constexpr std::string_view SeamClass = "soudure";
    FMessages Messages;
    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".3do"))
        {
            continue;
        }

        std::expected<FLevel, std::string> Level = LoadLevel(Archive, Name.substr(0, Name.size() - 4));
        const std::optional<size_t> PlayerIndex = Level ? FindPlayerObject(*Level) : std::nullopt;
        if (!PlayerIndex)
        {
            continue;
        }

        std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
        const auto IsSeam = [&](const FObjectBody& Body) { return Level->Objects[Body.ObjectIndex].ClassName == SeamClass; };
        const auto First = std::ranges::find_if(Bodies, IsSeam);
        if (First == Bodies.end())
        {
            continue;
        }

        const size_t Seams = size_t(std::ranges::count_if(Bodies, IsSeam));
        const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
        std::expected<FCombat, std::string> Combat = CreateCombat(*Level, Level->Objects[*PlayerIndex], Archive, Bodies);
        const std::expected<FMovementClass, std::string> Class = ReadMovementClass(Level->Objects[*PlayerIndex]);
        if (!Terrain || !Combat || !Class)
        {
            return std::unexpected(!Terrain ? Terrain.error() : !Combat ? Combat.error() : Class.error());
        }

        FPlayerInventory Inventory = CreateInventory(Level->Objects[*PlayerIndex]);
        const FPlayerState Player = SpawnPlayer(Level->Objects[*PlayerIndex], *Class);
        if (First->Explosions.empty())
        {
            return std::unexpected("a seam has no explosion");
        }

        const FExplosionBlock& Explosion = First->Explosions.front();
        SpawnEffect(*Combat, Explosion.BoomEffect, First->State.Position, First->Altitude, Explosion.Damage, Explosion.Zone);
        for (uint32_t Time = 0; Time < ChainMilliseconds; Time += StepMilliseconds)
        {
            Combat->Clock += StepMilliseconds;
            StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = *PlayerIndex }, Messages);
        }

        const size_t Left = size_t(std::ranges::count_if(Bodies, IsSeam));
        constexpr size_t FewestChained = 2;
        if (Seams - Left < FewestChained)
        {
            return std::unexpected(std::format("{} of {} weld seams went off in {}", Seams - Left, Seams, Level->Name));
        }

        return std::format("{} of {} weld seams in {} went off in a chain", Seams - Left, Seams, Level->Name);
    }

    return std::unexpected("no level has weld seams");
}

std::expected<std::string, std::string> VerifyOrbiters(FResourceArchive& Archive)
{
    constexpr uint32_t StepMilliseconds = 14;
    FMessages Messages;
    std::expected<FLevel, std::string> Level = LoadLevel(Archive, "cpu\\t3p2");
    const std::optional<size_t> PlayerIndex = Level ? FindPlayerObject(*Level) : std::nullopt;
    if (!PlayerIndex)
    {
        return std::unexpected(Level ? std::string("no player") : Level.error());
    }

    std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
    const std::expected<FTerrainCollision, std::string> Terrain = BuildTerrainCollision(*Level);
    std::expected<FCombat, std::string> Combat = CreateCombat(*Level, Level->Objects[*PlayerIndex], Archive, Bodies);
    const std::expected<FMovementClass, std::string> Class = ReadMovementClass(Level->Objects[*PlayerIndex]);
    if (!Terrain || !Combat || !Class)
    {
        return std::unexpected(!Terrain ? Terrain.error() : !Combat ? Combat.error() : Class.error());
    }

    const auto Final = std::ranges::find_if(Bodies, [](const FObjectBody& Body) { return Body.bFinalObject; });
    const size_t FinalIndex = Final != Bodies.end() ? Final->ObjectIndex : SIZE_MAX;
    const auto OnFinal = [&](const FOrbiter& Orbiter) { return Orbiter.Host == FinalIndex; };
    const size_t Orbiters = size_t(std::ranges::count_if(Combat->Orbiters, OnFinal));
    if (Final == Bodies.end() || Orbiters != 15)
    {
        return std::unexpected(std::format("the Final-PC has {} orbiters", Orbiters));
    }

    FPlayerInventory Inventory = CreateInventory(Level->Objects[*PlayerIndex]);
    // The player waits far away, out of every fight.
    FPlayerState Player = SpawnPlayer(Level->Objects[*PlayerIndex], *Class);
    uint32_t OpenedAt = 0;
    uint32_t ClosedAt = 0;
    bool bHiddenWhileOpen = false;
    constexpr uint32_t Duration = 13000;
    for (uint32_t Time = StepMilliseconds; Time <= Duration; Time += StepMilliseconds)
    {
        Combat->Clock += StepMilliseconds;
        StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = *PlayerIndex },
            Messages);
        const auto Host = std::ranges::find(Bodies, FinalIndex, &FObjectBody::ObjectIndex);
        if (Host == Bodies.end())
        {
            return std::unexpected("the Final-PC was destroyed");
        }

        if (OpenedAt == 0 && !Host->bInvulnerable)
        {
            OpenedAt = Time;
            bHiddenWhileOpen = std::ranges::all_of(Combat->Orbiters | std::views::filter(OnFinal), &FOrbiter::bHidden);
        }
        else if (OpenedAt != 0 && ClosedAt == 0 && Host->bInvulnerable)
        {
            ClosedAt = Time;
        }
    }

    constexpr uint32_t OnTime = 10000;
    constexpr uint32_t OffTime = 2000;
    if (OpenedAt < OnTime || OpenedAt > OnTime + StepMilliseconds || ClosedAt < OpenedAt + OffTime || ClosedAt > OpenedAt + OffTime + StepMilliseconds || !bHiddenWhileOpen)
    {
        return std::unexpected(std::format("the Final-PC opened at {} ms, hid its orbiters {}, and closed at {} ms", OpenedAt, bHiddenWhileOpen, ClosedAt));
    }

    return std::format("the Final-PC's {} orbiters open it at {} ms and close it at {} ms, {} orbiters in all", Orbiters, OpenedAt, ClosedAt, Combat->Orbiters.size());
}
}

std::expected<std::string, std::string> VerifyCombat(FResourceArchive& Archive)
{
    // The check ignores the floating messages it raises.
    FMessages Messages;
    // Every level's player spins on the spot for a few seconds with the trigger held, while the turrets that see it shoot back; each level must load its weapons and the player must fire.
    constexpr uint32_t StepMilliseconds = 14;
    constexpr uint32_t DurationMilliseconds = 6000;
    constexpr float SpriteSize = 64.f;
    size_t Levels = 0;
    size_t Shots = 0;
    size_t TurretShots = 0;
    size_t GrenadeBounces = 0;
    // Levels where a bouncing grenade threw off its Small-Metal-Bounce-Spark.
    size_t SparkLevels = 0;
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
        // The class's own stocks, as a player arriving by lift brings them: some levels' player objects start empty (t2u2 has no Gatling ammo).
        const auto Template = Level->ClassObjects.find(ToLowerName(PlayerObject->ClassName));
        FPlayerInventory Inventory = CreateInventory(Template != Level->ClassObjects.end() ? Template->second : *PlayerObject);
        const size_t BodyCount = Bodies.size();
        size_t LevelShots = 0;
        const FPlayerCommand Spin{ .Turn = EPlayerTurn::Left };
        for (uint32_t Time = 0; Time < DurationMilliseconds; Time += StepMilliseconds)
        {
            StepPlayer(Player, *Class, Spin, StepMilliseconds, *Terrain, *Level, Bodies);
            Combat->Clock += StepMilliseconds;
            const size_t Before = Combat->Projectiles.size();
            StepPlayerWeapon(*Combat, true, Player, Level->Objects[PlayerIndex], PlayerIndex, SpriteSize, Inventory, Messages);
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

        // Grenades bounce off the terrain: thrown all round from where the player stopped, some must bounce and fly on before their fuse.
        if (const std::expected<size_t, std::string> Grenades = LoadWeapon(*Combat, *Level, "Grenade-Launcher"))
        {
            constexpr uint32_t FlightMilliseconds = 2500;
            Combat->Projectiles.clear();
            FWeaponState Throw{ .Weapon = *Grenades };
            FAmmoStock Stock{ .Name = Combat->Weapons[*Grenades].Ammo, .Count = float(DirectionCount), .Max = float(DirectionCount) };
            for (int Direction = 0; Direction < DirectionCount; ++Direction)
            {
                Throw.LastFireTime = 0;
                FireWeapon(*Combat, Throw, { .Position = Player.Position, .Direction = Direction, .SpriteSize = SpriteSize, .ObjectIndex = PlayerIndex }, &Stock);
            }

            const auto Spark = std::ranges::find(Combat->EffectClasses, std::string("small-metal-bounce-spark"), &FEffectClass::Name);
            bool bSparked = false;
            for (uint32_t Time = 0; Time < FlightMilliseconds; Time += StepMilliseconds)
            {
                Combat->Clock += StepMilliseconds;
                StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex }, Messages);
                bSparked = bSparked || (Spark != Combat->EffectClasses.end() && std::ranges::contains(Combat->Effects, size_t(Spark - Combat->EffectClasses.begin()), &FEffect::ClassIndex));
            }

            SparkLevels += bSparked ? 1 : 0;
            GrenadeBounces += size_t(std::ranges::count_if(Combat->Projectiles, [](const FProjectile& Shot) { return Shot.Bounces > 0; }));
            Combat->Projectiles.clear();
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
                const FPlayerTarget Target{ .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
                StepActors(&*Combat, Bodies, StepMilliseconds, *Terrain, *Level, &Target);
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

        // Nine in ten generators must give birth within 20 seconds while the player waits at the start; the others have something standing on them, which holds a birth back (ObjectOverlapsSolid).
        constexpr uint32_t GeneratorMilliseconds = 20000;
        std::set<size_t> Parents;
        const FPlayerState Waiting = SpawnPlayer(Level->Objects[PlayerIndex], *Class);
        const FPlayerTarget Target{ .State = Waiting, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex };
        Generators += size_t(std::ranges::count_if(Bodies, [&](const FObjectBody& Body) { return Body.Generator && !Level->Objects[Body.ObjectIndex].bRemoved; }));
        ActivateGenerators(*Level, Bodies, Target);
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

    // A complete timed code goes back where its parts lay once its time is up.
    {
        FLevel CodeLevel;
        CodeLevel.Objects.resize(2);
        CodeLevel.Objects[0].bRemoved = true;
        CodeLevel.Objects[1].bRemoved = true;
        FMessages CodeMessages;
        constexpr int CodeId = 7;
        constexpr int32_t CodeTimeout = 3000;
        constexpr int32_t CodeReactivate = 5000;
        constexpr uint32_t CodeStep = 10;
        constexpr uint32_t CodeMilliseconds = 3100;
        const FCarriedKey Part{ .Id = CodeId, .bCode = true, .PartId = 1, .PartCount = 2, .bHasTimeout = true, .Timeout = CodeTimeout, .ReactivateAfter = CodeReactivate };
        FCarriedKey Second = Part;
        Second.PartId = 2;
        Second.ObjectIndex = 1;
        FPlayerInventory Coded{ .Keys = { Part, Second } };
        for (uint32_t Time = 0; Time < CodeMilliseconds; Time += CodeStep)
        {
            StepCodeTimeouts(Coded, CodeLevel, CodeMessages, CodeStep);
        }

        if (!Coded.Keys.empty() || CodeLevel.Objects[0].bRemoved || CodeLevel.Objects[1].bRemoved)
        {
            return std::unexpected("a timed code did not go back into the level");
        }
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

    if (GrenadeBounces == 0)
    {
        return std::unexpected("no grenade bounced off the terrain");
    }

    if (CopDuels == 0 || CopShots == 0)
    {
        return std::unexpected(std::format("no cop fired in {} duels", CopDuels));
    }

    const std::expected<std::string, std::string> Mines = VerifyMines(Archive);
    if (!Mines)
    {
        return std::unexpected(std::format("Mines: {}", Mines.error()));
    }

    const std::expected<std::string, std::string> FireSurprises = VerifyFireSurprises(Archive);
    if (!FireSurprises)
    {
        return std::unexpected(std::format("Surprises: {}", FireSurprises.error()));
    }

    const std::expected<std::string, std::string> OrbitCheck = VerifyOrbiters(Archive);
    if (!OrbitCheck)
    {
        return std::unexpected(std::format("Orbiters: {}", OrbitCheck.error()));
    }

    const std::expected<std::string, std::string> DebrisCheck = VerifyDebris(Archive);
    if (!DebrisCheck)
    {
        return std::unexpected(std::format("Debris: {}", DebrisCheck.error()));
    }

    const std::expected<std::string, std::string> SeamCheck = VerifyWeldSeams(Archive);
    if (!SeamCheck)
    {
        return std::unexpected(std::format("Weld seams: {}", SeamCheck.error()));
    }

    if (SparkLevels == 0)
    {
        return std::unexpected("no bouncing grenade threw off a spark");
    }

    return std::format("{} levels, {} player shots, {} turret shots while spinning, {} grenades bounced (sparking in {} levels), {} objects destroyed, {} turrets wrecked, {} objects dropped, at least {} effects; "
        "{} turret duels: {} shots, {:.0f} damage taken; {} cop duels ({} classes): {} shots; {} of {} generators gave birth; {}; {}; {}; {}; {}",
        Levels, Shots, TurretShots, GrenadeBounces, SparkLevels, Destroyed, Wrecks, Dropped, Effects, Duels, DuelShots, DamageTaken, CopDuels, CopNames.size(), CopShots, GeneratorsBirthing, Generators,
        *Mines, *FireSurprises, *OrbitCheck, *DebrisCheck, *SeamCheck);
}

FDifficultyTable LoadDifficulties(const std::string_view Text)
{
    // DAT_0050d328: Easy, Normal, Hard and Insane before the file.
    FDifficultyTable Table{ .Levels = { {
        { 20000.f, 20000.f, 1.f, 0.5f, 0.5f, 2.f, 0.f, 2.f, 0.5f, 0.5f },
        { 10000.f, 10000.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f, 1.f },
        { 10000.f, 10000.f, 0.5f, 2.f, 3.f, 1.f, 2.f, 0.5f, 2.f, 2.f },
        { 10000.f, 10000.f, 0.5f, 4.f, 6.f, 1.f, 4.f, 0.25f, 4.f, 4.f },
    } } };

    // LoadObjectClassLibrary (keywords 0x102 to 0x10c): difficulty-level picks the level the following values go to, and the last one stays the default.
    constexpr std::array<std::pair<std::string_view, float FDifficulty::*>, 10> Fields = { {
        { "player-shield-max", &FDifficulty::PlayerShieldMax },
        { "player-nrj-max", &FDifficulty::PlayerEnergyMax },
        { "nmy-scan-delay-ratio", &FDifficulty::ScanDelayRatio },
        { "nmy-scan-range-ratio", &FDifficulty::ScanRangeRatio },
        { "nmy-shield-up-step-ratio", &FDifficulty::EnemyShieldUpStepRatio },
        { "player-shield-up-step-ratio", &FDifficulty::PlayerShieldUpStepRatio },
        { "mine-random-count-ratio", &FDifficulty::MineRandomCountRatio },
        { "nmy-fire-delay-ratio", &FDifficulty::EnemyFireDelayRatio },
        { "shield-up-item-value-ratio", &FDifficulty::ShieldUpItemValueRatio },
        { "nrj-up-item-value-ratio", &FDifficulty::EnergyUpItemValueRatio },
    } };
    size_t Level = Table.DefaultLevel;
    for (const auto Part : std::views::split(Text, '\n'))
    {
        std::string_view Row(Part.begin(), Part.end());
        Row = Row.substr(0, Row.find('#'));
        const size_t Equals = Row.find('=');
        if (Equals == std::string_view::npos)
        {
            continue;
        }

        const std::string Key = ToLowerName(std::string_view(Row.substr(0, Equals)));
        const std::optional<float> Value = ParseLevelFloat(Row.substr(Equals + 1));
        if (!Value)
        {
            continue;
        }

        if (Key == "difficulty-level")
        {
            if (*Value >= 0.f && *Value < float(DifficultyLevelCount))
            {
                Level = size_t(*Value);
                Table.DefaultLevel = Level;
            }

            continue;
        }

        if (const auto Field = std::ranges::find(Fields, Key, &std::pair<std::string_view, float FDifficulty::*>::first); Field != Fields.end())
        {
            Table.Levels[Level].*(Field->second) = *Value;
        }
    }

    return Table;
}

void ApplyDifficulty(const FDifficulty& Difficulty, const FCombat& Combat, const std::span<FObjectBody> Bodies, FPlayerInventory& Inventory)
{
    // The player: the class's shield and energy maxima, and its Shield-up-step scaled.
    if (0.f < Difficulty.PlayerShieldMax)
    {
        Inventory.ShieldMax = Difficulty.PlayerShieldMax;
        Inventory.Shield = std::min(Inventory.Shield, Inventory.ShieldMax);
    }

    if (Difficulty.PlayerEnergyMax != 1.f)
    {
        Inventory.EnergyMax = Difficulty.PlayerEnergyMax;
        Inventory.Energy = std::min(Inventory.Energy, Inventory.EnergyMax);
    }

    if (Difficulty.PlayerShieldUpStepRatio != 1.f)
    {
        Inventory.ShieldUpStep *= Difficulty.PlayerShieldUpStepRatio;
    }

    // Every hunter: a seeker's or tracker's scan delay and sensor range, its Shield-up-step, and its own copy of its weapon's Delay.
    const auto Scaled = [](const float Value, const float Ratio) { return TruncateLow(double(float(Value * Ratio))); };
    for (FObjectBody& Body : Bodies)
    {
        if (!HuntsPlayer(Body))
        {
            continue;
        }

        FWeaponState* Weapon = nullptr;
        if (Body.Actor && Body.Actor->Seeker)
        {
            FSeeker& Seeker = *Body.Actor->Seeker;
            if (Difficulty.ScanDelayRatio != 1.f)
            {
                Seeker.Work.Interval = Scaled(float(Seeker.Work.Interval), Difficulty.ScanDelayRatio);
            }

            if (Difficulty.ScanRangeRatio != 1.f)
            {
                Seeker.SensorRange = float(Scaled(Seeker.SensorRange, Difficulty.ScanRangeRatio));
            }

            Weapon = Seeker.Weapon ? &Seeker.WeaponState : nullptr;
        }
        else if (Body.Turret)
        {
            if (Difficulty.ScanDelayRatio != 1.f)
            {
                Body.Turret->ScanDelay = Scaled(float(Body.Turret->ScanDelay), Difficulty.ScanDelayRatio);
            }

            if (Difficulty.ScanRangeRatio != 1.f)
            {
                Body.Turret->SensorRange = float(Scaled(Body.Turret->SensorRange, Difficulty.ScanRangeRatio));
            }

            Weapon = &Body.Turret->Weapon;
        }

        if (Difficulty.EnemyShieldUpStepRatio != 1.f)
        {
            Body.ShieldUpStep *= Difficulty.EnemyShieldUpStepRatio;
        }

        if (Weapon && Difficulty.EnemyFireDelayRatio != 1.f && Weapon->Weapon < Combat.Weapons.size())
        {
            Weapon->Delay = uint32_t(Scaled(float(Combat.Weapons[Weapon->Weapon].Delay), Difficulty.EnemyFireDelayRatio));
        }
    }
}

void StepShields(const std::span<FObjectBody> Bodies, const FLevel& Level, FPlayerInventory& Inventory)
{
    // Cops, the player, walkers, burners, jeeps, rollers, helis, turrets, spiders and tanks (the 0x1ff40 kinds).
    constexpr std::array<std::string_view, 9> ShieldKinds = { "cop", "walker", "burner", "jeep", "roller", "heli", "turret", "spider", "tank" };
    for (FObjectBody& Body : Bodies)
    {
        if (Body.ShieldUpStep != 0.f && !Level.Objects[Body.ObjectIndex].bRemoved && std::ranges::contains(ShieldKinds, Level.Objects[Body.ObjectIndex].SubType))
        {
            Body.Shield = std::min(Body.Shield + Body.ShieldUpStep, Body.ShieldMax);
        }
    }

    if (Inventory.ShieldUpStep != 0.f)
    {
        Inventory.Shield = std::min(Inventory.Shield + Inventory.ShieldUpStep, Inventory.ShieldMax);
    }
}
