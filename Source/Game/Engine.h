#pragma once

#include "Game/Collision.h"
#include "Pg/Random.h"

#include <cstdint>
#include <expected>
#include <optional>
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

struct FAmmoStock
{
    std::string Name;
    float Count = 0.f;
    float Max = 0.f;
};

// One weapon's firing state (the weapon instance at object +0x24c).
struct FWeaponState
{
    // Index into FCombat::Weapons.
    size_t Weapon = 0;
    uint32_t LastFireTime = 0;
    // Weapon flag 2 and the slope that WeaponRaiseSlope (0042c210) ramps while it is set.
    bool bTriggerHeld = false;
    float Slope = 0.f;
    // Weapon flag 1, flipped on every shot of an Alternated-fire weapon.
    bool bAlternateSide = false;
};

// A Fixed Seeker turret: the TYPE Seeker block, its scan and lock state, and the rotating top that aims its weapon.
struct FTurret
{
    int VisionDirections = 0;
    float SensorRange = 0.f;
    int32_t ScanDelay = 0;
    float MinDistance = 0.f;
    int32_t TurnTimeout = 0;
    int32_t ScanTimer = 0;
    bool bLocked = false;
    // The target's world position when it was locked; the lock drops once the target has moved at the next scan.
    FCollisionPoint LockedTarget;
    // Direction of the line from the turret to the target, refreshed while engaging.
    int AimDirection = 0;
    // The fixed base's facing, which centres the vision cone, and the top's facing and turn timer.
    int BaseDirection = 0;
    int Direction = 0;
    int32_t TurnTimer = 0;
    FWeaponState Weapon;
    FAmmoStock Ammo;
    // Object flag 2: pull the trigger in this step's weapon update.
    bool bTrigger = false;
};

struct FExplosionBlock
{
    std::string BoomEffect;
    float Damage = 0.f;
    float Zone = 0.f;
};

struct FAbsorption
{
    std::string AmmoClass;
    float Coeff = 1.f;
};

// A placed object that takes part in collisions (Bounding-Box-Size above zero), with its own inertia once pushed.
struct FObjectBody
{
    size_t ObjectIndex = 0;
    FMovementClass Class;
    FPlayerState State;
    // Damage state as ApplyDamage (0043ca30) changes it.
    float Energy = 0.f;
    float Shield = 0.f;
    float Reward = 0.f;
    bool bIndestructible = false;
    bool bDamaged = false;
    std::vector<FAbsorption> Absorptions;
    // Every TYPE Explosion block: the Boom-fx objects spawned on destruction and the blasts they carry.
    std::vector<FExplosionBlock> Explosions;
    float Altitude = 0.f;
    std::optional<FTurret> Turret;
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

// The TYPE Weapon block of a weapon class (weapon.3dc), as the class loader stores it.
struct FWeaponClass
{
    std::string Name;
    std::string Ammo;
    uint32_t Delay = 0;
    float FireRange = 0.f;
    float WeaponsGap = 0.f;
    bool bLeftSide = false;
    bool bRightSide = false;
    bool bAlternated = false;
    int StartDistance = 0;
    int FireAltitude = 0;
    // Radians; LoadObjectClassLibrary converts the degrees in the file.
    float InitialSlope = 0.f;
    float SlopeStep = 0.f;
};

// An ammo class: its movement and the TYPE Ammo block.
struct FAmmoClass
{
    std::string Name;
    FMovementClass Movement;
    float Damage = 0.f;
    float DamageZone = 0.f;
    uint32_t DamageAfter = 0;
    std::string HitEffect;
};

// An FX class (fx.3dc) and the animation it plays.
struct FEffectClass
{
    std::string Name;
    std::string GafPath;
    int GafIndex = 0;
    // Frame durations in display frame periods.
    std::vector<int32_t> FrameDurations;
    uint32_t LoopStart = 0;
    bool bLoop = false;
};

// A running FX object; an explosion's blast goes off once with it.
struct FEffect
{
    size_t ClassIndex = 0;
    FCollisionPoint Position;
    float Altitude = 0.f;
    // 1-based animation frame; zero once a one-shot animation has ended.
    uint32_t Frame = 0;
    int32_t FrameTimer = 0;
    bool bFinished = false;
    float Damage = 0.f;
    float Zone = 0.f;
    int32_t BlastTimer = 0;
};

// A fired ammo object (kind Ammo); bullets with a slope sink as they fly until they reach the ground.
struct FProjectile
{
    size_t AmmoIndex = 0;
    FPlayerState State;
    int Altitude = 0;
    float AltitudeAccumulator = 0.f;
    float Slope = 0.f;
    bool bSloped = false;
    uint32_t DamageTimer = 0;
    // Level object index of the shooter, which its own shots pass through.
    size_t Owner = 0;
};

// The player's weapons (ADD Weapon blocks), the shots in flight and the game clock and random generator they use.
struct FCombat
{
    std::vector<FWeaponClass> Weapons;
    std::vector<FAmmoClass> AmmoClasses;
    // The player's ADD Weapon entries, as indices into Weapons, and the one in hand.
    std::vector<size_t> PlayerWeapons;
    FWeaponState PlayerWeapon;
    std::vector<FProjectile> Projectiles;
    std::vector<FEffectClass> EffectClasses;
    std::vector<FEffect> Effects;
    // g_FramePeriodMs: animation frame durations count display periods.
    uint32_t FramePeriodMilliseconds = 0;
    FRandom Random;
    // The game clock in milliseconds; never zero, since a zero last-shot time means no shot yet.
    uint32_t Clock = 1;
};

// The player as a target of shots and blasts.
struct FPlayerTarget
{
    const FPlayerState& State;
    float Radius = 0.f;
    FPlayerInventory& Inventory;
    size_t ObjectIndex = 0;
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
// Loads the player's weapons, arms the level's turrets and loads every FX class the level's ammo and explosions can spawn.
std::expected<FCombat, std::string> CreateCombat(const FLevel& Level, const FLevelObject& Player, FResourceArchive& Archive, uint32_t FramePeriod,
    std::vector<FObjectBody>& Bodies);
// One step of the player's weapon: the slope ramp, then a shot when the trigger is held and the weapon's delay has passed (WeaponFire at 00443bd0).
// SpriteSize is the smaller side of the player's current frame, which picks the random muzzle distance.
void StepPlayerWeapon(FCombat& Combat, bool bFire, const FPlayerState& Player, const FLevelObject& PlayerObject, size_t PlayerIndex, float SpriteSize,
    FPlayerInventory& Inventory);
// The seeker scan, top rotation and fire decision of every turret (SeekerUpdate at 0042c5a0), then its weapon update.
void StepTurrets(FCombat& Combat, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerState& Player);
// Moves every shot, detonating it against terrain, bodies or the ground, then runs the effects and their blasts; destroyed bodies are removed,
// spawn their Boom-fx and pay their Reward.
void StepProjectiles(FCombat& Combat, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerTarget& Player);
std::expected<void, std::string> VerifyPlayerMovement();
// Loads every level's weapons and fires them for a few seconds of game time.
std::expected<std::string, std::string> VerifyCombat(FResourceArchive& Archive);
// Replays the CAVE\DEMO.3DR inputs through StepPlayer and compares every step with the recorded position.
std::expected<std::string, std::string> VerifyDemoReplay(FResourceArchive& Archive);
