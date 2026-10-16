#pragma once

#include "Game/Panel.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

class FMenuArt;

// SaveScores (0043e470) and the table behind it (ScoreShowTable at 00401c40, ScoreRun at 00401960): vz.sco holds twenty records, best first, each one 32-bit value per score column and the player's name at +0x34.
struct FScoreRecord
{
    std::array<uint32_t, 13> Fields{};
    std::array<char, 20> Name{};
};
static_assert(sizeof(FScoreRecord) == 0x48);

inline constexpr size_t ScoreRecordCount = 20;
using FScoreTable = std::array<FScoreRecord, ScoreRecordCount>;

// The record SaveScores makes at the end of a game: the tower and level (the .3DO LEVEL-ID), kills, money earned, the share of the player's shots that hit, the shots, the game time and the name.
FScoreRecord MakeScore(uint32_t Tower, uint32_t Level, int32_t Kills, int32_t Earned, int32_t Hits, int32_t Misses, uint32_t ClockMilliseconds,
    std::string_view Name);
// ScoreRun at 00401960 starts a file of any other size over, empty.
FScoreTable ReadScores(const std::filesystem::path& Path);
bool WriteScores(const std::filesystem::path& Path, const FScoreTable& Table);
// ScoreInsert at 004015a0: the record climbs from the bottom past every record it beats on the sorted columns, compared in column order, ties to the newer; returns its 1-based rank, or 0 when it does not place.
size_t InsertScore(FScoreTable& Table, const FScoreRecord& Record);
// ScoreDrawRows at 00401480: the headers and the twenty rows in v-menu.gaf font 4, the given 1-based rank brightened (0 for none).
std::vector<FHudSprite> DrawScores(const FMenuArt& Art, const FScoreTable& Table, size_t Highlight);
// ScoreHighlightRow at 00401410: the strip the brightened row takes, across the table from its first column to its last edge, from the font's baseline less 2 above the row to 4 below it.
struct FScoreStrip
{
    int32_t Left = 0;
    int32_t Top = 0;
    int32_t Right = 0;
    int32_t Bottom = 0;
};
std::optional<FScoreStrip> ScoreHighlightStrip(const FMenuArt& Art, size_t Highlight);

// The --verify-game check: records rank by tower, level, kills, money, hit share and shots, ties to the newer, and vz.sco reads back.
bool VerifyScores();
