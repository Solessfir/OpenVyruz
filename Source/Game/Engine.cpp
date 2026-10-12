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
#include <optional>
#include <string_view>
#include <tuple>
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

// PlayerAdvancePose at 00438390 for 48-pose bodies: one pose per call, the short way round.
int AdvancePose(const int Pose, const int Target)
{
    constexpr int Half = PlayerPoseCount / 2;
    if (Pose < Target)
    {
        if (Target - Pose < Half)
        {
            return Pose + 1;
        }

        return Pose == 1 ? PlayerPoseCount : Pose - 1;
    }

    if (Pose > Target)
    {
        if (Pose - Target >= Half)
        {
            return Pose == PlayerPoseCount ? 1 : Pose + 1;
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
    const float Strafe = std::clamp(Command.Strafe, Class.ThrustMin, Class.ThrustMax);
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
        X = float(double(Player.Speed) * double(Facing.X) / double(Class.Weight) + X);
        Y = float(double(Player.Speed) * double(Facing.Y) / double(Class.Weight) + Y);
    }

    if (Strafe != 0.f)
    {
        const FCollisionPoint Side = FacingVector(Player.Direction, StrafeDirectionOffset);
        X = float(double(Strafe) * double(Side.X) + X);
        Y = float(double(Strafe) * double(Side.Y) + Y);
    }

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

void StepPlayer(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain,
    const FLevel& Level, const std::span<FObjectBody> Bodies, const FObjectBody* const Self)
{
    TurnPlayer(Player, Class, Command.Turn, StepMilliseconds);
    if (Command.Thrust == EPlayerThrust::None && Command.Strafe == 0.f && IsZero(Player.Velocity))
    {
        Player.PreviousPosition = Player.Position;
        DecaySpeed(Player, Class);
        return;
    }

    const float TimeScale = float(ReferenceStepMilliseconds / double(StepMilliseconds));
    const FCollisionPoint Delta = Displacement(Player, Class, Command, TimeScale);
    if (IsZero(Delta) && Player.Pose == Player.TargetPose)
    {
        Player.PreviousPosition = Player.Position;
        Player.Velocity = {};
        return;
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
    const std::optional<FTerrainHit> TerrainHit = QueryTerrainCollision(Terrain, { .Center = Player.Center, .Radius = Class.Radius }, { .Center = Candidate, .Radius = Class.Radius });

    // ponytail: objects are scanned after the terrain in object order instead of interleaved through the sweep endpoints; the nearest contact
    // still wins, so only exact ties between a wall and an object can resolve differently.
    std::optional<FCollisionPoint> Contact = TerrainHit ? std::optional(TerrainHit->Point) : std::nullopt;
    FObjectBody* HitBody = nullptr;
    for (FObjectBody& Body : Bodies)
    {
        if (&Body == Self)
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
        Player.Velocity = { .X = float(double(TimeScale) * Delta.X), .Y = float(double(TimeScale) * Delta.Y) };
        Player.Center = Candidate;
        return;
    }

    // ObjectRestorePosition at 004449f0 undoes the move before the bounce is resolved.
    Player.Position = Player.PreviousPosition;
    const EBounce Result = HitBody ? Push(Player, Class, Command, *HitBody, *Contact) : Bounce(Player, Class, Command, *Contact);
    if (Result == EBounce::Accept)
    {
        // Moving away from the contact commits the collision centre; the world position catches up on the next step.
        Player.Center = Candidate;
    }
}

std::vector<FObjectBody> CreateObjectBodies(const FLevel& Level)
{
    std::vector<FObjectBody> Bodies;
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        // ponytail: only static scenery for now; enemies, pickups, gates and lifts join once their behaviours move, collect or open them.
        // Kinds come from the class Sub-type or the GAF folder (CreateObject at 00413800). ponytail: actors, lifts and gates join once their
        // behaviours move or open them; pickups are collected instead of blocking.
        constexpr std::array<std::string_view, 6> SolidKinds = { "item", "thing", "turret", "staticgen", "burner", "roller" };
        if (Object.BoundingBoxSize <= 0 || !std::ranges::contains(SolidKinds, Object.SubType) || IsPickup(Object))
        {
            continue;
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
        Bodies.push_back(std::move(Body));
    }

    return Bodies;
}

void StepObjectBodies(const std::span<FObjectBody> Bodies, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level)
{
    for (FObjectBody& Body : Bodies)
    {
        if (Body.State.bPushed || !IsZero(Body.State.Velocity))
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

    return {};
}

std::expected<std::string, std::string> VerifyDemoReplay(FResourceArchive& Archive)
{
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
        Collected += CollectPickups(Recorded, *Class, Inventory, Pickups, PickupLevel);
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

std::vector<FPickup> CreatePickups(const FLevel& Level)
{
    std::vector<FPickup> Pickups;
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        const FLevelPropertyBlock* const Item = IsPickup(Object) ? FindItemBlock(Object) : nullptr;
        if (!Item)
        {
            continue;
        }

        const auto Number = [Item](const std::string_view Field) { return ParseLevelFloat(Item->Find(Field)).value_or(0.f); };
        Pickups.push_back({
            .ObjectIndex = Index,
            .Circle = { .Center = CollisionFromWorld({ .X = Object.X, .Y = Object.Y }), .Radius = Object.BoundingBoxSize > 0 ? float(Object.BoundingBoxSize) : DefaultCollisionRadius },
            .AddEnergy = Number("Add-NRJ"),
            .AddShield = Number("Add-Shield"),
            .AddMoney = Number("Add-Money"),
            .AmmoType = std::string(Item->Find("Ammo-Type")),
            .AddAmmo = Number("Add-Ammo"),
        });
    }

    return Pickups;
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

size_t CollectPickups(const FPlayerState& Player, const FMovementClass& Class, FPlayerInventory& Inventory, std::vector<FPickup>& Pickups, FLevel& Level)
{
    // Energy is only topped up below 95% of its maximum; shield below its maximum; money always.
    constexpr float EnergyRefillLimit = 0.95f;
    size_t Collected = 0;
    for (FPickup& Pickup : Pickups)
    {
        FLevelObject& Object = Level.Objects[Pickup.ObjectIndex];
        if (Object.bRemoved || !IntersectCircles({ .Center = Player.Center, .Radius = Class.Radius }, Pickup.Circle))
        {
            continue;
        }

        bool bUsed = false;
        if (Pickup.AddEnergy != 0.f && Inventory.Energy < Inventory.EnergyMax * EnergyRefillLimit)
        {
            Inventory.Energy = std::min(Inventory.EnergyMax, Inventory.Energy + Pickup.AddEnergy);
            bUsed = true;
        }

        if (Pickup.AddMoney != 0.f)
        {
            Inventory.Money += Pickup.AddMoney;
            bUsed = true;
        }

        if (Pickup.AddShield != 0.f && Inventory.Shield < Inventory.ShieldMax)
        {
            Inventory.Shield = std::min(Inventory.ShieldMax, Inventory.Shield + Pickup.AddShield);
            bUsed = true;
        }

        // ponytail: AddAmmo (00444540) is not traced yet; ammo fills up to the stock's maximum and counts as used when anything was added.
        if (!Pickup.AmmoType.empty() && Pickup.AddAmmo != 0.f)
        {
            const auto Stock = std::ranges::find(Inventory.Ammo, Pickup.AmmoType, &FAmmoStock::Name);
            if (Stock != Inventory.Ammo.end() && Stock->Count < Stock->Max)
            {
                Stock->Count = std::min(Stock->Max, Stock->Count + Pickup.AddAmmo);
                bUsed = true;
            }
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
            ApplyDamage(Player.Inventory.Energy, Player.Inventory.Shield, *Amount);
        }
    }
}

FObjectBody* FindStruckBody(std::vector<FObjectBody>& Bodies, const FCollisionCircle Shot, const size_t Owner, std::optional<FCollisionPoint>& Contact)
{
    FObjectBody* Struck = nullptr;
    for (FObjectBody& Body : Bodies)
    {
        if (Body.ObjectIndex == Owner)
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

// UpdateAnimation at 0045e7e0: each frame is held for its duration in display periods; a one-shot animation ends after its last frame.
void AdvanceEffect(FEffect& Effect, const FEffectClass& Class, const uint32_t StepMilliseconds, const uint32_t FramePeriod)
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

        Effect.FrameTimer += Class.FrameDurations[Effect.Frame - 1] * int32_t(FramePeriod);
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
    AdvanceEffect(Effect, *Class, 0, Combat.FramePeriodMilliseconds);
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

        Combat.AmmoClasses.push_back({
            .Name = AmmoName,
            .Movement = Movement,
            .Damage = ClassNumber(*AmmoBlock, "Damage"),
            .DamageZone = ClassNumber(*AmmoBlock, "Damage-zone"),
            .DamageAfter = uint32_t(ClassNumber(*AmmoBlock, "Damage-after")),
            .HitEffect = ToLowerName(AmmoBlock->Find("Hit-fx")),
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

std::expected<FCombat, std::string> CreateCombat(const FLevel& Level, const FLevelObject& Player, FResourceArchive& Archive, const uint32_t FramePeriod,
    std::vector<FObjectBody>& Bodies)
{
    FCombat Combat{ .FramePeriodMilliseconds = FramePeriod };
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

    if (!Combat.PlayerWeapons.empty())
    {
        Combat.PlayerWeapon.Weapon = Combat.PlayerWeapons.front();
        Combat.PlayerWeapon.Slope = Combat.Weapons[Combat.PlayerWeapon.Weapon].InitialSlope;
    }

    // ponytail: only turrets run the seeker so far; cops, walkers and the other seekers need path following first.
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

    for (const std::string& Name : EffectNames)
    {
        if (const std::expected<void, std::string> Result = LoadEffect(Combat, Level, Archive, Name); !Result)
        {
            return std::unexpected(Result.error());
        }
    }

    return Combat;
}

void StepPlayerWeapon(FCombat& Combat, const bool bFire, const FPlayerState& Player, const FLevelObject& PlayerObject, const size_t PlayerIndex,
    const float SpriteSize, FPlayerInventory& Inventory)
{
    if (Combat.PlayerWeapons.empty())
    {
        return;
    }

    FWeaponState& State = Combat.PlayerWeapon;
    const FWeaponClass& Weapon = Combat.Weapons[State.Weapon];
    UpdateSlope(State, Weapon, bFire);
    if (!bFire)
    {
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

void StepTurrets(FCombat& Combat, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerState& Player)
{
    const int32_t Step = int32_t(StepMilliseconds);
    for (FObjectBody& Body : Bodies)
    {
        if (!Body.Turret)
        {
            continue;
        }

        FTurret& Turret = *Body.Turret;
        const FWeaponClass& Weapon = Combat.Weapons[Turret.Weapon.Weapon];
        const FCollisionPoint ToTarget{ .X = float(double(Player.Center.X) - double(Body.State.Center.X)), .Y = float(double(Player.Center.Y) - double(Body.State.Center.Y)) };
        const float DistanceSquared = float(double(ToTarget.X) * double(ToTarget.X) + double(ToTarget.Y) * double(ToTarget.Y));
        Turret.bTrigger = false;
        bool bEngaged = false;
        if (!Turret.bLocked)
        {
            // SeekerFindTarget (0042da80) looks for the target within Sensor-Range and the vision cone around the base's facing; PathStartSearch (0040f6f0) routes to it.
            // ponytail: the A* route over collision shortcuts (FUN_00410460) is replaced by a clear straight line, which is all an engaging turret accepts.
            if (Turret.ScanTimer < 1)
            {
                Turret.ScanTimer += Turret.ScanDelay;
                const bool bInRange = DistanceSquared <= float(int(Turret.SensorRange) * int(Turret.SensorRange));
                const bool bInView = DirectionDistance(FindDirection(ToTarget), Turret.BaseDirection) <= Turret.VisionDirections;
                // The ray starts at the turret's edge: turrets stand on scenery that the terrain collision also covers.
                const double Distance = std::sqrt(double(DistanceSquared));
                const double Edge = Distance == 0. ? 0. : double(Body.Class.Radius) / Distance;
                const FCollisionPoint Start{ .X = float(double(Body.State.Center.X) + double(ToTarget.X) * Edge), .Y = float(double(Body.State.Center.Y) + double(ToTarget.Y) * Edge) };
                const bool bClear = Distance <= double(Body.Class.Radius) || IsLineClear(Terrain, Start, Player.Center);
                if (bInRange && bInView && bClear)
                {
                    Turret.bLocked = true;
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

void StepProjectiles(FCombat& Combat, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerTarget& Player)
{
    const float TimeScale = float(ReferenceStepMilliseconds / double(StepMilliseconds));
    constexpr FPlayerCommand Straight{ .Thrust = EPlayerThrust::Forward };
    std::erase_if(Combat.Projectiles, [&](FProjectile& Shot)
    {
        const FAmmoClass& Ammo = Combat.AmmoClasses[Shot.AmmoIndex];
        // Target is the object struck, bPlayer the player struck; bSurface marks a wall or other terrain contact.
        const auto Detonate = [&](FObjectBody* const Target, const bool bPlayer, const bool bSurface)
        {
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
                ApplyDamage(Player.Inventory.Energy, Player.Inventory.Shield, Ammo.Damage);
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

        AdvanceEffect(Effect, Class, StepMilliseconds, Combat.FramePeriodMilliseconds);
        return Effect.bFinished;
    });

    std::erase_if(Bodies, [&](const FObjectBody& Body)
    {
        // Objects die in their next update once damage has taken their energy to zero.
        if (!Body.bDamaged || 0.f < Body.Energy)
        {
            return false;
        }

        FLevelObject& Object = Level.Objects[Body.ObjectIndex];
        Object.bRemoved = true;
        // ObjectDestroy (00437d40) hands each TYPE Explosion block to a new Boom-fx object at the same spot.
        // ponytail: two-piece wrecks that stay behind and surprise drops are not ported.
        for (const FExplosionBlock& Explosion : Body.Explosions)
        {
            SpawnEffect(Combat, Explosion.BoomEffect, Body.State.Position, Body.Altitude, Explosion.Damage, Explosion.Zone);
        }

        if (0.f < Body.Reward && PaysReward(Object.SubType))
        {
            Player.Inventory.Money += Body.Reward;
        }

        return true;
    });
}

std::expected<std::string, std::string> VerifyCombat(FResourceArchive& Archive)
{
    // Every level's player spins on the spot for a few seconds with the trigger held, while the turrets that see it shoot back; each level must
    // load its weapons and the player must fire.
    constexpr uint32_t StepMilliseconds = 14;
    constexpr uint32_t DurationMilliseconds = 6000;
    constexpr float SpriteSize = 64.f;
    constexpr uint32_t VerifyRefreshMillihertz = 60000;
    size_t Levels = 0;
    size_t Shots = 0;
    size_t TurretShots = 0;
    size_t Duels = 0;
    size_t DuelShots = 0;
    float DamageTaken = 0.f;
    size_t Destroyed = 0;
    size_t Effects = 0;
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
        std::expected<FCombat, std::string> Combat = CreateCombat(*Level, *PlayerObject, Archive, FramePeriodMilliseconds(VerifyRefreshMillihertz), Bodies);
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
            StepPlayerWeapon(*Combat, true, Player, *PlayerObject, PlayerIndex, SpriteSize, Inventory);
            LevelShots += Combat->Projectiles.size() - Before;
            const size_t TurretBefore = Combat->Projectiles.size();
            StepTurrets(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, Player);
            TurretShots += Combat->Projectiles.size() - TurretBefore;
            const size_t EffectsBefore = Combat->Effects.size();
            StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex });
            Effects += Combat->Effects.size() > EffectsBefore ? Combat->Effects.size() - EffectsBefore : 0;
        }

        if (LevelShots == 0)
        {
            return std::unexpected(std::format("{}: the player never fired", Name));
        }

        // Then the player waits in clear view of the first turret that has such a spot inside its weapon's range, and must draw fire.
        constexpr std::array<float, 2> DuelDistances = { 150.f, 250.f };
        constexpr uint32_t DuelMilliseconds = 3000;
        std::optional<FCollisionPoint> Spot;
        for (const FObjectBody& Body : Bodies)
        {
            for (const float Distance : DuelDistances)
            {
                for (int Direction = 0; Body.Turret && !Spot && Direction < DirectionCount; ++Direction)
                {
                    // Facing vectors are a tenth of a unit long.
                    constexpr float TenthsPerUnit = 10.f;
                    const FCollisionPoint Facing = FacingVector(Direction);
                    const float Scale = Distance * TenthsPerUnit;
                    const FCollisionPoint Edge{ .X = Body.State.Center.X + Facing.X * Body.Class.Radius * TenthsPerUnit, .Y = Body.State.Center.Y + Facing.Y * Body.Class.Radius * TenthsPerUnit };
                    const FCollisionPoint Candidate{ .X = Body.State.Center.X + Facing.X * Scale, .Y = Body.State.Center.Y + Facing.Y * Scale };
                    const FCollisionPoint World = WorldFromCollision(Candidate);
                    if (World.X > 1.f && World.Y > 1.f && World.X < float(Level->WorldExtentX) && World.Y < float(Level->WorldExtentY) &&
                        IsLineClear(*Terrain, Edge, Candidate) && DirectionDistance(Direction, Body.Turret->BaseDirection) <= Body.Turret->VisionDirections)
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
                StepProjectiles(*Combat, StepMilliseconds, *Terrain, *Level, Bodies, { .State = Player, .Radius = Class->Radius, .Inventory = Inventory, .ObjectIndex = PlayerIndex });
            }

            ++Duels;
            DamageTaken += EnergyBefore - (Inventory.Energy + Inventory.Shield);
        }

        ++Levels;
        Shots += LevelShots;
        Destroyed += BodyCount - Bodies.size();
    }

    if (Duels != 0 && DuelShots == 0)
    {
        return std::unexpected(std::format("no turret fired in {} duels", Duels));
    }

    return std::format("{} levels, {} player shots, {} turret shots while spinning, {} objects destroyed, at least {} effects; {} turret duels: {} shots, {:.0f} damage taken",
        Levels, Shots, TurretShots, Destroyed, Effects, Duels, DuelShots, DamageTaken);
}
