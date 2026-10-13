#pragma once

#include "Game/Collision.h"
#include "Game/Message.h"
#include "Game/Path.h"
#include "Pg/Gaf.h"
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

// UpdateAnimation at 0045e7e0 holds each frame for its duration in ticks; VzRun fixes the tick at 14 ms through AnimationSetFixedPeriod
// (0045e4c0), so the display period never applies.
inline constexpr int32_t AnimationPeriodMilliseconds = 14;

// __ftol truncates into 64 bits and its callers keep the low half; NaN and out-of-range values give the integer indefinite, whose low half is zero.
constexpr int32_t TruncateLow(const double Value)
{
    constexpr double Limit = 9223372036854775808.0;
    return Value > -Limit && Value < Limit ? int32_t(uint32_t(uint64_t(int64_t(Value)))) : 0;
}

// An animation instance's frame and timer, as StartAnimation (0045e770) resets them and UpdateAnimation (0045e7e0) advances them.
struct FAnimationPlayback
{
    // 1-based.
    uint32_t Frame = 0;
    int32_t Timer = 0;
    bool bEnded = false;
};

// UpdateAnimation at 0045e7e0. Object animations neither run backwards nor play back and forth, so only looping is handled.
void UpdateAnimation(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, int32_t Milliseconds);
FAnimationPlayback StartAnimation(const Gaf::FAnimation& Animation);
// ObjectDraw at 00431ae0 shows a chosen frame by stepping back to the one before it and replaying the time left, which lands on it.
void ShowFrame(FAnimationPlayback& Playback, const Gaf::FAnimation& Animation, uint32_t Frame);

// The grid cells that the object passes of EngineRunLevel (EngineVisitViewObjects at 00433b30) visit around the view, as half-open ranges.
struct FViewCells
{
    bool Contains(FCollisionPoint World) const;

    int Left = 0;
    int Top = 0;
    int Right = 0;
    int Bottom = 0;
};

// The vertical bob of an object (object +0x174 to +0x184), from its class's Bounce-period, Bounce-amplitude and Bounce-average-altitude.
struct FObjectBob
{
    float Period = 0.f;
    float Amplitude = 0.f;
    // Radians, drawn once per object when the level starts.
    float Phase = 0.f;
    // Milliseconds of bob played; it winds back while a static object without Vertical-bounce-when-static settles.
    int32_t Timer = 0;
    int Average = 0;
    bool bWhenStatic = false;
    bool bWhenMoving = false;
};

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
    // Collision-space pull back onto a patrol or random route segment (object +0x134, converted from world space).
    FCollisionPoint Correction;
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
    // Walkers and spiders (kinds 0x19 and 0x1f): thrust is not divided by Weight (ObjectComputeDisplacement at 00438b20).
    bool bThrustIgnoresWeight = false;
    // Walkers, spiders and tanks keep no velocity after a free move (EngineUpdateObjects at 004406e0).
    bool bDropsVelocity = false;
    // Class property Over-water: water edges do not stop the body.
    bool bOverWater = false;
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
    int32_t SearchTimer = 0;
    bool bSearching = false;
    FPathSearch Search;
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

// A seeker's or mover's work attribute (kind 6 or 7): its scan timer, pending route search and the route segment it follows.
struct FRouteWork
{
    // Scan-delay: the timer refills by it when a plan starts and again when its search ends.
    int32_t Interval = 0;
    // SeekerRangeState radius: 64 for patrols, Min-distance for seekers.
    uint32_t Radius = 0;
    int32_t Timer = 0;
    int32_t SearchTimer = 0;
    bool bSearching = false;
    bool bOnSegment = false;
    FPathSearch Search;
    std::optional<FPathRoute> Route;
    // The segment being followed: route points Route->Current - 1 to Route->Current.
    FCollisionPoint SegmentStart;
    FCollisionPoint SegmentEnd;
    FCollisionPoint SegmentDelta;
    float SegmentLengthSquared = 0.f;
    int SegmentDirection = 0;
    // SeekerRangeState bits from the last engage: 1 near the segment end, 2 close, 4 within firing distance of the target.
    uint32_t RangeState = 0;
};

// The TYPE Patrol attribute (kind 0xe) beside the work attribute that PatrolMoveUpdate (0042e400) drives.
struct FPatrol
{
    // Index into FLevel::PatrolPaths; FUN_0042e910 takes the last path with the attribute's Path-ID.
    std::optional<size_t> Path;
    int Point = -1;
    bool bReverse = false;
    bool bClosed = false;
    bool bStarted = false;
    FCollisionPoint Goal;
    FRouteWork Work;
};

// The TYPE Random attribute (kind 0x14) beside the work attribute that RandomMoveUpdate (0042dd20) drives.
struct FRandomMove
{
    // Range limits both the route search and the window of cells a destination is drawn from; zero means the whole map.
    float Range = 0.f;
    // The last destination in world pixels; a route that did not reach its last segment retries it once (FUN_0042e040).
    FCollisionPoint Goal;
    int Retries = 0;
    FRouteWork Work;
};

// The TYPE Seeker or TYPE Tracker attribute that SeekerUpdate (0042c5a0) drives on a moving actor; the target is always the player.
struct FSeeker
{
    // Trackers keep searching around detours; seekers give up on them.
    bool bTracker = false;
    int VisionDirections = 0;
    float SensorRange = 0.f;
    bool bHasTarget = false;
    // The target's world position and cell when the route was planned; a moved target ends the route at the next scan.
    FCollisionPoint TargetPosition;
    int TargetCellX = 0;
    int TargetCellY = 0;
    FRouteWork Work;
    // Guardian behaviour (TYPE Guardian, kind 8): the search covers Range in every direction, and without a target the actor returns to Location.
    bool bGuardian = false;
    float GuardRange = 0.f;
    FCollisionPoint GuardLocation;
    std::optional<size_t> Weapon;
    FWeaponState WeaponState;
    FAmmoStock Ammo;
};

struct FActor
{
    uint32_t Behaviour = 0;
    // Class property Can-turn-on-place (class flag 0x20).
    bool bTurnOnPlace = false;
    // Body poses per direction: 2 for cops' 48 poses, 1 for the 24-pose vehicles (SeekerTurnBody, PlayerAdvancePose at 00438390).
    int PosesPerDirection = 2;
    // Two-pieces (class flag 0x20 at +0x20, bit 2): a turret top that aims on its own while the body holds still in firing range.
    bool bTwoPieces = false;
    // Tanks, walkers and spiders scale the segment pull by 0.01 (DAT_005095b8) in ObjectComputeDisplacement.
    float CorrectionScale = 1.f;
    // Spiders (kind 0x1f) stop only at every sixth pose, drive only when stopped there, take only straight axis routes to a target and never
    // thrust for their seeker.
    bool bSpider = false;
    int TopPose = 1;
    int TopTargetPose = 1;
    int32_t TopTurnTimer = 0;
    int32_t TopTurnTimeout = 0;
    // Object +0x13c: bit 1 follows a route, bit 2 lets the movement behaviour run beside the seeker.
    uint32_t RouteFlags = 0;
    // Object +0x194: the behaviour that ran last (1 seeker, 2 tracker, 0xc patrol); only patrol and random movers get the segment pull.
    uint32_t Mode = 0;
    std::optional<FSeeker> Seeker;
    std::optional<FPatrol> Patrol;
    std::optional<FRandomMove> RandomMove;
    FPlayerCommand Command;
    // Object flag 2: pull the trigger in this step's weapon update.
    bool bTrigger = false;
    // A heli's tilt counter (object +0x21c), which HeliSelectBodyPose (004334e0) moves toward its speed band in every object pass.
    int Tilt = 0;
};

// HeliSelectBodyPose at 004334e0: one tilt step toward the band for the current speed, then the tilt animation (0 to 3) to draw.
int SelectHeliTilt(FActor& Actor, float SpeedMagnitude);

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
    // Object +0x5c bit 1: the body's collision node is disabled, as for an open gate or a present lift.
    bool bPassable = false;
    std::optional<FTurret> Turret;
    std::optional<FActor> Actor;
};

inline constexpr int PlayerPoseCount = 48;

// A carried TYPE Key (attribute kind 5): a key, or one part of a code that only counts once every part is carried.
struct FCarriedKey
{
    int Id = 0;
    bool bCode = false;
    int PartId = 0;
    int PartCount = 0;
};

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
    std::optional<FCarriedKey> Key;
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
    // Carried key items in pickup order (object +0x11c lists them newest first).
    std::vector<FCarriedKey> Keys;
    // Object +0x26c: the objects the player's shots finished off.
    int Kills = 0;
};

// TYPE Gate and TYPE Lift flags (attribute +0x10) as LoadObjectClassLibrary packs them.
enum EDoorFlag : uint32_t
{
    DoorActive = 0x1,
    // Gate Opened, lift Present.
    DoorOpen = 0x2,
    // Gate Closing, lift Going-Up: set once the animation toward open has started.
    DoorOpening = 0x4,
    // Stay-Opened, Stay-Up.
    DoorStayOpen = 0x8,
    // Stay-Closed, Stay-Down; a sensing gate also sets it while nothing that may pass is near.
    DoorStayClosed = 0x10,
    // Gate Sensor: the gate opens only for something near.
    DoorSensor = 0x20,
    DoorKeyAlwaysNeeded = 0x40,
    DoorDestroyKey = 0x80,
    DoorNeedsCode = 0x100,
};

// A gate (TYPE Gate, attribute kind 4) or a lift (TYPE Lift, kind 3). The object's first animation leads to open or present, its second
// to closed or away; each plays once and holds its last frame.
struct FDoor
{
    size_t ObjectIndex = 0;
    bool bLift = false;
    uint32_t Flags = 0;
    int32_t Delay = 0;
    int32_t Timer = 0;
    // The key or code a holder must carry; zero lets anything through.
    int Id = 0;
    int UsageCount = 0;
    // A lift's 3do-file: the level it leads to, or a starred marker such as *End*; Scenery names the next level's zone when it changes.
    std::string NextLevel;
    std::string Scenery;
    std::array<Gaf::FAnimation, 2> Animations;
    size_t Animation = 0;
    FAnimationPlayback Playback;
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
    // Frame durations in animation ticks.
    std::vector<int32_t> FrameDurations;
    uint32_t LoopStart = 0;
    bool bLoop = false;
    // FX stretch-ratio; ObjectDraw at 00431ae0 treats zero as no stretch.
    float Stretch = 1.f;
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
    FRandom Random;
    // Every level object's bob, by object index.
    std::vector<FObjectBob> Bobs;
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
// Behaviour bits as ParseBehaviour (00409490) packs them into the object at +0x190.
enum EBehaviour : uint32_t
{
    BehaviourTargetMask = 0x03,
    BehaviourSeeker = 0x01,
    BehaviourTracker = 0x02,
    BehaviourPlayer = 0x03,
    BehaviourMovementMask = 0x1c,
    BehaviourRandom = 0x04,
    BehaviourFixed = 0x08,
    BehaviourPatrol = 0x0c,
    BehaviourStraight = 0x10,
    BehaviourGuardian = 0x20,
};

// Nullopt for the combinations the original rejects: a second movement, Seeker/Tracker/Guardian out of order, or an unknown word.
std::optional<uint32_t> ParseBehaviour(std::string_view Text);

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
size_t CollectPickups(const FPlayerState& Player, const FMovementClass& Class, FPlayerInventory& Inventory, std::vector<FPickup>& Pickups, FLevel& Level,
    FMessages& Messages);
// One game step of the player's part of EngineUpdateObjects (004406e0). Bodies are the other collidable objects; Self skips one of them. Returns
// the body the move ran into, if any.
const FObjectBody* StepPlayer(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, uint32_t StepMilliseconds, const FTerrainCollision& Terrain,
    const FLevel& Level, std::span<FObjectBody> Bodies = {}, const FObjectBody* Self = nullptr);
// Moves pushed bodies for one step, in object order.
// Actors look for the player, patrol, chase and shoot, then move like the player with the resulting command. Without combat they only patrol.
void StepActors(FCombat* Combat, std::span<FObjectBody> Bodies, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level,
    const FPlayerState* Player);
void StepObjectBodies(std::span<FObjectBody> Bodies, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level);
// Every gate and lift of the level, in their activation state (AnimInitializeGraphics at 00417b30): open ones are passable and show the
// last frame of their first animation.
std::expected<std::vector<FDoor>, std::string> CreateDoors(const FLevel& Level, FResourceArchive& Archive, std::span<FObjectBody> Bodies);
// One step of every gate (GateUpdate at 00443620) and lift (ObjectUpdateLevelExit at 00442bf0) after its animation advances. Returns the
// lift the player has taken, if any.
const FDoor* StepDoors(std::vector<FDoor>& Doors, const FLevel& Level, std::span<FObjectBody> Bodies, const FPlayerTarget& Player,
    uint32_t StepMilliseconds, FMessages& Messages);
// The player's contact with a closed lift or gate tells it what the door is waiting for.
void ReportDoorContact(std::span<const FDoor> Doors, const FObjectBody& Body, const FPlayerInventory& Inventory, FMessages& Messages);
// ObjectUpdateLevelExit names the next level within the current zone, or within the lift's Scenery when the zone changes.
std::string NextLevelPath(std::string_view CurrentPath, const FDoor& Lift);
// Activates the level's objects, which draws each bob's phase and sets its first altitude, then loads the player's weapons, arms the level's
// turrets and loads every FX class the level's ammo and explosions can spawn.
std::expected<FCombat, std::string> CreateCombat(FLevel& Level, const FLevelObject& Player, FResourceArchive& Archive, std::vector<FObjectBody>& Bodies);
// The bob part of the per-step update pass: every object in the view's cells bobs as ObjectSelectBobMode (00433f40) decides, which changes the altitude its
// shots start from.
void StepObjectBobs(FCombat& Combat, FLevel& Level, std::span<FObjectBody> Bodies, uint32_t StepMilliseconds, FViewCells View);
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
    const FPlayerTarget& Player, FMessages& Messages);
std::expected<void, std::string> VerifyPlayerMovement();
// Loads every level's weapons and fires them for a few seconds of game time.
std::expected<std::string, std::string> VerifyCombat(FResourceArchive& Archive);
std::expected<std::string, std::string> VerifyPatrols(FResourceArchive& Archive);
// The level's enemies still alive, as the session counter at +0x2094 keeps them: seekers and trackers that hunt the player.
// ponytail: generators of enemies, which EngineActivateObjects also counts, are not ported.
int CountEnemies(const FLevel& Level, std::span<const FObjectBody> Bodies);
// Every level's gates close with nothing near and open for the player, and the player rides every usable lift.
std::expected<std::string, std::string> VerifyDoors(FResourceArchive& Archive);
// Replays the CAVE\DEMO.3DR inputs through StepPlayer and compares every step with the recorded position.
std::expected<std::string, std::string> VerifyDemoReplay(FResourceArchive& Archive);
