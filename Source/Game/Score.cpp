#include "Game/Score.h"

#include "Game/Language.h"
#include "Game/Menu.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace
{
// The column flags of DAT_0050d070: 1 sorts by the column, 2 marks the title row's column, 4 prints the name, 8 prints the rank.
enum EScoreColumnFlag : uint32_t
{
    ScoreSortKey = 0x1,
    ScoreRank = 0x8,
};

enum class EScoreAlign : uint8_t
{
    Left,
    Centre,
    Right,
};

// A column of the table at DAT_0050d070, as ScoreShowTable at 00401c40 fills it: its left edge, a header offset, its width (zero runs to the next column), flags, alignment, header and value format, and the header and row colours (diffuse and specular) that ScoreRowColor at 00401350 blends.
struct FScoreColumn
{
    int32_t X = 0;
    int32_t HeaderOffsetY = 0;
    int32_t Width = 0;
    uint32_t Flags = 0;
    EScoreAlign Align = EScoreAlign::Left;
    std::string_view Title;
    std::string_view Format;
    uint32_t HeaderSpecular = 0;
};

constexpr std::array<FScoreColumn, 13> Columns = { {
    { .X = 0, .HeaderOffsetY = -0x20, .Width = 0x280, .Flags = 2, .Align = EScoreAlign::Centre, .Title = "THE BRAVEST RESISTANCE FIGHTERS", .HeaderSpecular = 0x2000 },
    { .X = 0x28, .Flags = ScoreRank, .Title = "Rank", .Format = "#%u" },
    { .X = 0x50, .Width = 0x28, .Flags = ScoreSortKey, .Align = EScoreAlign::Centre, .Title = "Tower", .Format = "%u" },
    { .X = 0x82, .Width = 0x28, .Flags = ScoreSortKey, .Align = EScoreAlign::Centre, .Title = "Level", .Format = "%u" },
    { .X = 0xb4, .Width = 0x1e, .Flags = ScoreSortKey, .Align = EScoreAlign::Centre, .Title = "Killed", .Format = "%d" },
    { .X = 0xdc, .Width = 0x46, .Flags = ScoreSortKey, .Align = EScoreAlign::Right, .Title = "Money", .Format = "%d" },
    { .X = 0x136, .Width = 0x14, .Flags = ScoreSortKey, .Align = EScoreAlign::Centre, .Title = "Hit~", .Format = "%d" },
    { .X = 0x154, .Width = 0x37, .Flags = ScoreSortKey, .Align = EScoreAlign::Right, .Title = "Ammo", .Format = "%d" },
    { .X = 0x19a, .Width = 0x14, .Align = EScoreAlign::Right, .Format = "%02d" },
    { .X = 0x1ae, .Width = 0x14, .Align = EScoreAlign::Centre, .Title = "Time", .Format = ":%02d" },
    { .X = 0x1c2, .Width = 0x14, .Format = ":%02d" },
    { .X = 0x1ea, .Flags = 4, .Title = "Name" },
    { .X = 0x280 },
} };

constexpr uint32_t HeaderDiffuse = 0x80ffa0;
// Row 1 prints in the first colour pair, row 20 in the second, the rows between blended.
constexpr uint32_t FirstRowDiffuse = 0xffffff;
constexpr uint32_t FirstRowSpecular = 0;
constexpr uint32_t LastRowDiffuse = 0x80a0ff;
constexpr uint32_t LastRowSpecular = 0x90;
constexpr size_t ScoreFont = 4;
constexpr int32_t HeaderY = 0x50;
constexpr int32_t FirstRowY = 0x6e;
constexpr int32_t RowStep = 0x12;

// ColorBlend at 0044df60: each channel A * WeightA + B * WeightB over 256.
constexpr uint32_t BlendColor(const uint32_t A, const uint32_t WeightA, const uint32_t B, const uint32_t WeightB)
{
    constexpr uint32_t ChannelMask = 0xff;
    constexpr int ChannelBits = 8;
    uint32_t Result = 0;
    for (int Shift = 0; Shift < 3 * ChannelBits; Shift += ChannelBits)
    {
        Result |= ((((A >> Shift) & ChannelMask) * WeightA + ((B >> Shift) & ChannelMask) * WeightB) >> ChannelBits) << Shift;
    }

    return Result;
}
static_assert(BlendColor(0xffffff, 0x100, 0x80a0ff, 0) == 0xffffff);

// The printf specifiers the columns use: %u, %d and %02d.
std::string FormatValue(const std::string_view Pattern, const uint32_t Value)
{
    const size_t Percent = Pattern.find('%');
    if (Percent == std::string_view::npos)
    {
        return std::string(Pattern);
    }

    const size_t End = Pattern.find_first_of("ud", Percent);
    if (End == std::string_view::npos)
    {
        return std::string(Pattern);
    }

    const std::string_view Spec = Pattern.substr(Percent + 1, End - Percent - 1);
    const bool bSigned = Pattern[End] == 'd';
    const std::string Number = Spec == "02" ? (bSigned ? std::format("{:02}", int32_t(Value)) : std::format("{:02}", Value))
                                            : (bSigned ? std::format("{}", int32_t(Value)) : std::format("{}", Value));
    return std::string(Pattern.substr(0, Percent)) + Number + std::string(Pattern.substr(End + 1));
}
}

FScoreRecord MakeScore(const uint32_t Tower, const uint32_t Level, const int32_t Kills, const int32_t Earned, const int32_t Hits, const int32_t Misses,
    const uint32_t ClockMilliseconds, const std::string_view Name)
{
    constexpr uint32_t MillisecondsPerHour = 3600000;
    constexpr uint32_t MillisecondsPerMinute = 60000;
    constexpr uint32_t MillisecondsPerSecond = 1000;
    constexpr uint32_t Sixty = 60;
    constexpr int32_t Percent = 100;
    const int32_t Shots = Hits + Misses;
    FScoreRecord Record;
    Record.Fields = {
        0,
        0,
        Tower,
        Level,
        uint32_t(Kills),
        uint32_t(Earned),
        uint32_t(Shots == 0 ? 0 : Hits * Percent / Shots),
        uint32_t(Shots),
        ClockMilliseconds / MillisecondsPerHour % Sixty,
        ClockMilliseconds / MillisecondsPerMinute % Sixty,
        ClockMilliseconds / MillisecondsPerSecond % Sixty,
        0,
        0,
    };
    std::ranges::copy(Name.substr(0, Record.Name.size() - 1), Record.Name.begin());
    return Record;
}

FScoreTable ReadScores(const std::filesystem::path& Path)
{
    FScoreTable Table{};
    std::error_code Error;
    if (std::filesystem::file_size(Path, Error) != sizeof(FScoreTable) || Error)
    {
        return Table;
    }

    std::ifstream File(Path, std::ios::binary);
    File.read(reinterpret_cast<char*>(Table.data()), sizeof(FScoreTable));
    if (!File)
    {
        return FScoreTable{};
    }

    // The names are untrusted: each ends inside its record.
    for (FScoreRecord& Record : Table)
    {
        Record.Name.back() = '\0';
    }

    return Table;
}

bool WriteScores(const std::filesystem::path& Path, const FScoreTable& Table)
{
    std::error_code Error;
    std::filesystem::create_directories(Path.parent_path(), Error);
    std::ofstream File(Path, std::ios::binary | std::ios::trunc);
    File.write(reinterpret_cast<const char*>(Table.data()), sizeof(FScoreTable));
    return bool(File);
}

size_t InsertScore(FScoreTable& Table, const FScoreRecord& Record)
{
    size_t Place = Table.size();
    while (Place > 0)
    {
        const FScoreRecord& Above = Table[Place - 1];
        bool bBeats = true;
        for (size_t Column = 0; Column < Columns.size(); ++Column)
        {
            if ((Columns[Column].Flags & ScoreSortKey) == 0)
            {
                continue;
            }

            if (Record.Fields[Column] < Above.Fields[Column])
            {
                bBeats = false;
            }
            else if (Above.Fields[Column] < Record.Fields[Column])
            {
                break;
            }
        }

        if (!bBeats)
        {
            break;
        }

        --Place;
    }

    if (Place == Table.size())
    {
        return 0;
    }

    std::shift_right(Table.begin() + std::ptrdiff_t(Place), Table.end(), 1);
    Table[Place] = Record;
    return Place + 1;
}

std::vector<FHudSprite> DrawScores(const FMenuArt& Art, const FScoreTable& Table, const size_t Highlight)
{
    std::vector<FHudSprite> Out;
    const auto Cell = [&](const size_t Column, const int32_t Y, const std::string_view Text, const Font::FColor& Color)
    {
        const FScoreColumn& Source = Columns[Column];
        if (Text.empty())
        {
            return;
        }

        // PgFontPrintAligned at 0044b100 aligns the text inside the column's box.
        const int32_t Width = Source.Width == 0 && Column + 1 < Columns.size() ? Columns[Column + 1].X - Source.X : Source.Width;
        const int32_t TextWidth = Art.TextWidth(ScoreFont, Text);
        const int32_t X = Source.Align == EScoreAlign::Centre ? Source.X + (Width - TextWidth) / 2
                          : Source.Align == EScoreAlign::Right ? Source.X + Width - TextWidth
                                                               : Source.X;
        const std::vector<FHudSprite> Glyphs = Art.DrawLine(ScoreFont, Text, X, Y, Color);
        Out.insert(Out.end(), Glyphs.begin(), Glyphs.end());
    };
    for (size_t Column = 0; Column < Columns.size(); ++Column)
    {
        Cell(Column, HeaderY + Columns[Column].HeaderOffsetY, Localize(Columns[Column].Title), { .Diffuse = HeaderDiffuse, .Specular = Columns[Column].HeaderSpecular });
    }

    constexpr uint32_t FullWeight = 0x100;
    constexpr int32_t LastBlend = 0x13;
    for (size_t Row = 1; Row <= Table.size(); ++Row)
    {
        const FScoreRecord& Record = Table[Row - 1];
        // ScoreDrawRows prints all twenty rows, zeros for the slots no game has filled; the port leaves those out, so a new table shows only real scores.
        if (Record.Name[0] == '\0' && std::ranges::all_of(Record.Fields, [](const uint32_t Field) { return Field == 0; }))
        {
            continue;
        }

        const int32_t Y = FirstRowY + int32_t(Row - 1) * RowStep;
        const int32_t Steps = std::min(int32_t(Row) - 1, LastBlend);
        const uint32_t Weight = Row == 1 ? FullWeight : uint32_t((Steps * -int32_t(FullWeight)) / LastBlend) & 0xff;
        const Font::FColor Color{
            .Diffuse = BlendColor(FirstRowDiffuse, Weight, LastRowDiffuse, FullWeight - Weight),
            .Specular = BlendColor(FirstRowSpecular, Weight, LastRowSpecular, FullWeight - Weight),
        };
        for (size_t Column = 0; Column < Columns.size(); ++Column)
        {
            const FScoreColumn& Source = Columns[Column];
            const std::string Name(Record.Name.data(), size_t(std::ranges::find(Record.Name, '\0') - Record.Name.begin()));
            const std::string Text = (Source.Flags & 4) != 0 ? Name : FormatValue(Source.Format, (Source.Flags & ScoreRank) != 0 ? uint32_t(Row) : Record.Fields[Column]);
            if (!Source.Format.empty() || (Source.Flags & 4) != 0)
            {
                Cell(Column, Y, Text, Color);
            }
        }
    }

    return Out;
}

std::optional<FScoreStrip> ScoreHighlightStrip(const FMenuArt& Art, const size_t Highlight)
{
    if (Highlight == 0 || Highlight > ScoreRecordCount)
    {
        return std::nullopt;
    }

    constexpr int32_t TopInset = 2;
    constexpr int32_t BottomReach = 4;
    const int32_t Y = FirstRowY + int32_t(Highlight - 1) * RowStep;
    return FScoreStrip{ .Left = Columns.front().X, .Top = Y - Art.FontBaseline(ScoreFont) + TopInset, .Right = Columns.back().X, .Bottom = Y + BottomReach };
}

bool VerifyScores()
{
    FScoreTable Table{};
    const FScoreRecord Low = MakeScore(1, 1, 5, 1000, 3, 1, 60000, "Low");
    const FScoreRecord High = MakeScore(1, 2, 0, 0, 0, 0, 1000, "High");
    const FScoreRecord Tie = MakeScore(1, 1, 5, 1000, 3, 1, 1000, "Tie");
    const bool bRanks = InsertScore(Table, Low) == 1 && InsertScore(Table, High) == 1 && InsertScore(Table, Tie) == 2 && Table[2].Name[0] == 'L';
    const std::filesystem::path Path = std::filesystem::temp_directory_path() / "openvyruz-verify.sco";
    const bool bWritten = WriteScores(Path, Table);
    const FScoreTable Read = ReadScores(Path);
    std::error_code Error;
    std::filesystem::remove(Path, Error);
    return bRanks && bWritten && std::memcmp(Read.data(), Table.data(), sizeof(FScoreTable)) == 0;
}
