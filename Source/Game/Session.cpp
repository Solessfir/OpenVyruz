#include "Game/Session.h"

#include "Game/Collision.h"
#include "Pg/File.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <tuple>

namespace
{
// SessionWrite keeps the six sessions before the one it writes.
constexpr int KeptSessions = 7;
constexpr std::string_view LastFileName = "last.txt";
constexpr std::string_view DefaultFolderName = "default";
// Larger than any level's snapshot; a bigger file is not a session.
constexpr std::uintmax_t MaxSessionBytes = 16 * 1024 * 1024;

// dir_to_string at 00407850, by direction.
constexpr std::array<std::string_view, ObjectDirectionCount> DirectionNames{
    "SW", "SW_1", "SW_2", "S", "S_1", "S_2", "SE", "SE_1", "SE_2", "E", "E_1", "E_2", "NE", "NE_1", "NE_2", "N", "N_1", "N_2", "NW", "NW_1", "NW_2", "W", "W_1", "W_2",
};

std::string ToLowerCopy(const std::string_view Text)
{
    std::string Result(Text);
    std::ranges::transform(Result, Result.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
    return Result;
}

int Integer(const std::string_view Text)
{
    return int(ParseLevelFloat(Text).value_or(0.f));
}

FLevelPropertyBlock* FindChild(FLevelPropertyBlock& Properties, const std::string_view Kind, const std::string_view Name)
{
    for (FLevelPropertyBlock& Child : Properties.Children)
    {
        if (ToLowerCopy(Child.Kind) == Kind && ToLowerCopy(Child.Name) == ToLowerCopy(Name))
        {
            return &Child;
        }
    }

    return nullptr;
}

const FLevelPropertyBlock* FindChild(const FLevelPropertyBlock& Properties, const std::string_view Kind, const std::string_view Name)
{
    return FindChild(const_cast<FLevelPropertyBlock&>(Properties), Kind, Name);
}

// The class's Energy, Shield and the like: the Initial of its block, or the plain value.
float ClassValue(const FLevelPropertyBlock& Class, const std::string_view Field)
{
    for (const FLevelPropertyBlock& Child : Class.Children)
    {
        if (ToLowerCopy(Child.Kind) == ToLowerCopy(Field))
        {
            return ParseLevelFloat(Child.Find("Initial")).value_or(0.f);
        }
    }

    return ParseLevelFloat(Class.Find(Field)).value_or(0.f);
}

const FLevelPropertyBlock* FindClass(const FLevel& Level, const std::string_view ClassName)
{
    const auto Class = Level.Classes.find(ToLowerCopy(ClassName));
    return Class == Level.Classes.end() ? nullptr : &Class->second;
}

// The object's name (object +0xc4), which is its class's unless the .3do gave it one.
std::string_view ObjectName(const FLevelPropertyBlock& Properties)
{
    return Properties.Find("Name");
}

// write_3do_attributes (FUN_00417210) for one attribute block, with the live state the port keeps for gates and lifts.
void WriteAttribute(std::string& Out, const FLevelPropertyBlock& Block, const FLevelPropertyBlock* const ClassBlocks, const FDoor* const Door,
    const std::optional<int32_t> ItemTimeout)
{
    const std::string Kind = ToLowerCopy(Block.Kind);
    const std::string Name = ToLowerCopy(Block.Name);
    const auto Value = [&](const std::string_view Field) { return Integer(Block.Find(Field)); };
    const auto DoorFlag = [&](const std::string_view Field, const uint32_t Bit) { return Door ? int((Door->Flags & Bit) != 0) : Value(Field); };
    if (Kind == "type" && Name == "generator")
    {
        Out += std::format("\tTYPE Generator [\n\t\tProduct\t= {}\n\t\tMax-births\t= {}\n\t\tBirth-delay\t= {}\n\t\tBirth-duration\t= {}\n\t\tStressable\t= {}\n\t\tActive\t= {}\n"
                           "\t\tStable-population\t= {}\n\t]\n",
            Block.Find("Product"), Value("Max-births"), Value("Birth-delay"), Value("Birth-duration"), Value("Stressable"), Value("Active"), Value("Stable-population"));
    }
    else if (Kind == "type" && Name == "lift")
    {
        Out += std::format("\tTYPE Lift [\n\t\tPresent\t= {}\n\t\tActive\t= {}\n\t\tGoing-Up\t= {}\n\t\tDelay\t= {}\n\t\tStay-Up\t= {}\n\t\tStay-Down\t= {}\n\t\tUsage-Count\t= {}\n"
                           "\t\tFull-load\t= {}\n\t\tDestroy-key\t= {}\n\t\tKey-always-needed\t= {}\n\t\tId\t= {}\n\t\tNeeds-code\t= {}\n",
            DoorFlag("Present", DoorOpen), DoorFlag("Active", DoorActive), DoorFlag("Going-Up", DoorOpening), Value("Delay"), DoorFlag("Stay-Up", DoorStayOpen),
            DoorFlag("Stay-Down", DoorStayClosed), Door ? Door->UsageCount : Value("Usage-Count"), Value("Full-load"), Value("Destroy-key"), Value("Key-always-needed"),
            Value("Id"), Value("Needs-code"));
        if (const std::string_view Scenery = Block.Find("Scenery"); !Scenery.empty())
        {
            Out += std::format("\t\tScenery\t= {}\n", Scenery);
        }

        if (const std::string_view File = Block.Find("3do-file"); !File.empty())
        {
            Out += std::format("\t\t3do-file\t= {}\n", File);
        }

        Out += "\t]\n";
    }
    else if (Kind == "type" && Name == "gate")
    {
        Out += std::format("\tTYPE Gate [\n\t\tOpened\t= {}\n\t\tActive\t= {}\n\t\tClosing\t= {}\n\t\tSensor\t= {}\n\t\tDelay\t= {}\n\t\tStay-Opened\t= {}\n\t\tStay-Closed\t= {}\n"
                           "\t\tDestroy-key\t= {}\n\t\tKey-always-needed\t= {}\n\t\tId\t= {}\n\t\tNeeds-Code\t= {}\n\t]\n",
            DoorFlag("Opened", DoorOpen), DoorFlag("Active", DoorActive), DoorFlag("Closing", DoorOpening), Value("Sensor"), Value("Delay"), DoorFlag("Stay-Opened", DoorStayOpen),
            DoorFlag("Stay-Closed", DoorStayClosed), Value("Destroy-key"), Value("Key-always-needed"), Value("Id"), Value("Needs-Code"));
    }
    else if (Kind == "type" && Name == "key")
    {
        Out += Value("Is-Code") == 0 ? std::format("\tTYPE Key [\n\t\tId\t= {}\n\t]\n", Value("Id"))
                                     : std::format("\tTYPE Key [\n\t\tId\t= {}\n\t\tIs-Code\t= 1\n\t\tCode-Part-Id\t= {}\n\t\tCode-Part-Count\t= {}\n\t\tCode-String\t= {}\n\t]\n",
                                           Value("Id"), Value("Code-Part-Id"), Value("Code-Part-Count"), Block.Find("Code-String"));
    }
    // The original writes TYPE Guardian only while its location or centre is still unset (-1), which activation fills in, so a running level never has one.
    else if (Kind == "type" && Name == "patrol")
    {
        if (Value("Path-ID") != 0)
        {
            Out += std::format("\tTYPE Patrol [\n\t\tPath-ID\t= {}\n\t\tPoint-ID\t= {}\n\t\tReverse\t= {}\n\t\tClosed\t= {}\n\t\tLoop\t= {}\n\t]\n", Value("Path-ID"), Value("Point-ID"),
                Value("Reverse"), Value("Closed"), Value("Loop"));
        }
    }
    else if (Kind == "type" && Name == "item")
    {
        if (const int32_t Timeout = ItemTimeout.value_or(Value("Timeout")); Timeout != 0)
        {
            Out += std::format("\tTYPE Item [\n\t\tTimeout\t= {}\n\t]\n", Timeout);
        }
    }
    else if (Kind == "add" && Name == "ammo")
    {
        // Only a stock the class has not, with the same count and maximum.
        const bool bClassHas = ClassBlocks && std::ranges::any_of(ClassBlocks->Children, [&](const FLevelPropertyBlock& Class)
        {
            return ToLowerCopy(Class.Kind) == "add" && ToLowerCopy(Class.Name) == "ammo" && Class.Find("Name") == Block.Find("Name") && Integer(Class.Find("Count")) == Value("Count") &&
                Integer(Class.Find("Max")) == Value("Max");
        });
        if (!bClassHas)
        {
            Out += std::format("\tADD Ammo [\n\t\tName\t= {}\n\t\tCount\t= {}\n\t\tMax\t= {}\n\t]\n", Block.Find("Name"), Value("Count"), Value("Max"));
        }
    }
}

// What Level3doWriteObject writes before the attributes: the class, the position, a name of its own, the facing, and the energy and shield that differ from the class's.
void WriteObjectHead(std::string& Out, const FLevel& Level, const FLevelObject& Object, const float X, const float Y, const int Direction, const std::optional<float> Energy,
    const std::optional<float> Shield)
{
    Out += std::format("\nCREATE-OBJECT [\n\tClass-name\t= {}\n\tPosition\t= {:.3f}, {:.3f}\n", Object.ClassName, X, Y);
    const FLevelPropertyBlock* const Class = FindClass(Level, Object.ClassName);
    if (const std::string_view Name = ObjectName(Object.Properties); Class && !Name.empty() && Name != ObjectName(*Class))
    {
        Out += std::format("\tName\t= {}\n", Name);
    }

    if (Direction > 0 && Direction < ObjectDirectionCount)
    {
        Out += std::format("\tDir\t= {}\n", DirectionNames[size_t(Direction)]);
    }

    if (Class && Energy && *Energy != ClassValue(*Class, "Energy"))
    {
        Out += std::format("\tEnergy\t= {:.3f}\n", *Energy);
    }

    if (Class && Shield && *Shield != ClassValue(*Class, "Shield"))
    {
        Out += std::format("\tShield\t= {:.3f}\n", *Shield);
    }
}

// A carried item: its class's attributes, with a key's own timeout.
void WriteCarried(std::string& Out, const FLevel& Level, const std::string_view ClassName, const float X, const float Y, const std::optional<int32_t> Timeout)
{
    const FLevelPropertyBlock* const Class = FindClass(Level, ClassName);
    if (!Class)
    {
        return;
    }

    const FLevelObject Object{ .ClassName = std::string(ClassName), .Properties = *Class };
    WriteObjectHead(Out, Level, Object, X, Y, 0, std::nullopt, std::nullopt);
    for (const FLevelPropertyBlock& Block : Class->Children)
    {
        WriteAttribute(Out, Block, Class, nullptr, Timeout);
    }

    Out += "]\n";
}

std::vector<std::pair<int, std::filesystem::path>> NumberedSessions(const std::filesystem::path& PlayerFolder)
{
    std::vector<std::pair<int, std::filesystem::path>> Result;
    std::error_code Error;
    for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(PlayerFolder, Error))
    {
        const std::string Name = Entry.path().filename().string();
        int Number = 0;
        if (Entry.is_directory(Error) && Name.size() > 1 && (Name.front() == 's' || Name.front() == 'S') &&
            std::from_chars(Name.data() + 1, Name.data() + Name.size(), Number).ec == std::errc() && Number >= 0)
        {
            Result.emplace_back(Number, Entry.path());
        }
    }

    std::ranges::sort(Result, std::ranges::greater{}, &std::pair<int, std::filesystem::path>::first);
    return Result;
}

// The level's file name in the session, <zone>-<level>.<extension>.
std::filesystem::path SessionFile(const FSession& Session, const std::string_view Extension)
{
    std::string Name = Session.LevelPath;
    std::ranges::replace(Name, '\\', '-');
    return Session.Folder / std::format("{}.{}", Name, Extension);
}

std::optional<std::string> ReadSmallFile(const std::filesystem::path& Path)
{
    std::error_code Error;
    const std::uintmax_t Size = std::filesystem::file_size(Path, Error);
    if (Error || Size > MaxSessionBytes)
    {
        return std::nullopt;
    }

    std::ifstream File(Path, std::ios::binary);
    std::ostringstream Text;
    Text << File.rdbuf();
    return File ? std::optional(Text.str()) : std::nullopt;
}
}

std::string FormatSessionObjects(const FLevel& Level, const std::span<const FObjectBody> Bodies, const std::span<const FDoor> Doors, const FSessionPlayer& Player,
    const size_t DifficultyLevel)
{
    std::vector<const FObjectBody*> BodyOf(Level.Objects.size(), nullptr);
    for (const FObjectBody& Body : Bodies)
    {
        BodyOf[Body.ObjectIndex] = &Body;
    }

    std::vector<const FDoor*> DoorOf(Level.Objects.size(), nullptr);
    for (const FDoor& Door : Doors)
    {
        DoorOf[Door.ObjectIndex] = &Door;
    }

    const auto Position = [&](const size_t Index)
    {
        return Index == Player.ObjectIndex ? std::pair(Player.X, Player.Y) : std::pair(Level.Objects[Index].X, Level.Objects[Index].Y);
    };
    std::vector<size_t> Order;
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        if (!Object.bRemoved && ParseLevelFloat(Object.Properties.Find("No-saving")).value_or(0.f) == 0.f)
        {
            Order.push_back(Index);
        }
    }

    std::ranges::sort(Order, std::ranges::greater{}, [&](const size_t Index)
    {
        const auto [X, Y] = Position(Index);
        const FWorldCell Cell = CellOf({ .X = X, .Y = Y });
        return std::tuple(Cell.Y, Cell.X, Index);
    });

    std::string Out = "################\n1.3 # File format version\n################\n#\n";
    if (!Level.Name.empty())
    {
        Out += std::format("LEVEL-NAME [{}]\n", Level.Name);
    }

    int Tower = 0;
    int Floor = 0;
    if (const size_t Dot = Level.Id.find('.'); Dot != std::string::npos)
    {
        std::from_chars(Level.Id.data(), Level.Id.data() + Dot, Tower);
        std::from_chars(Level.Id.data() + Dot + 1, Level.Id.data() + Level.Id.size(), Floor);
    }

    if (Tower != 0 && Floor != 0)
    {
        Out += std::format("LEVEL-ID [ {}.{} ]\n", Tower, Floor);
    }

    Out += std::format("OBJECT-COUNT [ {} ]\n3DG-FILENAME [{}]\nDIFFICULTY-LEVEL\t= {}\n", Order.size(), Level.GraphicsName, DifficultyLevel);
    for (const size_t Index : Order)
    {
        const FLevelObject& Object = Level.Objects[Index];
        const FLevelPropertyBlock* const Class = FindClass(Level, Object.ClassName);
        if (Index != Player.ObjectIndex || !Player.Inventory)
        {
            const FObjectBody* const Body = BodyOf[Index];
            WriteObjectHead(Out, Level, Object, Object.X, Object.Y, Object.Direction == ObjectDirectionCount ? 0 : Object.Direction,
                Body ? std::optional(Body->Energy) : std::nullopt, Body ? std::optional(Body->Shield) : std::nullopt);
            for (const FLevelPropertyBlock& Block : Object.Properties.Children)
            {
                WriteAttribute(Out, Block, Class, DoorOf[Index], std::nullopt);
            }

            Out += "]\n";
            continue;
        }

        // The objects the player carries come first, newest first; a surprise in effect is written but not named as carried.
        const FPlayerInventory& Inventory = *Player.Inventory;
        std::vector<std::string> Have;
        for (const FCarriedSurprise& Surprise : Inventory.Surprises.Carried)
        {
            WriteCarried(Out, Level, Surprise.ClassName, Surprise.X, Surprise.Y, std::nullopt);
            if (const FLevelPropertyBlock* const SurpriseClass = FindClass(Level, Surprise.ClassName); SurpriseClass && !Surprise.bStarted)
            {
                Have.emplace_back(ObjectName(*SurpriseClass));
            }
        }

        for (const FCarriedKey& Key : std::views::reverse(Inventory.Keys))
        {
            WriteCarried(Out, Level, Key.ClassName, Key.X, Key.Y, Key.bHasTimeout ? std::optional(Key.Timeout) : std::nullopt);
            if (const FLevelPropertyBlock* const KeyClass = FindClass(Level, Key.ClassName))
            {
                Have.emplace_back(ObjectName(*KeyClass));
            }
        }

        WriteObjectHead(Out, Level, Object, Player.X, Player.Y, Player.Direction, Inventory.Energy, Inventory.Shield);
        for (const FLevelPropertyBlock& Block : Object.Properties.Children)
        {
            if (ToLowerCopy(Block.Kind) != "add" || ToLowerCopy(Block.Name) != "ammo")
            {
                WriteAttribute(Out, Block, Class, nullptr, std::nullopt);
            }
        }

        for (const FAmmoStock& Stock : Inventory.Ammo)
        {
            const FLevelPropertyBlock Ammo{
                .Kind = "ADD",
                .Name = "Ammo",
                .Fields = { { "Name", Stock.Name }, { "Count", std::to_string(int(Stock.Count)) }, { "Max", std::to_string(int(Stock.Max)) } },
            };
            WriteAttribute(Out, Ammo, Class, nullptr, std::nullopt);
        }

        for (const std::string& Name : Have)
        {
            Out += std::format("\tHAVE {}\n", Name);
        }

        Out += "]\n";
    }

    return Out;
}

std::vector<std::vector<size_t>> ResolveCarriedObjects(FLevel& Level)
{
    std::vector<std::vector<size_t>> Carried(Level.Objects.size());
    for (size_t Carrier = 0; Carrier < Level.Objects.size(); ++Carrier)
    {
        for (const std::string& Line : Level.Objects[Carrier].CarriedNames)
        {
            const size_t Blank = Line.find_first_of(" \t");
            const std::string_view Name = std::string_view(Line).substr(0, Blank);
            std::optional<size_t> Found;
            std::tuple<int, int, size_t> Best{};
            for (size_t Index = 0; Index < Carrier; ++Index)
            {
                const FLevelObject& Object = Level.Objects[Index];
                if (Object.bRemoved || ObjectName(Object.Properties) != Name)
                {
                    continue;
                }

                const FWorldCell Cell = CellOf({ .X = Object.X, .Y = Object.Y });
                if (const std::tuple Rank(Cell.Y, Cell.X, Index); !Found || Rank > Best)
                {
                    Found = Index;
                    Best = Rank;
                }
            }

            if (!Found)
            {
                continue;
            }

            FLevelObject& Object = Level.Objects[*Found];
            Object.bRemoved = true;
            Carried[Carrier].push_back(*Found);
            // "HAVE <name> <timeout>" sets the carried item's timeout.
            if (Blank != std::string::npos)
            {
                if (FLevelPropertyBlock* const Item = FindChild(Object.Properties, "type", "Item"))
                {
                    const std::string_view Count = std::string_view(Line).substr(Line.find_first_not_of(" 	", Blank));
                    Item->Fields.emplace_back("Timeout", std::string(Count));
                }
            }
        }
    }

    return Carried;
}

std::filesystem::path PlayerSessionFolder(const std::filesystem::path& GameFolder, const std::string_view PlayerName)
{
    return GameFolder / "session" / std::filesystem::path(PlayerName);
}

std::filesystem::path DefaultSessionFolder(const std::filesystem::path& PlayerFolder)
{
    return PlayerFolder / DefaultFolderName;
}

std::string FormatLastText(const FSession& Session, const std::string_view RelativeFolder)
{
    const size_t Separator = Session.LevelPath.rfind('\\');
    const std::string_view Zone = std::string_view(Session.LevelPath).substr(0, Separator == std::string::npos ? 0 : Separator);
    const std::string_view Level = std::string_view(Session.LevelPath).substr(Separator == std::string::npos ? 0 : Separator + 1);
    return std::format("#0 1\n{}\n{}\n{}.3di\n{}\n{}\n{:.3f}\n{:.3f}\n{}\n{}\n{}\n{}\n{}\n{}\n{}\n", RelativeFolder, Zone, Level, Session.LevelName, int32_t(Session.Clock),
        Session.Energy, Session.Shield, Session.WeaponSlot, Session.DifficultyLevel, Session.ShotsHit, Session.ShotsMissed, Session.Kills, Session.Earned, Session.Money);
}

std::optional<FSession> ParseLastText(const std::string_view Text)
{
    std::vector<std::string_view> Lines;
    for (const auto Part : std::views::split(Text, '\n'))
    {
        std::string_view Line(Part.begin(), Part.end());
        if (Line.ends_with('\r'))
        {
            Line.remove_suffix(1);
        }

        Lines.push_back(Line);
    }

    enum ELine : size_t
    {
        HeaderLine,
        FolderLine,
        ZoneLine,
        FileLine,
        NameLine,
        ClockLine,
        EnergyLine,
        ShieldLine,
        SlotLine,
        DifficultyLine,
        HitsLine,
        MissesLine,
        KillsLine,
        EarnedLine,
        MoneyLine,
        LineCount,
    };
    if (Lines.size() < LineCount || !Lines[HeaderLine].starts_with('#'))
    {
        return std::nullopt;
    }

    const auto Number = [&](const size_t Line, auto& Out)
    {
        const std::string_view Value = Lines[Line];
        return std::from_chars(Value.data(), Value.data() + Value.size(), Out).ec == std::errc();
    };
    FSession Session;
    int Clock = 0;
    int Slot = 0;
    int Difficulty = 0;
    const std::string_view File = Lines[FileLine];
    const size_t Dot = File.rfind('.');
    if (!Number(ClockLine, Clock) || !Number(EnergyLine, Session.Energy) || !Number(ShieldLine, Session.Shield) || !Number(SlotLine, Slot) ||
        !Number(DifficultyLine, Difficulty) || !Number(HitsLine, Session.ShotsHit) || !Number(MissesLine, Session.ShotsMissed) || !Number(KillsLine, Session.Kills) ||
        !Number(EarnedLine, Session.Earned) || !Number(MoneyLine, Session.Money) || Lines[ZoneLine].empty() || Dot == 0 || Dot == std::string_view::npos ||
        Difficulty < 0 || Difficulty >= int(DifficultyLevelCount) || Slot < 0)
    {
        return std::nullopt;
    }

    Session.LevelPath = std::format("{}\\{}", ToLowerCopy(Lines[ZoneLine]), ToLowerCopy(File.substr(0, Dot)));
    Session.LevelName = std::string(Lines[NameLine]);
    Session.Clock = uint32_t(Clock);
    Session.WeaponSlot = size_t(Slot);
    Session.DifficultyLevel = size_t(Difficulty);
    return Session;
}

std::optional<FSession> ReadSession(const std::filesystem::path& Folder)
{
    const std::optional<std::string> Text = ReadSmallFile(Folder / LastFileName);
    std::optional<FSession> Session = Text ? ParseLastText(*Text) : std::nullopt;
    if (Session)
    {
        Session->Folder = Folder;
    }

    return Session;
}

std::optional<std::string> ReadSessionObjects(const FSession& Session)
{
    return ReadSmallFile(SessionFile(Session, "3do"));
}

std::vector<FSession> ReadSavedSessions(const std::filesystem::path& PlayerFolder)
{
    std::vector<FSession> Sessions;
    for (const auto& [Number, Folder] : NumberedSessions(PlayerFolder))
    {
        if (std::optional<FSession> Session = ReadSession(Folder))
        {
            Sessions.push_back(std::move(*Session));
        }
    }

    return Sessions;
}

std::optional<std::vector<uint16_t>> ReadSessionThumbnail(const FSession& Session)
{
    // An uncompressed 16-bit Targa (image type 2) stored from the bottom row up.
    constexpr size_t HeaderBytes = 18;
    constexpr uint8_t TrueColor = 2;
    constexpr size_t TypeOffset = 2;
    constexpr size_t WidthOffset = 12;
    constexpr size_t HeightOffset = 14;
    constexpr size_t DepthOffset = 16;
    constexpr uint8_t Depth = 16;
    const std::optional<std::string> File = ReadSmallFile(SessionFile(Session, "tga"));
    constexpr size_t PixelCount = size_t(ThumbnailWidth) * ThumbnailHeight;
    const auto Word = [&](const size_t Offset) { return uint32_t(uint8_t((*File)[Offset])) | uint32_t(uint8_t((*File)[Offset + 1])) << 8; };
    if (!File || File->size() < HeaderBytes + PixelCount * 2 || uint8_t((*File)[TypeOffset]) != TrueColor || Word(WidthOffset) != ThumbnailWidth ||
        Word(HeightOffset) != ThumbnailHeight || uint8_t((*File)[DepthOffset]) != Depth)
    {
        return std::nullopt;
    }

    const size_t Start = HeaderBytes + uint8_t((*File)[0]);
    if (File->size() < Start + PixelCount * 2)
    {
        return std::nullopt;
    }

    std::vector<uint16_t> Pixels(PixelCount);
    for (uint32_t Row = 0; Row < ThumbnailHeight; ++Row)
    {
        for (uint32_t Column = 0; Column < ThumbnailWidth; ++Column)
        {
            Pixels[size_t(ThumbnailHeight - 1 - Row) * ThumbnailWidth + Column] = uint16_t(Word(Start + (size_t(Row) * ThumbnailWidth + Column) * 2));
        }
    }

    return Pixels;
}

bool WriteSession(const std::filesystem::path& PlayerFolder, const bool bDefault, const FSession& Session, const std::string_view Objects,
    const std::span<const uint16_t> Thumbnail)
{
    std::filesystem::path Folder = DefaultSessionFolder(PlayerFolder);
    std::string Leaf(DefaultFolderName);
    std::error_code Error;
    if (!bDefault)
    {
        const std::vector<std::pair<int, std::filesystem::path>> Existing = NumberedSessions(PlayerFolder);
        const int Next = Existing.empty() ? 0 : Existing.front().first + 1;
        for (const auto& [Number, Path] : Existing)
        {
            if (Number <= Next - KeptSessions)
            {
                std::filesystem::remove_all(Path, Error);
            }
        }

        Leaf = std::format("s{:03}", Next);
        Folder = PlayerFolder / Leaf;
    }

    std::filesystem::create_directories(Folder, Error);
    FSession Written = Session;
    Written.Folder = Folder;
    const std::string Relative = std::format("session\\{}\\{}", PlayerFolder.filename().string(), Leaf);
    bool bWritten = WriteFileText(SessionFile(Written, "3do"), Objects) && WriteFileText(Folder / LastFileName, FormatLastText(Written, Relative));
    constexpr size_t PixelCount = size_t(ThumbnailWidth) * ThumbnailHeight;
    if (!bDefault && Thumbnail.size() == PixelCount)
    {
        std::string Tga(18, '\0');
        constexpr uint8_t TrueColor = 2;
        constexpr uint8_t Depth = 16;
        Tga[2] = char(TrueColor);
        Tga[12] = char(ThumbnailWidth & 0xff);
        Tga[13] = char(ThumbnailWidth >> 8);
        Tga[14] = char(ThumbnailHeight & 0xff);
        Tga[15] = char(ThumbnailHeight >> 8);
        Tga[16] = char(Depth);
        for (uint32_t Row = ThumbnailHeight; Row-- > 0;)
        {
            for (uint32_t Column = 0; Column < ThumbnailWidth; ++Column)
            {
                const uint16_t Pixel = Thumbnail[size_t(Row) * ThumbnailWidth + Column];
                Tga += char(Pixel & 0xff);
                Tga += char(Pixel >> 8);
            }
        }

        bWritten = WriteFileText(SessionFile(Written, "tga"), Tga) && bWritten;
    }

    return bWritten;
}

std::filesystem::path UserDataDirectory()
{
    constexpr std::string_view Application = "OpenVyruz";
#ifdef _WIN32
    wchar_t* AppData = nullptr;
    size_t Length = 0;
    if (_wdupenv_s(&AppData, &Length, L"APPDATA") == 0 && AppData)
    {
        std::filesystem::path Result = std::filesystem::path(AppData) / Application;
        std::free(AppData);
        return Result;
    }
#else
    if (const char* const DataHome = std::getenv("XDG_DATA_HOME"); DataHome && *DataHome)
    {
        return std::filesystem::path(DataHome) / Application;
    }

    if (const char* const Home = std::getenv("HOME"); Home && *Home)
    {
        return std::filesystem::path(Home) / ".local" / "share" / Application;
    }
#endif
    return std::filesystem::path(Application);
}

std::expected<std::string, std::string> VerifySessions(FResourceArchive& Archive)
{
    // Every seventh object is gone, as collected or destroyed.
    constexpr size_t RemovedEvery = 7;
    constexpr float CreatedOffset = 10.f;
    constexpr size_t DifficultyLevel = 2;
    size_t Levels = 0;
    size_t Objects = 0;
    for (const std::string& Name : Archive.GetNames())
    {
        if (!Name.ends_with(".3do"))
        {
            continue;
        }

        const std::string LevelPath = Name.substr(0, Name.size() - 4);
        std::expected<FLevel, std::string> Level = LoadLevel(Archive, LevelPath);
        if (!Level || Level->Objects.empty())
        {
            return std::unexpected(std::format("{}: {}", LevelPath, !Level ? Level.error() : "no objects"));
        }

        const std::string CreatedClass = Level->Objects.front().ClassName;
        const std::optional<size_t> Created = CreateLevelObject(*Level, CreatedClass, Level->Objects.front().X + CreatedOffset, Level->Objects.front().Y);
        const std::optional<size_t> PlayerFound = FindPlayerObject(*Level);
        const auto PlayerObject = PlayerFound ? Level->Objects.begin() + std::ptrdiff_t(*PlayerFound) : Level->Objects.end();
        // A key class none of the level's objects has, so its HAVE line can only find the carried one.
        const auto KeyClass = std::ranges::find_if(Level->Classes, [&](const auto& Class)
        {
            const auto Template = Level->ClassObjects.find(Class.first);
            return FindChild(Class.second, "type", "Key") && FindChild(Class.second, "type", "Item") && Template != Level->ClassObjects.end() &&
                Template->second.SubType == "item" && ParseLevelFloat(Class.second.Find("Indestructible")).value_or(0.f) != 0.f &&
                std::ranges::none_of(Level->Objects, [&](const FLevelObject& Object) { return ObjectName(Object.Properties) == Class.second.Find("Name"); });
        });
        if (!Created || PlayerObject == Level->Objects.end())
        {
            continue;
        }

        for (size_t Index = 0; Index < Level->Objects.size(); ++Index)
        {
            Level->Objects[Index].bRemoved = Index % RemovedEvery == RemovedEvery - 1 && Level->Objects[Index].Behaviour != "Player";
        }

        std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
        std::expected<std::vector<FDoor>, std::string> Doors = CreateDoors(*Level, Archive, Bodies);
        if (!Doors)
        {
            return std::unexpected(std::format("{}: {}", LevelPath, Doors.error()));
        }

        for (FDoor& Door : *Doors)
        {
            Door.Flags ^= DoorOpen;
            ++Door.UsageCount;
        }

        const size_t PlayerIndex = size_t(PlayerObject - Level->Objects.begin());
        FPlayerInventory Inventory = CreateInventory(*PlayerObject);
        if (!Inventory.Ammo.empty())
        {
            Inventory.Ammo.front().Count = std::max(0.f, Inventory.Ammo.front().Count - 1.f);
        }

        if (KeyClass != Level->Classes.end())
        {
            Inventory.Keys.push_back({ .ClassName = std::string(KeyClass->second.Find("Name")), .X = PlayerObject->X, .Y = PlayerObject->Y });
        }

        const FSessionPlayer Player{ .ObjectIndex = PlayerIndex, .X = PlayerObject->X, .Y = PlayerObject->Y, .Direction = 1, .Inventory = &Inventory };
        const std::string Text = FormatSessionObjects(*Level, Bodies, *Doors, Player, DifficultyLevel);

        // Loaded back, the level and the player write the same session.
        std::expected<FLevel, std::string> Read = LoadLevel(Archive, LevelPath, Text);
        if (!Read)
        {
            return std::unexpected(std::format("{}: the session does not load: {}", LevelPath, Read.error()));
        }

        const std::vector<std::vector<size_t>> Carried = ResolveCarriedObjects(*Read);
        // The carrier: with two players in one cell (HITEK\T1H1) the reload makes the other one the player, as in the original.
        const auto Carrier = std::ranges::find_if(Carried, [](const std::vector<size_t>& Objects) { return !Objects.empty(); });
        const std::optional<size_t> ReadFound = Carrier != Carried.end() ? std::optional(size_t(Carrier - Carried.begin())) : FindPlayerObject(*Read);
        const auto ReadPlayer = ReadFound ? Read->Objects.begin() + std::ptrdiff_t(*ReadFound) : Read->Objects.end();
        if (ReadPlayer == Read->Objects.end() || Read->DifficultyLevel != int(DifficultyLevel))
        {
            return std::unexpected(std::format("{}: the session lost its player or difficulty", LevelPath));
        }

        const size_t ReadIndex = size_t(ReadPlayer - Read->Objects.begin());
        FPlayerInventory ReadInventory = CreateInventory(*ReadPlayer);
        for (const size_t Index : Carried[ReadIndex])
        {
            if (const std::optional<FPickup> Pickup = CreatePickup(*Read, Index); Pickup && Pickup->Key)
            {
                ReadInventory.Keys.push_back(*Pickup->Key);
            }
        }

        if (ReadInventory.Keys.size() != Inventory.Keys.size())
        {
            return std::unexpected(std::format("{}: the carried key is lost", LevelPath));
        }

        std::vector<FObjectBody> ReadBodies = CreateObjectBodies(*Read);
        std::expected<std::vector<FDoor>, std::string> ReadDoors = CreateDoors(*Read, Archive, ReadBodies);
        const FSessionPlayer Again{ .ObjectIndex = ReadIndex, .X = ReadPlayer->X, .Y = ReadPlayer->Y, .Direction = ReadPlayer->Direction, .Inventory = &ReadInventory };
        // A loaded level lists each cell's objects the other way round, so the blocks are compared in any order.
        const auto Blocks = [](const std::string_view Session)
        {
            std::vector<std::string> Result;
            for (const auto Part : std::views::split(Session, std::string_view("\nCREATE-OBJECT [")))
            {
                Result.emplace_back(Part.begin(), Part.end());
            }

            std::ranges::sort(Result);
            return Result;
        };
        if (!ReadDoors || Blocks(FormatSessionObjects(*Read, ReadBodies, *ReadDoors, Again, DifficultyLevel)) != Blocks(Text))
        {
            return std::unexpected(std::format("{}: the loaded session writes differently", LevelPath));
        }

        ++Levels;
        Objects += Read->Objects.size();
    }

    // last.txt reads back.
    const FSession Last{ .LevelPath = "underground\\t1u1", .LevelName = "Entering Tower 1", .Clock = 5216, .Energy = 5000.f, .Shield = 4999.5f, .WeaponSlot = 3, .Kills = 4 };
    const std::string LastText = FormatLastText(Last, "session\\Player\\s000");
    if (const std::optional<FSession> Parsed = ParseLastText(LastText); !Parsed || FormatLastText(*Parsed, "session\\Player\\s000") != LastText)
    {
        return std::unexpected("last.txt does not read back");
    }

    // CAVE\T1C1's first saving plot, with the player on it.
    std::expected<FLevel, std::string> Cave = LoadLevel(Archive, "cave\\t1c1");
    const std::vector<FSavingPlot> Plots = Cave ? FindSavingPlots(*Cave) : std::vector<FSavingPlot>{};
    const auto Plot = std::ranges::find_if(Plots, [](const FSavingPlot& Saving) { return Saving.Plot.has_value(); });
    if (Plot == Plots.end())
    {
        return std::unexpected("cave\\t1c1 has no saving plot");
    }

    constexpr float Cost = 100000.f;
    constexpr float PlayerRadius = 20.f;
    constexpr uint32_t StepMilliseconds = 100;
    constexpr uint32_t WaitMilliseconds = 7000;
    const FLevelObject& Zone = Cave->Objects[Plot->Zone];
    FPlayerState Player;
    Player.Position = { .X = Zone.X, .Y = Zone.Y };
    Player.Center = CollisionFromWorld(Player.Position);
    FMessages Messages;
    const auto Stand = [&](FPlayerInventory& Inventory)
    {
        FSavingState State;
        std::vector<uint32_t> Saves;
        for (uint32_t Time = 1; Time < WaitMilliseconds; Time += StepMilliseconds)
        {
            if (StepSavingPlots(State, Plots, *Cave, Player, PlayerRadius, Inventory, Messages, Time, "Player"))
            {
                Saves.push_back(Time);
            }
        }

        return Saves;
    };
    FPlayerInventory Poor{ .Energy = 1.f };
    FPlayerInventory Rich{ .Energy = 1.f, .Money = Cost };
    const std::vector<uint32_t> PoorSaves = Stand(Poor);
    const std::vector<uint32_t> RichSaves = Stand(Rich);
    constexpr uint32_t SaveDelay = 5500;
    if (!PoorSaves.empty() || RichSaves.size() != 1 || RichSaves.front() < SaveDelay || Rich.Money != 0.f)
    {
        return std::unexpected("the saving plot did not save once after 5.5 seconds for its cost");
    }

    return std::format("{} levels ({} objects) written and loaded back as sessions, saving plot passed", Levels, Objects);
}
