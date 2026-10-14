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
    FLevelObject Object;
    FPlayerState State;
    FPlayerInventory Inventory;
    FWeaponState Weapon;
    size_t WeaponSlot = 1;
    std::vector<FWeaponState> WeaponSlots;
    uint32_t Clock = 1;
};

// A session of the original's own format (SessionWrite at 0042ad70): a folder, session\<name>\default or s%03d in the game folder, holding last.txt (FUN_0042b190),
// the level's objects as <zone>-<level>.3do (Level3doWrite at 004163a0) and, for a numbered one, the same name in .tga, a 224x168 picture of the screen (FUN_0042b360).
struct FSession
{
    std::filesystem::path Folder;
    // <zone>\<level>, as the level loader takes it.
    std::string LevelPath;
    std::string LevelName;
    // The game clock in milliseconds.
    uint32_t Clock = 0;
    float Energy = 0.f;
    float Shield = 0.f;
    // Session +0x1028: the weapon slot in hand.
    size_t WeaponSlot = 1;
    size_t DifficultyLevel = 1;
    // Session +0x26e0 and +0x26e4, and the player's +0x26c, +0x270 and +0x274.
    int ShotsHit = 0;
    int ShotsMissed = 0;
    int Kills = 0;
    int Earned = 0;
    int Money = 0;
};

// What the .3do writes on the player object: where it stands and faces, and its inventory with the objects it carries.
struct FSessionPlayer
{
    size_t ObjectIndex = 0;
    float X = 0.f;
    float Y = 0.f;
    int Direction = 0;
    const FPlayerInventory* Inventory = nullptr;
    // Its motion, pose and flags, and its turn timeout, which the speed toggle changes.
    const FPlayerState* State = nullptr;
    int TurnTimeout = 0;
};

// Level3doWrite (004163a0) with flags 0xb: the header, then every object from the grid's last row and column back to the first, each cell's newest object first, leaving out the No-saving classes.
// Level3doWriteObject (00416730) writes the class, the position and what differs from a fresh object of the class, then the attribute blocks (FUN_00417210); a carrier's objects come before it, named again by its HAVE lines.
std::string FormatSessionObjects(const FLevel& Level, std::span<const FObjectBody> Bodies, std::span<const FDoor> Doors, const FSessionPlayer& Player, size_t DifficultyLevel);

// LevelLoad3do (00413d80) takes a HAVE line's object off the level (FUN_004160a0 finds the name from the grid's last cell back) and gives it to the carrier.
// Returns, for each object, the indices of those it carries; they stay in the level marked removed.
std::vector<std::vector<size_t>> ResolveCarriedObjects(FLevel& Level);

// The game folder's session\<name>.
std::filesystem::path PlayerSessionFolder(const std::filesystem::path& GameFolder, std::string_view PlayerName);
std::filesystem::path DefaultSessionFolder(const std::filesystem::path& PlayerFolder);
// last.txt read from a session folder; nullopt when it is missing or malformed. Session files are untrusted.
std::optional<FSession> ReadSession(const std::filesystem::path& Folder);
std::optional<std::string> ReadSessionObjects(const FSession& Session);
// The numbered sessions (s%03d), newest first.
std::vector<FSession> ReadSavedSessions(const std::filesystem::path& PlayerFolder);
// The session's thumbnail, 224x168 RGB555 pixels from the top row down, if it has one.
std::optional<std::vector<uint16_t>> ReadSessionThumbnail(const FSession& Session);

inline constexpr uint32_t ThumbnailWidth = 0xe0;
inline constexpr uint32_t ThumbnailHeight = 0xa8;

// Writes the default session, or a numbered one after the highest, deleting those six or more numbers older, as SessionWrite does; a numbered one also gets the thumbnail.
bool WriteSession(const std::filesystem::path& PlayerFolder, bool bDefault, const FSession& Session, std::string_view Objects,
    std::span<const uint16_t> Thumbnail = {});

std::string FormatLastText(const FSession& Session, std::string_view RelativeFolder);
std::optional<FSession> ParseLastText(std::string_view Text);

// The port's folder for what it keeps for itself: %APPDATA%\OpenVyruz on Windows, $XDG_DATA_HOME/OpenVyruz (or ~/.local/share) elsewhere.
std::filesystem::path UserDataDirectory();

class FResourceArchive;

// The --verify-game check: every level written as a session and loaded back keeps its objects, with the removed ones gone, an object created during play, changed doors and a carried key; and a saving plot counts down, refuses without the cash and saves once with it.
std::expected<std::string, std::string> VerifySessions(FResourceArchive& Archive);
