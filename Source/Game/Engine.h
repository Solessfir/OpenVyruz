#pragma once

#include "Game/Animation.h"
#include "Game/Collision.h"
#include "Game/Message.h"
#include "Game/Path.h"
#include "Pg/Gaf.h"
#include "Pg/Random.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

class FResourceArchive;
struct FLevel;
struct FLevelObject;


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
    // Where this step's hard knock struck, in world space, for the bounce spark (FUN_0043d140).
    std::optional<FCollisionPoint> KnockPoint;
    // Object flag 8: seekers and gates do not find the object, and the player takes no damage.
    bool bInvisible = false;
    // Object flag 0x400000, Touch-and-destroy: what the object runs into, or what runs into it, loses its energy unless it is Indestructible.
    bool bTouchAndDestroy = false;
    // Object +0xcc runs: the dead player waits for the game to end, and SeekerFindTarget (0042da80) takes no such object for a target.
    bool bDying = false;
    // Object +0x258: forward thrust sets it to a second and every other step runs it down (EngineUpdateObjects at 004406e0); ObjectDraw shows the player's reactor glow while it lasts.
    uint32_t ReactorMilliseconds = 0;
};

struct FAmmoStock
{
    std::string Name;
    float Count = 0.f;
    float Max = 0.f;
};

// The surprise types of surprise.cpp, in the order of their probability table (DAT_0050d5d0) and of FUN_0041bc10's names.
enum class ESurprise : int32_t
{
    None,
    Random,
    ReverseCommands,
    Trap,
    WeaponJam,
    Darken,
    Extasy,
    LowSpeed,
    Pinball,
    Banana,
    BlockedMotor,
    DestroyAround,
    TouchAndDestroy,
    AutoShieldUp,
    Invisible,
    CircularFire,
    TimeWarp,
    ReloadWeapons,
    Invulnerability,
    TripleFire,
    Nop,
};

// A collected surprise item the player carries (object +0x11c): its delay before it starts and the time it lasts (item attribute +0x2c, +0x30).
struct FCarriedSurprise
{
    ESurprise Type = ESurprise::None;
    int32_t Delay = 0;
    int32_t Timeout = 0;
    bool bStarted = false;
    // The blinking icon message at the screen's corner.
    uint32_t IconMessage = 0;
    // The item the surprise came from, which a session writes before the player.
    std::string ClassName;
    float X = 0.f;
    float Y = 0.f;
};

// What the surprise in effect changes on the player and the session.
struct FSurpriseState
{
    // Newest first, as ConsumeItem prepends them.
    std::vector<FCarriedSurprise> Carried;
    // Object +0xd4: only one surprise acts at a time.
    ESurprise Active = ESurprise::None;
    bool bReversed = false;
    bool bBlockedMotor = false;
    bool bInvulnerable = false;
    bool bTripleFire = false;
    bool bCircularFire = false;
    // Object +0x114 and +0x118: Triple Fire's spread, which opens by 20 a drawn frame up to 400 while the trigger is held and closes by 50 (FUN_00443b80, ObjectDraw).
    int32_t TripleSpread = 0;
    int32_t TripleSpreadStep = 0;
    // Object +0x104: Circular Fire's shots left to turn, 24 when the trigger is pulled.
    int32_t CircularShots = 0;
    // Session +0x21f4 and +0x21f0: Time Warp's divisor and the counter that lets the objects other than the player update once every divisor + 1 steps.
    int32_t WarpDivisor = 0;
    int32_t WarpCounter = 0;
    // Session +0x220c: the weapon that will not fire.
    std::optional<size_t> JammedWeapon;
    // The movement values the surprise replaced (kept at session +0x2208): Extasy and Low Speed change the object's Speed-max (+0x1a4) and Speed-min (+0x1a8), which ObjectInitializeClassProperties (00419d30) copies from class +0x64 and +0x60.
    // ponytail: the port keeps no Speed-min, which nothing it runs reads.
    float SpeedMax = 0.f;
    float Elasticity = 0.f;
    FCollisionPoint Friction;
    // Session +0x25a0: the red, green and blue gamma offsets that flashes push and each frame takes back by one (EngineRunLevel at 0043e5f0).
    std::array<int32_t, 3> Gamma{};
    // Reload-weapons gives back the stocks it found when it ends.
    std::vector<float> SavedAmmo;
    // Invisible: the drone's body stays where it was (session +0x26ac) and the drone flies back to it in the last second (FUN_00445380).
    FCollisionPoint Body;
    int32_t InvisibleTimer = 0;
    std::optional<FCollisionPoint> ReturnStart;
    // Session +0x2200 and +0x2204: the screen shake a trap, Destroy-around or a blast on the player starts.
    int ShakeX = 0;
    int ShakeY = 0;
};

// One weapon's firing state (the weapon instance at object +0x24c).
struct FWeaponState
{
    // Index into FCombat::Weapons.
    size_t Weapon = 0;
    // The object's own copy of the weapon's Delay, which the difficulty's nmy-fire-delay-ratio scales (FUN_0043c4a0); none uses the class's.
    std::optional<uint32_t> Delay;
    uint32_t LastFireTime = 0;
    // Weapon flag 2 and the slope that WeaponRaiseSlope (0042c210) ramps while it is set.
    bool bTriggerHeld = false;
    float Slope = 0.f;
    // Weapon flag 1, flipped on every shot of an Alternated-fire weapon.
    bool bAlternateSide = false;
    // The trigger was pulled since the last frame's weapon sounds, and the owner's object flag 0x20000 (ObjectsUpdateWeaponSounds at 0043df70): its fire sound is under way.
    bool bSoundTrigger = false;
    bool bSoundFiring = false;
    // The port gives each armed object one positional sound at a time (FAudio::UpdateObjectWeapon).
    FSoundTrack SoundTrack;
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
    // The player object searched for or locked, and its world position when it was locked; the lock drops once the target has moved at the next scan.
    std::optional<size_t> TargetObject;
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
    // The player object hunted, the nearest the sensor found when the route was planned.
    size_t TargetObject = 0;
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
    // The class's own Turn-timeout (class +0x7c), from which a body that cannot turn on the spot takes its timeout by its speed.
    int ClassTurnTimeout = 0;
    // Body poses per direction: 2 for cops' 48 poses, 1 for the 24-pose vehicles (SeekerTurnBody, PlayerAdvancePose at 00438390).
    int PosesPerDirection = 2;
    // Two-pieces (class flag 0x20 at +0x20, bit 2): a turret top that aims on its own while the body holds still in firing range.
    bool bTwoPieces = false;
    // Tanks, walkers and spiders scale the segment pull by 0.01 (DAT_005095b8) in ObjectComputeDisplacement.
    float CorrectionScale = 1.f;
    // Spiders (kind 0x1f) stop only at every sixth pose, drive only when stopped there, take only straight axis routes to a target and never thrust for their seeker.
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
    // Object +0x220, the turn command: a behaviour wanted the body to face another pose this step.
    bool bTurnCommand = false;
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

// An ammo object lying in the level, a mine: placed (Mine-Infinite) or dropped by the mine weapon (Mine-Ammo).
// A mover that runs into it sets it off (EngineUpdateObjects at 004406e0 calls AmmoHit), and its on and off timers (object +0xc8 and +0xcc) show and hide it.
struct FMine
{
    // Object +0x2b4: the shooter that dropped it, by object index.
    std::optional<size_t> Owner;
    // The TYPE Ammo damage-after (attribute +0x20): it goes off when this runs out.
    uint32_t DamageTimer = 0;
    // While the off timer runs the mine is left out of its update; once it runs out the mine shows and the on timer is drawn from Random-on-timeout (class +0xb0, +0xb4), and the other way round.
    int32_t OnTimer = 0;
    int32_t OffTimer = 0;
    int32_t OnMin = 0;
    int32_t OnMax = 0;
    int32_t OffMin = 0;
    int32_t OffMax = 0;
    // Keep-bb-when-invisible (class +0x20 bit 1): a hidden mine still collides.
    bool bKeepBoxHidden = false;
    // Set when it goes off on a contact; its update stops and it is destroyed.
    bool bStruck = false;
    // The contacts AmmoHit counts for a mine of the player's, a hit on a hunter and a miss on anything else, which StepProjectiles adds to the player's shots.
    int Hits = 0;
    int Misses = 0;
};

// A placed object that takes part in collisions (Bounding-Box-Size above zero), with its own inertia once pushed.
// A StaticGen's TYPE Generator block (attribute kind 2) and its birth cycle (ObjectUpdateGenerator at 004372a0).
struct FGenerator
{
    // Lower-case class name.
    std::string Product;
    // Negative for no limit.
    int MaxBirths = 0;
    int Births = 0;
    int32_t BirthDelay = 0;
    int32_t BirthDuration = 0;
    int32_t Timer = 0;
    // A damaged generator counts its time down twice as fast.
    bool bStressable = false;
    // Flag 2: the generator has closed for a birth, which comes when the timer runs out. Active sets it from the start.
    bool bBirthing = false;
    // A product's death frees its place for another birth (ObjectDropContents at 00437f80).
    bool bStablePopulation = false;
    // EngineActivateObjects counts a generator of seekers or trackers as an enemy.
    bool bProductHunts = false;
};

// A hunter's values before the level-start difficulty scaled them, so another difficulty can replace it during the level.
struct FDifficultyBase
{
    int32_t ScanInterval = 0;
    float SensorRange = 0.f;
    int32_t TurretScanDelay = 0;
    float TurretSensorRange = 0.f;
    float AppliedShieldUpRatio = 1.f;
};

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
    // Player-direct-fire-only (class flag 0x40 at +0x20): knocks hurt it only when the player is in them.
    bool bPlayerDirectFireOnly = false;
    // Ignore-same-class-objects: it passes through objects of its own class.
    bool bIgnoreSameClass = false;
    // Object flag 8, from the class's Invisible (ObjectInitialize at 00419580): ObjectDraw fades it out.
    // Visible-when-firing (class +0x1c bit 0x400) shows it while it shoots and hides it again otherwise (SeekerEngage at 0042caa0).
    bool bInvisible = false;
    bool bVisibleWhenFiring = false;
    // Final-object (class +0x20 bit 0x20, only Final-PC): its death ends the game (FUN_0043db70).
    bool bFinalObject = false;
    // Object flag 0x800000: ApplyDamage takes nothing, as the Final-PC's orbiting FX switch it.
    bool bInvulnerable = false;
    // The class's Shield.Max and Shield-up-step (object +0xe4): EngineUpdateObjects adds the step to the shield on each update, up to the maximum.
    float ShieldMax = 0.f;
    float ShieldUpStep = 0.f;
    // Set for the hunters the level-start difficulty reached; objects born later keep their class values, as in the original.
    std::optional<FDifficultyBase> DifficultyBase;
    // The class's Shield.Min (class +0x6c), which a shield takes back when it breaks.
    float ShieldMin = 0.f;
    std::vector<FAbsorption> Absorptions;
    // Every TYPE Explosion block: the Boom-fx objects spawned on destruction and the blasts they carry.
    std::vector<FExplosionBlock> Explosions;
    float Altitude = 0.f;
    // Object +0x5c bit 1: the body's collision node is disabled, as for an open gate or a present lift.
    bool bPassable = false;
    std::optional<FTurret> Turret;
    std::optional<FActor> Actor;
    std::optional<FGenerator> Generator;
    // Object +0x2b4: the generator that made this object, by object index.
    std::optional<size_t> Parent;
    std::optional<FMine> Mine;
};

inline constexpr int PlayerPoseCount = 48;

// A carried TYPE Key (attribute kind 5): a key, or one part of a code that only counts once every part is carried.
struct FCarriedKey
{
    int Id = 0;
    bool bCode = false;
    int PartId = 0;
    int PartCount = 0;
    // The part's Code-String (key attribute +0x20), which the inventory lists.
    std::string CodeString;
    // The level object the key was picked up from, which a code's expiry puts back.
    size_t ObjectIndex = 0;
    // The TYPE Item's timeout fields (item attribute +0x28 to +0x7c): a complete code counts down on its first part.
    bool bHasTimeout = false;
    int32_t Timeout = 0;
    int32_t WarnTimeout = 0;
    std::string WarnFormat;
    int32_t WarnX = 0;
    int32_t WarnY = 0;
    int32_t ReactivateAfter = 0;
    // The item's class and where it lay, which a session writes before the player as the object it carries.
    std::string ClassName;
    float X = 0.f;
    float Y = 0.f;
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
    // The TYPE Item's Surprise-type.
    ESurprise Surprise = ESurprise::None;
    // The port's co-op: the player objects that took a pack every player has their own of, which stays in the level until each has.
    std::vector<size_t> Takers;
};

// The player's part of the combat: the weapon in hand and each slot's trigger state, the lock warning and the fast-kill window.
struct FPlayerArms
{
    FWeaponState Weapon;
    // Session +0x1028: the 1-based weapon slot in hand; a slot past the player's weapons leaves it unarmed.
    // Each ADD Weapon keeps its own trigger state, which SlotStates holds for the slots not in hand.
    size_t Slot = 1;
    std::vector<FWeaponState> SlotStates;
    // Counts PlayerSelectWeapon's switches, each of which plays weapon-switch.wav.
    uint32_t Switches = 0;
    // Session +0x20 bit 0x200000: EngineUpdateObjects clears it, and SeekerUpdate sets it for a seeker engaging the player.
    bool bLocked = false;
    // Session +0x274c and +0x2750: the fast-kill window and the hunters the player's blasts finished off in it.
    int32_t FastKillTimer = 0;
    int FastKillCount = 0;
};

// The player's Energy, Shield, money and ammo, as the item pickup branch of EngineUpdateObjects (004406e0) changes them.
struct FPlayerInventory
{
    // Object flag 0x200: AreaDamage (0043cb20) marks the player it damages, and ObjectDraw turns the mark into a red glow.
    bool bHit = false;
    float Energy = 0.f;
    float EnergyMax = 0.f;
    float Shield = 0.f;
    float ShieldMax = 0.f;
    // Shield.Min (class +0x6c), which the shield takes back when it breaks.
    float ShieldMin = 0.f;
    float Money = 0.f;
    std::vector<FAmmoStock> Ammo;
    // Carried key items in pickup order (object +0x11c lists them newest first).
    std::vector<FCarriedKey> Keys;
    // Object +0x26c: the objects the player's shots finished off.
    int Kills = 0;
    // Object +0x270, every cash the player has been paid, which spending leaves alone; the score's Money.
    float Earned = 0.f;
    // Session +0x26e0 and +0x26e4: AmmoHit counts each of the player's shots that struck a hunter or an item, and every other one.
    int ShotsHit = 0;
    int ShotsMissed = 0;
    FSurpriseState Surprises;
    // The player class's TYPE Absorption blocks and Weight, which AmmoImpact (0043d350) uses on the player as on any object.
    std::vector<FAbsorption> Absorptions;
    float Weight = 1.f;
    // The shoves of this step's hits, for the caller to add to the player's velocity.
    FCollisionPoint Shove;
    // The player's Shield-up-step (object +0xe4), which the difficulty scales and Auto Shield Up raises by 10.
    float ShieldUpStep = 0.f;
    // The maxima as the level started, before the difficulty, and the shield-up ratio applied since, so another difficulty can replace it during the level.
    float BaseShieldMax = 0.f;
    float BaseEnergyMax = 0.f;
    float AppliedShieldUpRatio = 1.f;
    // Each level starts it from the combat's loadout (FCombat::StartArms).
    FPlayerArms Arms;
};

// A difficulty level's values (DAT_0050d328, ten per level), which COMMON\DIFFICULTY.3DC overrides; ConfigLoad's difficulty-level picks one and copies it to session +0x26b8.
// The mine count and item value ratios are loaded but read nowhere.
struct FDifficulty
{
    float PlayerShieldMax = 10000.f;
    float PlayerEnergyMax = 10000.f;
    float ScanDelayRatio = 1.f;
    float ScanRangeRatio = 1.f;
    float EnemyShieldUpStepRatio = 1.f;
    float PlayerShieldUpStepRatio = 1.f;
    float MineRandomCountRatio = 1.f;
    float EnemyFireDelayRatio = 1.f;
    float ShieldUpItemValueRatio = 1.f;
    float EnergyUpItemValueRatio = 1.f;
};

inline constexpr size_t DifficultyLevelCount = 4;
// The table DIFFICULTY.3DC has been read into, and its default level (the file's last difficulty-level, Normal).
struct FDifficultyTable
{
    std::array<FDifficulty, DifficultyLevelCount> Levels;
    size_t DefaultLevel = 1;
};

struct FCombat;

// The exe's own table, then the file's values over it.
FDifficultyTable LoadDifficulties(std::string_view Text);
// FUN_0043c4a0, which EngineActivateObjects runs on the player and on every hunter as a level starts.
// bLevelStart records what the difficulty scales; a later call, after the difficulty changed during the level, works from those records instead.
void ApplyDifficulty(const FDifficulty& Difficulty, const FCombat& Combat, std::span<FObjectBody> Bodies, FPlayerInventory& Inventory, bool bLevelStart);
// EngineUpdateObjects adds each fighting object's Shield-up-step to its shield, and the player's to its own, every update.
void StepShields(std::span<FObjectBody> Bodies, const FLevel& Level, FPlayerInventory& Inventory);

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

// A gate (TYPE Gate, attribute kind 4) or a lift (TYPE Lift, kind 3).
// The object's first animation leads to open or present, its second to closed or away; each plays once and holds its last frame.
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
    // Where its animation sounds play from, with its class's sample channels. ponytail: the original advances these Graphic-update-needed animations in the object update, outside ObjectDraw's position copy; whether their instance position lags behind the object is not checked in the running game.
    FSoundEmitter Emitter;
};

// The TYPE Weapon block of a weapon class (weapon.3dc), as the class loader stores it.
struct FWeaponClass
{
    std::string Name;
    // The class as the class library spells it ("Gatling-II"), which the inventory prints.
    std::string ClassName;
    // The first TYPE Weapon block's ammo, whose block also gives the weapon's other fields (ObjectSetWeaponSlot at 004375f0 keeps the first block).
    std::string Ammo;
    // Every TYPE Weapon block's ammo in order: the player's stock binds to any of them (ObjectBindAmmo at 00437540).
    std::vector<std::string> AmmoChoices;
    uint32_t Delay = 0;
    float FireRange = 0.f;
    float WeaponsGap = 0.f;
    bool bLeftSide = false;
    bool bRightSide = false;
    bool bAlternated = false;
    int StartDistance = 0;
    int FireAltitude = 0;
    // Smoke-fx (block +0x14), the puff WeaponFire (00443bd0) leaves at the muzzle with every shot, whose animation also carries the launch's whoosh, and Smoke-altitude (+0x28) above the shooter's.
    std::string SmokeEffect;
    int SmokeAltitude = 0;
    // Radians; LoadObjectClassLibrary converts the degrees in the file.
    float InitialSlope = 0.f;
    float SlopeStep = 0.f;
    // The TYPE Weapon block's sounds (keywords 0x98 to 0x9c, block +0x34 to +0x40), file names in Data\Sound, and Fire-sound-loop (flag 0x10).
    std::string FireSound;
    std::string FireStartSound;
    std::string FireEndSound;
    std::string NoAmmoSound;
    bool bFireSoundLoops = false;
    // The weapon class's Sample-channel-range and Sample-channel-priority, which other objects' weapon sounds play on.
    uint32_t SoundChannelMask = 0;
    uint32_t SoundPriority = 0;
};

// A CREATE block of an ammo class: ObjectUpdateCreates (00437b00) makes an object of class Name at Offset from the shot once it has lived Delay milliseconds, then again every Repeat (never again without one).
struct FCreateBlock
{
    std::string Name;
    FCollisionPoint Offset;
    uint32_t Delay = 0;
    uint32_t Repeat = 0;
    // FUN_00413740 launches the product: with Random-dir (keyword 0xec, flag 2) along a direction drawn from the Misc stream, at its class's initial thrust,
    // or with Set-speed (0xed, flag 4) at the block's Speed (0xea), which also becomes its Speed-max.
    bool bRandomDirection = false;
    bool bSetSpeed = false;
    float Speed = 0.f;
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
    // Spark-on-hit-point (class flag 0x20000000) with its Bounce-fx classes (class +0xd0 to +0xdc: up-left, up-right, down-left, down-right), which a hard bounce throws off; empty without the flag.
    std::array<std::string, 4> BounceEffects;
    // The damage and area weapon selection lists (FUN_00437740): the TYPE Ammo block's, or the largest of the class's Ammo and Explosion blocks when either is zero.
    int ListedDamage = 0;
    int ListedArea = 0;
    // Usable-when-invisible (the ammo attribute's flag 8): only such ammo fires while the player is invisible.
    bool bUsableWhenInvisible = false;
    // On-object-hit without On-hit (the ammo attribute's flags 2 and 1): grenades, saws and pulsars burst on objects but bounce off the terrain.
    bool bBouncesOffTerrain = false;
    // Launched (object flag 0x8000) with Use-inertia: ObjectComputeDisplacement (00438b20) drops the shot's command, so it coasts on its launch speed and slows by its friction (grenades, saws, flames).
    // Without Use-inertia the original clears the flag on the first move.
    bool bLaunched = false;
    // Vertical-bounce (object flag 0x1000): the shot hops along the ground, as grenades and bouncing pulsars do (EngineUpdateObjects at 004406e0), with its class's Bounce-period.
    bool bVerticalBounce = false;
    float BouncePeriod = 0.f;
    // The class's Energy, which hard bounces wear down, and Indestructible (class flag 0x40 at +0x20), which spares it.
    float Energy = 0.f;
    bool bIndestructible = false;
    // Objects the shot leaves behind as it flies.
    std::vector<FCreateBlock> Creates;
    // The class's TYPE Explosion blocks, which ObjectDestroy (00437d40) sets off when the shot goes.
    std::vector<FExplosionBlock> Explosions;
    // Behaviour Fixed: the shot is dropped as a mine, a level object, instead of flying.
    bool bDropped = false;
    // The TYPE Ammo active-after (attribute +0x1c): a dropped mine lies visible but passable this long before it arms (WeaponFire at 00443bd0).
    int32_t ActiveAfter = 0;
};

// A TYPE FX check-event: what keeps an orbiting FX on its host (FUN_0041c0b0), and what its on and off timers switch on the host (FUN_004452c0).
enum class EFxEvent : uint8_t
{
    None,
    // bonus-invulnerability (0xc9): the host's object flag 0x800000.
    Invulnerability,
    // bonus-touch-and-destroy (0xcd): object flag 0x400000.
    TouchAndDestroy,
    // malus-banana (0xcf): Banana in effect on the host.
    Banana,
    // do-fx (0x10d): always, as on the scouts' stars.
    Always,
};

// An FX class (fx.3dc) and the animation it plays.
struct FEffectClass
{
    std::string Name;
    std::string GafPath;
    int GafIndex = 0;
    // Uniq-at-xy (keyword 0x47, class flag 0x1000000): an impact adds none where one of the class already stands (FUN_0043dc40).
    bool bUniqAtXy = false;
    Gaf::FAnimation Animation;
    // The class's sample channels and priority; each effect plays its sounds from where it is.
    uint32_t ChannelMask = 0;
    uint32_t Priority = 0;
    uint32_t LoopStart = 0;
    bool bLoop = false;
    // FX stretch-ratio; ObjectDraw at 00431ae0 treats zero as no stretch.
    float Stretch = 1.f;
    // The orbit of TYPE FX (attribute kind 0xf): Count copies, Phase apart, circle the host Ray away (circular) at Angular-speed radians a millisecond of game clock,
    // and bob Vertical-amplitude at Vertical-pulsation; on-timeout and off-timeout switch the check-event on the host and off again.
    int Count = 0;
    bool bCircular = false;
    float AngularSpeed = 0.f;
    float Ray = 0.f;
    float Phase = 0.f;
    float VerticalAmplitude = 0.f;
    float VerticalPulsation = 0.f;
    float VerticalPhase = 0.f;
    EFxEvent Event = EFxEvent::None;
    int32_t OnTimeout = 0;
    int32_t OffTimeout = 0;
    int Altitude = 0;
    std::vector<FCreateBlock> Creates;
};

// A map animation: FUN_00433930, at the end of LevelStartMapAnimations (0041f0b0), makes a decor object (category 4) for each slot of each .3DI cell whose descriptor is a loaded Anim one, column by column, and adds it to the cell's object list.
struct FDecorAnimation
{
    // World position: the cell's top left corner.
    float X = 0.f;
    float Y = 0.f;
    std::string GafPath;
    // 0-based animation of the GAF.
    size_t Animation = 0;
    // From the descriptor's terrain type (object +4): EngineVisitViewObjects sorts the frame a row lower for walls and obstacles and two rows lower for roofs, in world pixels.
    int32_t DepthBias = 0;
    // Object +0x114: ObjectInitialize (00419580) draws RandomBelow(10000), and EngineVisitViewObjects (00433b30) steps the animation only once the clock (session +0x1024) has passed it.
    uint32_t StartMilliseconds = 0;
    FAnimationPlayback Playback;
};

// LevelStartMapAnimations at 0041f0b0: the level's map animations, each starting on RandomRange(1, frames) and drawing its start time.
std::vector<FDecorAnimation> StartMapAnimations(const FLevel& Level, FResourceArchive& Archive, FRandom& Random);

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
    FSoundTrack Sound;
    float Damage = 0.f;
    float Zone = 0.f;
    int32_t BlastTimer = 0;
    // Its age and its class's CREATE blocks' next times (ObjectUpdateCreates at 00437b00), none once a block has made its last.
    uint32_t Age = 0;
    std::vector<std::optional<uint32_t>> NextCreates;
    // The player object whose death set it off, whose blast spares the other players.
    std::optional<size_t> Owner;
};

// An FX object circling its host (FUN_0041c180 creates it, the FX part of ObjectUpdateWeapon at 00444ca0 moves it): a class's ADD FX, as on the Final-PC,
// or what Touch-and-destroy, Invulnerability and Banana attach to the player.
struct FOrbiter
{
    FEffect Effect;
    // The host's object index; none for the player.
    std::optional<size_t> Host;
    float Phase = 0.f;
    int32_t OnTimer = 0;
    int32_t OffTimer = 0;
    // Object flag 8: the host check failed while its class has an on-timeout, so it waits hidden for its next on time.
    bool bHidden = false;
    uint32_t Age = 0;
    std::vector<std::optional<uint32_t>> NextCreates;
};


// An object a shot leaves behind, a Fixed thing such as a pulsar's glow or a rocket's smoke, which lives its class's Life-Time at the shot's altitude.
struct FTrail
{
    std::string ClassName;
    FCollisionPoint Position;
    float Altitude = 0.f;
    uint32_t Age = 0;
    uint32_t LifeTime = 0;
    // A launched product, an explosion's debris: it flies on its inertia, through everything, since its Bounding-Box-Size is zero (object +0xb4 bit 1 skips every collision),
    // leaving its own CREATE trails, such as Big-Yellow-Smoke-Trail.
    struct FFlight
    {
        FPlayerState State;
        FMovementClass Class;
        std::vector<FCreateBlock> Creates;
        std::vector<std::optional<uint32_t>> NextCreates;
    };
    std::optional<FFlight> Flight;
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
    float Energy = 0.f;
    // Terrain bounces so far, which --verify-game checks.
    uint32_t Bounces = 0;
    // Milliseconds since it was fired, which picks its animation frame.
    uint32_t Age = 0;
    // When each of its class's CREATE blocks next makes an object, by its age; none once a block without a repeat has made its one.
    std::vector<std::optional<uint32_t>> NextCreates;
};

// The Video Details options (session +0x27e0) that the port follows.
// ConfigLoad (0042f570) sets them from vzconf.txt's shadow, reactor-fx, hud-green-info, translucent-panel, tv-borders and low-details (or high-details) keywords; the defaults are the Max preset (MenuVideoDetails at 004235a0).
struct FVideoDetails
{
    // Flag 2: object shadows.
    bool bShadows = true;
    // Flag 4: the player's reactor glow.
    bool bReactorFx = true;
    // Flag 8: the weapon lists and the panel's warnings.
    bool bGreenInfo = true;
    // Flag 0x10: the panel's screen is drawn half translucent.
    bool bTranslucentPanel = true;
    // Flag 0x20: the TV borders around the view.
    bool bTvBorders = true;
    // Flag 0x40 (Ground/Smoke details off): hides Is-detail classes and the player's front light.
    bool bLowDetails = false;
    // Flag 0x80: the screen flashes and the death's red tint through the gamma ramp.
    bool bGammaFx = true;
};

// The player's weapons (ADD Weapon blocks), the shots in flight and the game clock and random generator they use.
struct FCombat
{
    FVideoDetails Details;
    // The view's width in screen pixels, 640 in the original; the port's wide view widens it.
    float ViewWidth = 640.f;
    std::vector<FWeaponClass> Weapons;
    std::vector<FAmmoClass> AmmoClasses;
    // The player class's ADD Weapon entries, as indices into Weapons, and the arms every player starts the level with: the first in hand.
    std::vector<size_t> PlayerWeapons;
    FPlayerArms StartArms;
    // The player class's TYPE Explosion blocks, set off when it dies.
    std::vector<FExplosionBlock> PlayerExplosions;
    // Common\Data\Object\Item\surprise.gaf's animations, one per surprise icon.
    std::vector<Gaf::FAnimation> SurpriseIcons;
    std::vector<FProjectile> Projectiles;
    std::vector<FTrail> Trails;
    std::vector<FEffectClass> EffectClasses;
    std::vector<FEffect> Effects;
    std::vector<FOrbiter> Orbiters;
    std::vector<FDecorAnimation> DecorAnimations;
    FRandom Random;
    // Session +0x259c, the Misc stream of EngineCreateRandomStreams (0043c7a0), which EngineRunLevel seeds like the shared generator; the panel's weapon lights draw from it (RandomPushStream at 00428e20, RandomPopStream at 00428eb0).
    FRandom MiscRandom;
    // Every level object's bob, by object index.
    std::vector<FObjectBob> Bobs;
    // The game clock in milliseconds; never zero, since a zero last-shot time means no shot yet.
    uint32_t Clock = 1;
    // A Final-object died: the level is destroyed and the finale runs (FUN_0043db70, then FUN_00445dd0 in EngineRunLevel).
    bool bFinale = false;
    // Level objects the steps created, such as dropped items, for the window and the pickups to take on.
    std::vector<size_t> CreatedObjects;
    // Mines fired this step, which StepProjectiles adds to the level once its object list may grow.
    struct FDroppedMine
    {
        size_t AmmoIndex = 0;
        FCollisionPoint Position;
        int Direction = 0;
        size_t Owner = 0;
    };
    std::vector<FDroppedMine> DroppedMines;
    // Session flags 4 and 8, Triple Fire and Circular Fire: a volley takes one round whatever it fires (WeaponFire at 00443bd0).
    bool bOneRoundPerVolley = false;
};

// The player as a target of shots and blasts.
struct FPlayerTarget
{
    const FPlayerState& State;
    float Radius = 0.f;
    FPlayerInventory& Inventory;
    size_t ObjectIndex = 0;
    // The session's player name (session +0x2754), which the original starts as Jack.
    std::string_view Name = "Jack";
    // The port's co-op: the inventory holding the team's money, keys and codes, null for the player's own (the first player's, or a lone one's).
    FPlayerInventory* Team = nullptr;
};

// The inventory a player's money, keys and codes go to.
inline FPlayerInventory& PurseOf(const FPlayerTarget& Player)
{
    return Player.Team ? *Player.Team : Player.Inventory;
}

// The players a step meets, each a level object with its own state and inventory.
using FPlayerTargets = std::span<const FPlayerTarget>;

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

// What the player asks of a step, from the keys, the mouse's sight or the pad, before the surprises and the finale bend it.
struct FPlayerIntent
{
    bool bForward = false;
    bool bBackward = false;
    bool bTurnLeft = false;
    bool bTurnRight = false;
    bool bStrafeLeft = false;
    bool bStrafeRight = false;
    // The port's pad: its turn rate, a share of the class's, and its push, which scales the top speed and the strafe.
    float TurnRate = 1.f;
    std::optional<float> Throttle;
};

// ReadPlayerInput's rules: Extasy holds the thrust forward, Reversed-commands swaps left with right and the two strafes, and Blocked-motor ignores every command.
// Unlike the original, the finale takes the controls away too, so nothing can undo the win.
FPlayerCommand PlayerCommandFromIntent(const FPlayerIntent& Intent, const FSurpriseState& Surprises, bool bFinale);
// Whether the step thrusts forward, which a blocked motor answers with start-engine.wav.
bool IsThrustIntent(const FPlayerIntent& Intent, const FSurpriseState& Surprises, bool bFinale);

// One .3DR record per game step: the step's start time on the game clock, its input bits, and the player's position and direction before the step.
struct FDemoRecord
{
    uint32_t Time;
    uint32_t Input;
    float X;
    float Y;
    int32_t Direction;
};
static_assert(sizeof(FDemoRecord) == 20);

// DemoLoad at 004292b0: VzMenuLoop plays CAVE\DEMO.3DR, the one recording, on the CAVE\DEMO level.
std::expected<std::vector<FDemoRecord>, std::string> LoadDemo(FResourceArchive& Archive);
// The step length of each record, as the recording machine's frames split their time: playing the steps in this order gives every step its own record.
// FrameEnds, when given, marks each frame's last record, after which that machine ran its draw pass.
std::vector<uint32_t> DemoStepLengths(std::span<const FDemoRecord> Records, std::vector<bool>* FrameEnds = nullptr);
// Demo semantics of ReplayPlayerInput (00438800): strafing cancels turning.
FPlayerCommand PlayerCommandFromInput(uint32_t InputBits);
// With the mouse as input device, the turn of InputReadDirection (0041d7e0) toward the sight, given the screen vector from the player to it: the short way round (DirectionTurnCommand at 00436600), none when the sight lies straight behind or, while the player moves, within one direction of its facing.
EPlayerTurn TurnTowardSight(const FPlayerState& Player, FCollisionPoint ScreenVector);
// The player's facing as a screen vector.
FCollisionPoint PlayerFacing(const FPlayerState& Player);
// How far the screen vector lies off the player's facing, in directions (24 to a full turn).
float AimOffDirections(const FPlayerState& Player, FCollisionPoint ScreenVector);
// The pad's aim turning the ship: the short way round, straight back included, until it faces the aim's direction or lies within HoldDirections of it.
EPlayerTurn TurnTowardAim(const FPlayerState& Player, FCollisionPoint ScreenVector, float HoldDirections = 0.f);
// InputReadDirection with the keyboard and the absolute command: the movement keys give a screen direction; facing it the player thrusts, facing straight away it backs off, otherwise it only turns toward it.
// Returns PlayerInput bits.
uint32_t AbsoluteKeyInput(const FPlayerState& Player, FCollisionPoint ScreenVector);
// The pad's left stick heading the ship: the facing's neighbours count as on course, the ship turns the short way round and thrusts only within a quarter turn of the target.
uint32_t StickMoveInput(const FPlayerState& Player, FCollisionPoint ScreenVector);
// The pad's move while the right stick aims: the screen direction split into thrust and strafe along the ship, as PlayerInput bits.
uint32_t FacingMoveInput(const FPlayerState& Player, FCollisionPoint ScreenVector);

std::expected<FMovementClass, std::string> ReadMovementClass(const FLevelObject& Object);
FPlayerState SpawnPlayer(const FLevelObject& Object, const FMovementClass& Class);
// Every collidable object except the player; classes without movement properties keep zeroed fields like the original class record.
std::vector<FObjectBody> CreateObjectBodies(const FLevel& Level);
// The body of one level object, or nullopt when it does not collide.
std::optional<FObjectBody> CreateObjectBody(const FLevel& Level, size_t Index);
// Every generator's birth cycle for one step; new products join Bodies and Combat.CreatedObjects.
void StepGenerators(FCombat& Combat, FLevel& Level, std::vector<FObjectBody>& Bodies, FPlayerTargets Players, uint32_t StepMilliseconds);
// EngineActivateObject (0043c150) for each placed StaticGen as the level starts: with something solid on it the generator opens and waits; otherwise a birth is under way at once and it stands closed.
void ActivateGenerators(const FLevel& Level, std::vector<FObjectBody>& Bodies, FPlayerTargets Players);
std::vector<FPickup> CreatePickups(const FLevel& Level);
// The 1-based slot "next weapon" moves to (FUN_004376a0): the first after the one in hand, wrapping after slot 9, whose ammo stock is not empty, or the one in hand when none is.
size_t NextPlayerWeaponSlot(const FCombat& Combat, const FPlayerInventory& Inventory);
// The pad's previous weapon, which the original lacks: the same search the other way round.
size_t PreviousPlayerWeaponSlot(const FCombat& Combat, const FPlayerInventory& Inventory);
// What the pad's aim may lock onto: a live, visible object that hunts the player (ObjectHuntsPlayer at 004371b0), not yet a wreck.
bool IsAutoAimTarget(const FObjectBody& Body, const FLevel& Level);
// The port's co-op: the hunters' energy and shield for a number of players, so the extra guns do not make a level easier, and their scaling by a ratio of two such factors.
constexpr float CoopHunterScale(const size_t PlayerCount)
{
    constexpr float PerExtraPlayer = 0.6f;
    return 1.f + PerExtraPlayer * float(PlayerCount > 1 ? PlayerCount - 1 : 0);
}
void ScaleHunters(std::span<FObjectBody> Bodies, float Ratio);
// The player's death branch of EngineUpdateObjects (004406e0): ObjectDestroy keeps the player object but sets off its explosions.
void ExplodePlayer(FCombat& Combat, FCollisionPoint Position, float Altitude, size_t PlayerObject);
// FUN_00437740: takes the slot's weapon in hand and, with option flag 8, lists its weapon and ammo for 4 seconds.
void SelectPlayerWeapon(const FCombat& Combat, size_t Slot, FPlayerInventory& Inventory, FMessages& Messages);
// The stock that feeds the weapon in hand, if the player carries one.
const FAmmoStock* PlayerSlotAmmo(const FCombat& Combat, const FPlayerInventory& Inventory);
// The stock bound to a 1-based slot's weapon, if the slot holds one (ObjectBindAmmo at 00437540).
const FAmmoStock* SlotAmmo(const FCombat& Combat, size_t Slot, const FPlayerInventory& Inventory);
// FUN_004375f0 on every slot, as the inventory's weapon list binds them in turn: each ADD Weapon's last shot (entry +0x14) is cleared, the one in hand's too.
void ClearLastShots(FPlayerInventory& Inventory);
std::optional<FPickup> CreatePickup(const FLevel& Level, size_t ObjectIndex);
// FUN_0041cf40 with the item it comes from: the surprise in effect ends and the new one is carried, announced and waits half a second.
void CarrySurprise(FCombat& Combat, FPlayerInventory& Inventory, ESurprise Type, const FLevelObject& Item, FMessages& Messages);
FPlayerInventory CreateInventory(const FLevelObject& Player);
// Collects every pickup the player's circle touches; returns how many were taken and marks them removed in the level.
// With several players, a pack of energy, shield or ammo is every player's own and stays until each has taken it, its money going to the first; Team takes the money, keys and codes.
size_t CollectPickups(const FPlayerState& Player, const FMovementClass& Class, FPlayerInventory& Inventory, std::vector<FPickup>& Pickups, FLevel& Level,
    FMessages& Messages, FCombat* Combat = nullptr, size_t PlayerObject = 0, size_t PlayerCount = 1, FPlayerInventory* Team = nullptr);
// FUN_0041d290 for the player once a step, before it moves: surprises wait, start, and end. Then the invisible drone's return (FUN_00445380).
// AnimUpdateBounds at 0041b4b0 recomputes the collision centre from the world position.
FCollisionPoint CollisionFromWorld(FCollisionPoint World);

// The 64 x 32 world-pixel grid cell an object is linked into.
struct FWorldCell
{
    int X = 0;
    int Y = 0;
};

FWorldCell CellOf(FCollisionPoint World);
// The player object: EngineActivateObjects (0043c700) visits the grid from its last cell back, each cell's newest object first, and every Player it activates becomes session +0x203c (FUN_0043c150),
// so the player is the one in the lowest cell, the oldest there. Levels such as HITEK\T1H1 place two.
std::optional<size_t> FindPlayerObject(const FLevel& Level);

// A Saving-zone object (class flag 0x4000000) and the Saving-plot object at its exact position, which EngineUpdateObjects looks for in the zone's cell; a zone without one does nothing but keep the count going.
struct FSavingPlot
{
    size_t Zone = 0;
    std::optional<size_t> Plot;
};

std::vector<FSavingPlot> FindSavingPlots(const FLevel& Level);

// Session +0x20c0 to +0x20cc: whether a count runs (bit 0), when it started on PgClockNow, whether it saved or refused (bit 1), and the plot.
struct FSavingState
{
    bool bCounting = false;
    uint32_t Start = 0;
    bool bDone = false;
    std::optional<size_t> Plot;
};

// SavingPlotUpdate at 0042a3b0 for each zone the living player touches, and SavingPlotLeave at 0042a650 once it touches none: after 5.5 seconds on a plot the plot's cost (its negative Reward) is paid and the session is saved.
// True when the session is to be written now.
bool StepSavingPlots(FSavingState& State, std::span<const FSavingPlot> Plots, const FLevel& Level, const FPlayerState& Player, float PlayerRadius,
    FPlayerInventory& Inventory, FMessages& Messages, uint32_t NowMilliseconds, std::string_view PlayerName);

// FUN_00439510 for the player after its surprises: a complete code with a timeout counts down on its first part, warns, and at the end goes back into the level (or is destroyed without Reactivate-after).
void StepCodeTimeouts(FPlayerInventory& Inventory, FLevel& Level, FMessages& Messages, uint32_t StepMilliseconds);
void StepSurprises(FCombat& Combat, const FLevel& Level, std::vector<FObjectBody>& Bodies, FPlayerState& Player, FMovementClass& Class, FPlayerInventory& Inventory,
    FMessages& Messages, uint32_t StepMilliseconds);
// One game step of the player's part of EngineUpdateObjects (004406e0).
// The player as an obstacle for an actor's move, and the knock damage it took.
struct FPlayerKnock
{
    FCollisionCircle Circle;
    bool bHit = false;
    float Damage = 0.f;
    bool bTouchAndDestroy = false;
    // A mine that went off against the player pushes it (AmmoImpact at 0043d350), before the player's Weight divides it.
    FCollisionPoint Push;
};

// Bodies are the other collidable objects; Self is the body moving, if any, which knocks hurt.
// Returns the body the move ran into, if any.
const FObjectBody* StepPlayer(FPlayerState& Player, const FMovementClass& Class, const FPlayerCommand& Command, uint32_t StepMilliseconds, const FTerrainCollision& Terrain,
    const FLevel& Level, std::span<FObjectBody> Bodies = {}, FObjectBody* Self = nullptr, FPlayerKnock* PlayerHit = nullptr);
// The player's bounce spark after its step (FUN_0043d140): only under Pinball, the Flipper-Bong FX.
void ThrowPlayerBounceSpark(FCombat& Combat, const FPlayerState& Player, ESurprise Active);
// Moves pushed bodies for one step, in object order. They run into the player like into any object, hurt it by their knocks, and a pushed mine goes off against it;
// returns how far those mines push the player.
// Actors look for the player, patrol, chase and shoot, then move like the player with the resulting command. Without combat they only patrol.
// Actors that run into the player hurt it by their knocks.
void StepActors(FCombat* Combat, std::span<FObjectBody> Bodies, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level,
    FPlayerTargets Players);
// Returns each player's push from the bodies that knocked it, in the players' order.
std::vector<FCollisionPoint> StepObjectBodies(std::span<FObjectBody> Bodies, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level,
    FPlayerTargets Players = {});
// Every gate and lift of the level, in their activation state (AnimInitializeGraphics at 00417b30): open ones are passable and show the last frame of their first animation.
std::expected<std::vector<FDoor>, std::string> CreateDoors(const FLevel& Level, FResourceArchive& Archive, std::span<FObjectBody> Bodies);
// One step of every gate (GateUpdate at 00443620) and lift (ObjectUpdateLevelExit at 00442bf0) after its animation advances.
// Returns the lift the player has taken, if any.
const FDoor* StepDoors(std::vector<FDoor>& Doors, const FLevel& Level, std::span<FObjectBody> Bodies, FPlayerTargets Players,
    uint32_t StepMilliseconds, FMessages& Messages);
// The player's contact with a closed lift or gate tells it what the door is waiting for.
void ReportDoorContact(std::span<const FDoor> Doors, const FObjectBody& Body, const FPlayerState& Player, const FPlayerInventory& Inventory, FMessages& Messages);
// ObjectUpdateLevelExit names the next level within the current zone, or within the lift's Scenery when the zone changes.
std::string NextLevelPath(std::string_view CurrentPath, const FDoor& Lift);
// Activates the level's objects, which draws each bob's phase and sets its first altitude, then loads the player's weapons, arms the level's turrets and loads every FX class the level's ammo and explosions can spawn.
// A demo's playback reseeds the generator (DemoStartPlayback at 00429110) after the map animations' draws and before the objects activate.
std::expected<FCombat, std::string> CreateCombat(FLevel& Level, const FLevelObject& Player, FResourceArchive& Archive, std::vector<FObjectBody>& Bodies,
    bool bDemo = false);
// The bob part of the per-step update pass: every object in the view's cells bobs as ObjectSelectBobMode (00433f40) decides, which changes the altitude its shots start from.
void StepObjectBobs(FCombat& Combat, FLevel& Level, std::span<FObjectBody> Bodies, uint32_t StepMilliseconds, FViewCells View);
// One step of the player's weapon: the slope ramp, then a shot when the trigger is held and the weapon's delay has passed (WeaponFire at 00443bd0).
// SpriteSize is the smaller side of the player's current frame, which picks the random muzzle distance.
// The world offset Distance along a direction, by the table WeaponFire and ObjectDraw use (0050d440).
FCollisionPoint WorldFacingOffset(int Direction, float Distance);
void StepPlayerWeapon(FCombat& Combat, bool bFire, FPlayerState& Player, const FLevelObject& PlayerObject, size_t PlayerIndex, float SpriteSize,
    FPlayerInventory& Inventory, FMessages& Messages);
// The seeker scan, top rotation and fire decision of every turret (SeekerUpdate at 0042c5a0), then its weapon update.
void StepTurrets(FCombat& Combat, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, const FLevel& Level, std::vector<FObjectBody>& Bodies,
    FPlayerTargets Players);
// Moves every shot, detonating it against terrain, bodies or the ground, then runs the effects and their blasts; destroyed bodies are removed, spawn their Boom-fx and pay their Reward.
void StepProjectiles(FCombat& Combat, uint32_t StepMilliseconds, const FTerrainCollision& Terrain, FLevel& Level, std::vector<FObjectBody>& Bodies,
    FPlayerTargets Players, FMessages& Messages);
inline void StepProjectiles(FCombat& Combat, const uint32_t StepMilliseconds, const FTerrainCollision& Terrain, FLevel& Level, std::vector<FObjectBody>& Bodies,
    const FPlayerTarget& Player, FMessages& Messages)
{
    StepProjectiles(Combat, StepMilliseconds, Terrain, Level, Bodies, FPlayerTargets(&Player, 1), Messages);
}
std::expected<void, std::string> VerifyPlayerMovement();
// Loads every level's weapons and fires them for a few seconds of game time.
std::expected<std::string, std::string> VerifyCombat(FResourceArchive& Archive);
std::expected<std::string, std::string> VerifyPatrols(FResourceArchive& Archive);
// The level's enemies still alive, as the session counter at +0x2094 keeps them: seekers and trackers that hunt the player.
// Generators of hunters count too, as EngineActivateObjects counts them.
int CountEnemies(const FLevel& Level, std::span<const FObjectBody> Bodies);
// Every level's gates close with nothing near and open for the player, and the player rides every usable lift.
std::expected<std::string, std::string> VerifyDoors(FResourceArchive& Archive);
// EngineRunLevel at 0043e5f0 moves the dead player's position, which the view follows, in a growing square: right, up, left, down.
// It turns whenever a coordinate lands on a multiple of the turn interval or leaves the level.
struct FDeathDrift
{
    float VelocityX = 0.5f;
    float VelocityY = 0.f;
    int32_t TurnInterval = 32;
};
void StepDeathDrift(FDeathDrift& Drift, FPlayerState& Player, const FLevel& Level, uint32_t StepMilliseconds);
// The flashes that push the gamma offsets, each channel clamped to Limit.
void PushGamma(std::array<int32_t, 3>& Gamma, int32_t Amount, int32_t Limit);

// Replays the CAVE\DEMO.3DR inputs through StepPlayer and compares every step with the recorded position.
std::expected<std::string, std::string> VerifyDemoReplay(FResourceArchive& Archive);
