#pragma once

#include "Game/Collision.h"

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

class FResourceArchive;
struct FLevel;
struct FLevelObject;

// Input bits as RecordInput (004290e0) logs them and .3DR records store them.
enum EPlayerInput : uint32_t
{
    PlayerInputFire = 0x02,
    PlayerInputForward = 0x04,
    PlayerInputBackward = 0x08,
    PlayerInputTurnLeft = 0x10,
    PlayerInputTurnRight = 0x20,
    PlayerInputStrafeLeft = 0x40,
    PlayerInputStrafeRight = 0x80,
};

enum class EPlayerTurn : uint8_t
{
    None,
    // Original command 0xcb: next node of the direction ring, counter-clockwise on screen.
    Left,
    // Original command 0xcd: previous node of the direction ring.
    Right,
};

enum class EPlayerThrust : uint8_t
{
    None,
    // Original command 200.
    Forward,
    // Original command 0xd0.
    Backward,
};

struct FPlayerCommand
{
    EPlayerTurn Turn = EPlayerTurn::None;
    EPlayerThrust Thrust = EPlayerThrust::None;
    float Strafe = 0.f;
};

// Movement properties as ObjectInitializeClassProperties (00419d30) copies them from the class record.
struct FMovementClass
{
    float ThrustInitial = 0.f;
    float ThrustMin = 0.f;
    float ThrustMax = 0.f;
    float Weight = 1.f;
    float AccelerationStep = 0.f;
    float DecelerationStep = 0.f;
    float SpeedMax = 0.f;
    float Elasticity = 1.f;
    float Grip = 1.f;
    FCollisionPoint Friction{ .X = 1.f, .Y = 1.f };
    FCollisionPoint FrictionThreshold;
    int TurnTimeout = 0;
    bool bUseInertia = false;
    float Radius = DefaultCollisionRadius;
};

// The player object's movement fields; velocities are in collision units per 14 ms.
struct FPlayerState
{
    FCollisionPoint Position;
    FCollisionPoint PreviousPosition;
    FCollisionPoint Center;
    FCollisionPoint Velocity;
    float Speed = 0.f;
    float SpeedMagnitude = 0.f;
    int Direction = 0;
    // 1-based body pose; turning steps through the in-between pose before the next direction.
    int Pose = 1;
    int TargetPose = 1;
    int TurnTimer = 0;
    // Object flag 0x100: set on objects that were hit, so their velocity carries them until it drops below 1.
    bool bPushed = false;
};

// A placed object that takes part in collisions (Bounding-Box-Size above zero), with its own inertia once pushed.
struct FObjectBody
{
    size_t ObjectIndex = 0;
    FMovementClass Class;
    FPlayerState State;
};

inline constexpr int PlayerPoseCount = 48;

// A collectable item: an indestructible Item-kind object with a TYPE Item block.
struct FPickup
{
    size_t ObjectIndex = 0;
    FCollisionCircle Circle;
    float AddEnergy = 0.f;
    float AddShield = 0.f;
    float AddMoney = 0.f;
    std::string AmmoType;
    float AddAmmo = 0.f;
};

struct FAmmoStock
{
    std::string Name;
    float Count = 0.f;
    float Max = 0.f;
};

// The player's Energy, Shield, money and ammo, as the item pickup branch of EngineUpdateObjects (004406e0) changes them.
struct FPlayerInventory
{
    float Energy = 0.f;
    float EnergyMax = 0.f;
    float Shield = 0.f;
    float ShieldMax = 0.f;
    float Money = 0.f;
    std::vector<FAmmoStock> Ammo;
};

// Live keyboard semantics of ReadPlayerInput (00438540): turning and strafing combine.
FPlayerCommand PlayerCommandFromKeys(bool bForward, bool bBackward, bool bTurnLeft, bool bTurnRight, bool bStrafeLeft, bool bStrafeRight);
// Demo semantics of ReplayPlayerInput (00438800): strafing cancels turning.
FPlayerCommand PlayerCommandFromInput(uint32_t InputBits);

std::expected<FMovementClass, std::string> ReadMovementClass(const FLevelObject& Object);
FPlayerState SpawnPlayer(const FLevelObject& Object, const FMovementClass& Class);
// Every collidable object except the player; classes without movement properties keep zeroed fields like the original class record.
std::vector<FObjectBody> CreateObjectBodies(const FLevel& Level);
std::vector<FPickup> CreatePickups(const FLevel& Level);
FPlayerInventory CreateInventory(const FLevelObject& Player);
// Collects every pickup the player's circle touches; returns how many were taken and marks them removed in the level.
size_t CollectPickups(const FPlayerState& Player, const FMovementClass& Class, FPlayerInventory& Inventory, std::vector<FPickup>& Pickups, FLevel& Level);
// One game step of the player's part of EngineUpdateObjects (004406e0). Bodies are the other collidable objects; Self skips one of them.
void StepPlayer(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, uint32_t StepMilliseconds, const FTerrainCollision& Terrain,
    const FLevel& Level, std::span<FObjectBody> Bodies = {}, const FObjectBody* Self = nullptr);
// Moves pushed bodies for one step, in object order.
void StepObjectBodies(std::span<FObjectBody> Bodies, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level);
std::expected<void, std::string> VerifyPlayerMovement();
// Replays the CAVE\DEMO.3DR inputs through StepPlayer and compares every step with the recorded position.
std::expected<std::string, std::string> VerifyDemoReplay(FResourceArchive& Archive);
