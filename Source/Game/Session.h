#pragma once

#include "Game/Engine.h"
#include "Game/Level.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What LoopLevels (004467d0) keeps of the player object for the next level: everything but its position and velocity.
struct FCarriedPlayer
{
    // Empty when read from a session file; the level's own player object stands in.
    FLevelObject Object;
    FPlayerState State;
    FPlayerInventory Inventory;
    FWeaponState Weapon;
    size_t WeaponSlot = 1;
    std::vector<FWeaponState> WeaponSlots;
    uint32_t Clock = 1;
};

// One object as Level3doWriteObject (00416730) writes it: its class, position and the fields that differ from a fresh object.
struct FSessionObject
{
    // The object's place in the level's .3DO, or SIZE_MAX for one created during play, which comes from its class.
    size_t FileIndex = SIZE_MAX;
    std::string ClassName;
    float X = 0.f;
    float Y = 0.f;
    float Altitude = 0.f;
    int Direction = ObjectDirectionCount;
    int TopPose = 0;
    bool bWrecked = false;
    // A carried key's object, kept so a timed code can return to the level.
    bool bRemoved = false;
    std::optional<float> Energy;
    std::optional<float> Shield;
    // A gate's or lift's EDoorFlag bits and usage count, which the writer puts in its TYPE block.
    std::optional<uint32_t> DoorFlags;
    int UsageCount = 0;
};

// A session (SessionWrite at 0042ad70): a level, the player brought into it, and for a session saved during play the level's objects.
struct FSession
{
    std::string LevelPath;
    std::string LevelName;
    // What SessionDescribeDefault (00427ad0) shows: the game clock and the player's energy and shield.
    uint32_t Clock = 0;
    float Energy = 0.f;
    float Shield = 0.f;
    // Nullopt for a new game's first level, whose player comes from the level.
    std::optional<FCarriedPlayer> Player;
    // Nullopt for a session written as its level starts, which loads the level as it is.
    std::optional<std::vector<FSessionObject>> Objects;
};

// Level3doWrite (004163a0) visits the grid from its last row and column back to the first, each cell's newest object first, so a level loaded from the session activates its objects in that order.
// Objects carried keys refer to are kept as removed; KeyObjects maps the keys' object indices to the snapshot's.
std::vector<FSessionObject> CaptureObjects(const FLevel& Level, std::span<const FObjectBody> Bodies, std::span<const FDoor> Doors, std::vector<FCarriedKey>& Keys);
// Replaces the level's objects with the snapshot's, as loading the session's .3DO would create them.
void RestoreObjects(FLevel& Level, std::span<const FSessionObject> Objects);

std::string FormatSession(const FSession& Session);
// Session files are untrusted: nullopt for anything malformed.
std::optional<FSession> ParseSession(std::string_view Text);

// The port's folder for what the original keeps beside it: %APPDATA%\OpenVyruz on Windows, $XDG_DATA_HOME/OpenVyruz (or ~/.local/share) elsewhere.
std::filesystem::path UserDataDirectory();
// The player's session folder (session\<name> in the original's game folder), kept in the user's data folder.
std::filesystem::path SessionDirectory(std::string_view PlayerName);
bool WriteSessionFile(const std::filesystem::path& Path, const FSession& Session);
std::optional<FSession> ReadSessionFile(const std::filesystem::path& Path);
// The default session, written as each level starts.
std::filesystem::path DefaultSessionPath(const std::filesystem::path& Directory);
// SessionWrite's numbered sessions (s%03d), newest first.
std::vector<FSession> ReadSavedSessions(const std::filesystem::path& Directory);
// Writes the next numbered session and deletes those six or more numbers older, as SessionWrite does.
bool WriteSavedSession(const std::filesystem::path& Directory, const FSession& Session);

class FResourceArchive;

// The --verify-game check: every level's objects survive a session written and read back, with the removed ones gone, an object created during play and changed doors; and a saving plot counts down, refuses without the cash and saves once with it.
std::expected<std::string, std::string> VerifySessions(FResourceArchive& Archive);
