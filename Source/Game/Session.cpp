#include "Game/Session.h"

#include "Game/Collision.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <format>
#include <fstream>
#include <ranges>
#include <sstream>
#include <tuple>

namespace
{
constexpr std::string_view Signature = "OpenVyruz-session";
constexpr int FormatVersion = 1;
constexpr char Separator = '\t';
// SessionWrite keeps the six sessions before the one it writes.
constexpr int KeptSessions = 7;
constexpr std::string_view SessionExtension = ".txt";

// Strings go between tab separators, so tabs, line breaks and backslashes are escaped.
std::string Escape(const std::string_view Text)
{
    std::string Result;
    for (const char Character : Text)
    {
        switch (Character)
        {
        case '\\':
            Result += "\\\\";
            break;
        case '\t':
            Result += "\\t";
            break;
        case '\n':
            Result += "\\n";
            break;
        case '\r':
            Result += "\\r";
            break;
        default:
            Result += Character;
        }
    }

    return Result;
}

std::optional<std::string> Unescape(const std::string_view Text)
{
    std::string Result;
    for (size_t Index = 0; Index < Text.size(); ++Index)
    {
        if (Text[Index] != '\\')
        {
            Result += Text[Index];
            continue;
        }

        if (++Index == Text.size())
        {
            return std::nullopt;
        }

        switch (Text[Index])
        {
        case '\\':
            Result += '\\';
            break;
        case 't':
            Result += '\t';
            break;
        case 'n':
            Result += '\n';
            break;
        case 'r':
            Result += '\r';
            break;
        default:
            return std::nullopt;
        }
    }

    return Result;
}

// One line of tab-separated fields, read in order.
class FFields
{
public:
    explicit FFields(const std::string_view Line)
    {
        for (const auto Part : std::views::split(Line, Separator))
        {
            Parts.emplace_back(Part.begin(), Part.end());
        }
    }

    std::string_view Kind() const { return Parts.empty() ? std::string_view() : Parts.front(); }
    bool AtEnd() const { return Next == Parts.size(); }

    std::optional<std::string> Text()
    {
        return Next < Parts.size() ? Unescape(Parts[Next++]) : std::nullopt;
    }

    template <typename T>
    std::optional<T> Number()
    {
        if (Next >= Parts.size())
        {
            return std::nullopt;
        }

        const std::string_view Part = Parts[Next++];
        T Value{};
        const auto [End, Error] = std::from_chars(Part.data(), Part.data() + Part.size(), Value);
        return Error == std::errc() && End == Part.data() + Part.size() ? std::optional(Value) : std::nullopt;
    }

    std::optional<bool> Flag()
    {
        const std::optional<int> Value = Number<int>();
        return Value && (*Value == 0 || *Value == 1) ? std::optional(*Value == 1) : std::nullopt;
    }

private:
    std::vector<std::string_view> Parts;
    // The kind is the first part.
    size_t Next = 1;
};

// Floats print in their shortest round-trip form, so a session reads back exactly.
template <typename... TArgs>
void Line(std::string& Out, const std::string_view Kind, const TArgs&... Args)
{
    Out += Kind;
    ((Out += Separator, Out += std::format("{}", Args)), ...);
    Out += '\n';
}

void WriteWeapon(std::string& Out, const std::string_view Kind, const FWeaponState& Weapon)
{
    Line(Out, Kind, Weapon.Weapon, Weapon.LastFireTime, int(Weapon.bTriggerHeld), Weapon.Slope, int(Weapon.bAlternateSide));
}

std::optional<FWeaponState> ReadWeapon(FFields& Fields)
{
    const std::optional<size_t> Weapon = Fields.Number<size_t>();
    const std::optional<uint32_t> LastFireTime = Fields.Number<uint32_t>();
    const std::optional<bool> bTriggerHeld = Fields.Flag();
    const std::optional<float> Slope = Fields.Number<float>();
    const std::optional<bool> bAlternateSide = Fields.Flag();
    if (!Weapon || !LastFireTime || !bTriggerHeld || !Slope || !bAlternateSide)
    {
        return std::nullopt;
    }

    return FWeaponState{ .Weapon = *Weapon, .LastFireTime = *LastFireTime, .bTriggerHeld = *bTriggerHeld, .Slope = *Slope, .bAlternateSide = *bAlternateSide };
}

std::string ToLowerCopy(const std::string_view Text)
{
    std::string Result(Text);
    std::ranges::transform(Result, Result.begin(), [](const unsigned char Character) { return char(std::tolower(Character)); });
    return Result;
}

FLevelPropertyBlock* FindTypeChild(FLevelPropertyBlock& Properties, const std::string_view Name)
{
    for (FLevelPropertyBlock& Child : Properties.Children)
    {
        if (ToLowerCopy(Child.Kind) == "type" && ToLowerCopy(Child.Name) == ToLowerCopy(Name))
        {
            return &Child;
        }
    }

    return nullptr;
}

std::optional<int> SessionNumber(const std::filesystem::path& Path)
{
    constexpr char Prefix = 's';
    const std::string Stem = Path.stem().string();
    if (Path.extension() != SessionExtension || Stem.size() < 2 || Stem.front() != Prefix)
    {
        return std::nullopt;
    }

    int Number = 0;
    const auto [End, Error] = std::from_chars(Stem.data() + 1, Stem.data() + Stem.size(), Number);
    return Error == std::errc() && End == Stem.data() + Stem.size() && Number >= 0 ? std::optional(Number) : std::nullopt;
}

std::vector<std::pair<int, std::filesystem::path>> NumberedSessions(const std::filesystem::path& Directory)
{
    std::vector<std::pair<int, std::filesystem::path>> Result;
    std::error_code Error;
    for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Directory, Error))
    {
        if (const std::optional<int> Number = SessionNumber(Entry.path()))
        {
            Result.emplace_back(*Number, Entry.path());
        }
    }

    std::ranges::sort(Result, std::ranges::greater{}, &std::pair<int, std::filesystem::path>::first);
    return Result;
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
}

std::vector<FSessionObject> CaptureObjects(const FLevel& Level, const std::span<const FObjectBody> Bodies, const std::span<const FDoor> Doors,
    std::vector<FCarriedKey>& Keys)
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

    std::vector<size_t> Order;
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        if (!Level.Objects[Index].bRemoved || std::ranges::contains(Keys, Index, &FCarriedKey::ObjectIndex))
        {
            Order.push_back(Index);
        }
    }

    std::ranges::sort(Order, std::ranges::greater{}, [&](const size_t Index)
    {
        const FWorldCell Cell = CellOf({ .X = Level.Objects[Index].X, .Y = Level.Objects[Index].Y });
        return std::tuple(Cell.Y, Cell.X, Index);
    });

    std::vector<FSessionObject> Result;
    std::vector<size_t> NewIndex(Level.Objects.size(), SIZE_MAX);
    for (const size_t Index : Order)
    {
        const FLevelObject& Object = Level.Objects[Index];
        FSessionObject Saved{
            .FileIndex = Object.FileIndex,
            .ClassName = Object.ClassName,
            .X = Object.X,
            .Y = Object.Y,
            .Altitude = Object.Altitude,
            .Direction = Object.Direction,
            .TopPose = Object.TopPose,
            .bWrecked = Object.bWrecked,
            .bRemoved = Object.bRemoved,
        };
        if (const FObjectBody* const Body = BodyOf[Index])
        {
            Saved.Energy = Body->Energy;
            Saved.Shield = Body->Shield;
        }

        if (const FDoor* const Door = DoorOf[Index])
        {
            Saved.DoorFlags = Door->Flags;
            Saved.UsageCount = Door->UsageCount;
        }

        NewIndex[Index] = Result.size();
        Result.push_back(std::move(Saved));
    }

    for (FCarriedKey& Key : Keys)
    {
        Key.ObjectIndex = Key.ObjectIndex < NewIndex.size() ? NewIndex[Key.ObjectIndex] : SIZE_MAX;
    }

    return Result;
}

void RestoreObjects(FLevel& Level, const std::span<const FSessionObject> Objects)
{
    const std::vector<FLevelObject> FileObjects = std::exchange(Level.Objects, {});
    for (const FSessionObject& Saved : Objects)
    {
        const bool bFromFile = Saved.FileIndex < FileObjects.size() && ToLowerCopy(FileObjects[Saved.FileIndex].ClassName) == ToLowerCopy(Saved.ClassName);
        if (bFromFile)
        {
            Level.Objects.push_back(FileObjects[Saved.FileIndex]);
        }
        else if (!CreateLevelObject(Level, Saved.ClassName, Saved.X, Saved.Y))
        {
            continue;
        }

        FLevelObject& Object = Level.Objects.back();
        Object.X = Saved.X;
        Object.Y = Saved.Y;
        Object.Altitude = Saved.Altitude;
        Object.Direction = Saved.Direction;
        Object.TopPose = Saved.TopPose;
        Object.bWrecked = Saved.bWrecked;
        Object.bRemoved = Saved.bRemoved;
        // The writer's Energy and Shield fields, which CreateObjectBody reads over the class's.
        if (Saved.Energy)
        {
            Object.Properties.Fields.emplace_back("Energy", std::format("{}", *Saved.Energy));
        }

        if (Saved.Shield)
        {
            Object.Properties.Fields.emplace_back("Shield", std::format("{}", *Saved.Shield));
        }

        // FUN_00417210's TYPE Gate and TYPE Lift fields, which CreateDoors reads.
        const bool bLift = Object.SubType == "lift";
        FLevelPropertyBlock* const Door = Saved.DoorFlags ? FindTypeChild(Object.Properties, bLift ? "Lift" : "Gate") : nullptr;
        if (Door)
        {
            const auto Set = [&](const std::string_view Field, const uint32_t Bit) { Door->Fields.emplace_back(Field, (*Saved.DoorFlags & Bit) != 0 ? "1" : "0"); };
            Set("Active", DoorActive);
            Set(bLift ? "Present" : "Opened", DoorOpen);
            Set(bLift ? "Going-up" : "Closing", DoorOpening);
            Set(bLift ? "Stay-up" : "Stay-opened", DoorStayOpen);
            Set(bLift ? "Stay-down" : "Stay-closed", DoorStayClosed);
            Door->Fields.emplace_back("Usage-count", std::to_string(Saved.UsageCount));
        }
    }
}

std::string FormatSession(const FSession& Session)
{
    std::string Out;
    Line(Out, Signature, FormatVersion);
    Line(Out, "level", Escape(Session.LevelPath), Escape(Session.LevelName));
    Line(Out, "summary", Session.Clock, Session.Energy, Session.Shield);
    if (const std::optional<FCarriedPlayer>& Player = Session.Player)
    {
        const FPlayerState& State = Player->State;
        Line(Out, "player", Player->Clock, State.Direction, State.Pose, State.TargetPose, State.TurnTimer, State.Speed, State.SpeedMagnitude);
        const FPlayerInventory& Inventory = Player->Inventory;
        Line(Out, "inventory", Inventory.Energy, Inventory.EnergyMax, Inventory.Shield, Inventory.ShieldMax, Inventory.Money, Inventory.Kills);
        for (const FAmmoStock& Ammo : Inventory.Ammo)
        {
            Line(Out, "ammo", Escape(Ammo.Name), Ammo.Count, Ammo.Max);
        }

        for (const FCarriedKey& Key : Inventory.Keys)
        {
            Line(Out, "key", Key.Id, int(Key.bCode), Key.PartId, Key.PartCount, Key.ObjectIndex, int(Key.bHasTimeout), Key.Timeout, Key.WarnTimeout, Key.WarnX,
                Key.WarnY, Key.ReactivateAfter, Escape(Key.WarnFormat));
        }

        Line(Out, "slot", Player->WeaponSlot);
        WriteWeapon(Out, "weapon", Player->Weapon);
        for (const FWeaponState& Slot : Player->WeaponSlots)
        {
            WriteWeapon(Out, "slot-weapon", Slot);
        }
    }

    if (Session.Objects)
    {
        Line(Out, "objects", Session.Objects->size());
        for (const FSessionObject& Object : *Session.Objects)
        {
            const std::string FileIndex = Object.FileIndex == SIZE_MAX ? "-" : std::to_string(Object.FileIndex);
            Line(Out, "object", FileIndex, Escape(Object.ClassName), Object.X, Object.Y, Object.Altitude, Object.Direction, Object.TopPose, int(Object.bWrecked),
                int(Object.bRemoved), Object.Energy ? std::format("{}", *Object.Energy) : "-", Object.Shield ? std::format("{}", *Object.Shield) : "-",
                Object.DoorFlags ? std::to_string(*Object.DoorFlags) : "-", Object.UsageCount);
        }
    }

    return Out;
}

std::optional<FSession> ParseSession(const std::string_view Text)
{
    FSession Session;
    bool bSigned = false;
    bool bLevel = false;
    size_t ExpectedObjects = 0;
    // An optional number written as '-' when absent.
    const auto OptionalNumber = [](FFields& Fields, auto Type) -> std::optional<std::optional<decltype(Type)>>
    {
        using T = decltype(Type);
        if (const std::optional<std::string> Raw = Fields.Text(); Raw && *Raw == "-")
        {
            return std::optional<std::optional<T>>(std::in_place);
        }
        else if (Raw)
        {
            T Value{};
            const auto [End, Error] = std::from_chars(Raw->data(), Raw->data() + Raw->size(), Value);
            if (Error == std::errc() && End == Raw->data() + Raw->size())
            {
                return std::optional<std::optional<T>>(Value);
            }
        }

        return std::nullopt;
    };

    for (const auto Part : std::views::split(Text, '\n'))
    {
        std::string_view Row(Part.begin(), Part.end());
        if (!Row.empty() && Row.back() == '\r')
        {
            Row.remove_suffix(1);
        }

        if (Row.empty())
        {
            continue;
        }

        FFields Fields(Row);
        const std::string_view Kind = Fields.Kind();
        bool bValid = true;
        if (!bSigned)
        {
            bSigned = Kind == Signature && Fields.Number<int>() == FormatVersion;
            bValid = bSigned;
        }
        else if (Kind == "level")
        {
            const std::optional<std::string> Path = Fields.Text();
            const std::optional<std::string> Name = Fields.Text();
            bValid = Path && Name && !Path->empty();
            Session.LevelPath = Path.value_or("");
            Session.LevelName = Name.value_or("");
            bLevel = bValid;
        }
        else if (Kind == "summary")
        {
            const std::optional<uint32_t> Clock = Fields.Number<uint32_t>();
            const std::optional<float> Energy = Fields.Number<float>();
            const std::optional<float> Shield = Fields.Number<float>();
            bValid = Clock && Energy && Shield;
            Session.Clock = Clock.value_or(0);
            Session.Energy = Energy.value_or(0.f);
            Session.Shield = Shield.value_or(0.f);
        }
        else if (Kind == "player")
        {
            FCarriedPlayer& Player = Session.Player.emplace();
            const std::optional<uint32_t> Clock = Fields.Number<uint32_t>();
            const std::optional<int> Direction = Fields.Number<int>();
            const std::optional<int> Pose = Fields.Number<int>();
            const std::optional<int> TargetPose = Fields.Number<int>();
            const std::optional<int> TurnTimer = Fields.Number<int>();
            const std::optional<float> Speed = Fields.Number<float>();
            const std::optional<float> SpeedMagnitude = Fields.Number<float>();
            bValid = Clock && Direction && Pose && TargetPose && TurnTimer && Speed && SpeedMagnitude;
            Player.Clock = Clock.value_or(1);
            Player.State = {
                .Speed = Speed.value_or(0.f),
                .SpeedMagnitude = SpeedMagnitude.value_or(0.f),
                .Direction = Direction.value_or(0),
                .Pose = Pose.value_or(1),
                .TargetPose = TargetPose.value_or(1),
                .TurnTimer = TurnTimer.value_or(0),
            };
        }
        else if (Kind == "inventory" && Session.Player)
        {
            FPlayerInventory& Inventory = Session.Player->Inventory;
            const std::optional<float> Energy = Fields.Number<float>();
            const std::optional<float> EnergyMax = Fields.Number<float>();
            const std::optional<float> Shield = Fields.Number<float>();
            const std::optional<float> ShieldMax = Fields.Number<float>();
            const std::optional<float> Money = Fields.Number<float>();
            const std::optional<int> Kills = Fields.Number<int>();
            bValid = Energy && EnergyMax && Shield && ShieldMax && Money && Kills;
            Inventory.Energy = Energy.value_or(0.f);
            Inventory.EnergyMax = EnergyMax.value_or(0.f);
            Inventory.Shield = Shield.value_or(0.f);
            Inventory.ShieldMax = ShieldMax.value_or(0.f);
            Inventory.Money = Money.value_or(0.f);
            Inventory.Kills = Kills.value_or(0);
        }
        else if (Kind == "ammo" && Session.Player)
        {
            const std::optional<std::string> Name = Fields.Text();
            const std::optional<float> Count = Fields.Number<float>();
            const std::optional<float> Max = Fields.Number<float>();
            bValid = Name && Count && Max;
            Session.Player->Inventory.Ammo.push_back({ .Name = Name.value_or(""), .Count = Count.value_or(0.f), .Max = Max.value_or(0.f) });
        }
        else if (Kind == "key" && Session.Player)
        {
            const std::optional<int> Id = Fields.Number<int>();
            const std::optional<bool> bCode = Fields.Flag();
            const std::optional<int> PartId = Fields.Number<int>();
            const std::optional<int> PartCount = Fields.Number<int>();
            const std::optional<size_t> ObjectIndex = Fields.Number<size_t>();
            const std::optional<bool> bHasTimeout = Fields.Flag();
            const std::optional<int32_t> Timeout = Fields.Number<int32_t>();
            const std::optional<int32_t> WarnTimeout = Fields.Number<int32_t>();
            const std::optional<int32_t> WarnX = Fields.Number<int32_t>();
            const std::optional<int32_t> WarnY = Fields.Number<int32_t>();
            const std::optional<int32_t> ReactivateAfter = Fields.Number<int32_t>();
            const std::optional<std::string> WarnFormat = Fields.Text();
            bValid = Id && bCode && PartId && PartCount && ObjectIndex && bHasTimeout && Timeout && WarnTimeout && WarnX && WarnY && ReactivateAfter && WarnFormat;
            if (bValid)
            {
                Session.Player->Inventory.Keys.push_back({
                    .Id = *Id,
                    .bCode = *bCode,
                    .PartId = *PartId,
                    .PartCount = *PartCount,
                    .ObjectIndex = *ObjectIndex,
                    .bHasTimeout = *bHasTimeout,
                    .Timeout = *Timeout,
                    .WarnTimeout = *WarnTimeout,
                    .WarnFormat = *WarnFormat,
                    .WarnX = *WarnX,
                    .WarnY = *WarnY,
                    .ReactivateAfter = *ReactivateAfter,
                });
            }
        }
        else if (Kind == "slot" && Session.Player)
        {
            const std::optional<size_t> Slot = Fields.Number<size_t>();
            bValid = Slot.has_value();
            Session.Player->WeaponSlot = Slot.value_or(1);
        }
        else if ((Kind == "weapon" || Kind == "slot-weapon") && Session.Player)
        {
            const std::optional<FWeaponState> Weapon = ReadWeapon(Fields);
            bValid = Weapon.has_value();
            if (Weapon && Kind == "weapon")
            {
                Session.Player->Weapon = *Weapon;
            }
            else if (Weapon)
            {
                Session.Player->WeaponSlots.push_back(*Weapon);
            }
        }
        else if (Kind == "objects")
        {
            const std::optional<size_t> Count = Fields.Number<size_t>();
            bValid = Count.has_value();
            ExpectedObjects = Count.value_or(0);
            Session.Objects.emplace();
        }
        else if (Kind == "object" && Session.Objects)
        {
            FSessionObject Object;
            const std::optional<std::optional<size_t>> FileIndex = OptionalNumber(Fields, size_t{});
            const std::optional<std::string> ClassName = Fields.Text();
            const std::optional<float> X = Fields.Number<float>();
            const std::optional<float> Y = Fields.Number<float>();
            const std::optional<float> Altitude = Fields.Number<float>();
            const std::optional<int> Direction = Fields.Number<int>();
            const std::optional<int> TopPose = Fields.Number<int>();
            const std::optional<bool> bWrecked = Fields.Flag();
            const std::optional<bool> bRemoved = Fields.Flag();
            const std::optional<std::optional<float>> Energy = OptionalNumber(Fields, float{});
            const std::optional<std::optional<float>> Shield = OptionalNumber(Fields, float{});
            const std::optional<std::optional<uint32_t>> DoorFlags = OptionalNumber(Fields, uint32_t{});
            const std::optional<int> UsageCount = Fields.Number<int>();
            bValid = FileIndex && ClassName && X && Y && Altitude && Direction && TopPose && bWrecked && bRemoved && Energy && Shield && DoorFlags && UsageCount;
            if (bValid)
            {
                Session.Objects->push_back({
                    .FileIndex = FileIndex->value_or(SIZE_MAX),
                    .ClassName = *ClassName,
                    .X = *X,
                    .Y = *Y,
                    .Altitude = *Altitude,
                    .Direction = *Direction,
                    .TopPose = *TopPose,
                    .bWrecked = *bWrecked,
                    .bRemoved = *bRemoved,
                    .Energy = *Energy,
                    .Shield = *Shield,
                    .DoorFlags = *DoorFlags,
                    .UsageCount = *UsageCount,
                });
            }
        }
        else
        {
            bValid = false;
        }

        if (!bValid || !Fields.AtEnd())
        {
            return std::nullopt;
        }
    }

    if (!bSigned || !bLevel || (Session.Objects && Session.Objects->size() != ExpectedObjects))
    {
        return std::nullopt;
    }

    return Session;
}

std::filesystem::path SessionDirectory(const std::string_view PlayerName)
{
    return UserDataDirectory() / "session" / std::filesystem::path(PlayerName);
}

bool WriteSessionFile(const std::filesystem::path& Path, const FSession& Session)
{
    std::error_code Error;
    std::filesystem::create_directories(Path.parent_path(), Error);
    std::ofstream File(Path, std::ios::binary | std::ios::trunc);
    const std::string Text = FormatSession(Session);
    File.write(Text.data(), std::streamsize(Text.size()));
    return bool(File);
}

std::optional<FSession> ReadSessionFile(const std::filesystem::path& Path)
{
    // Larger than any level's snapshot; a bigger file is not a session.
    constexpr std::uintmax_t MaxSessionBytes = 16 * 1024 * 1024;
    std::error_code Error;
    const std::uintmax_t Size = std::filesystem::file_size(Path, Error);
    if (Error || Size > MaxSessionBytes)
    {
        return std::nullopt;
    }

    std::ifstream File(Path, std::ios::binary);
    std::ostringstream Text;
    Text << File.rdbuf();
    return File ? ParseSession(Text.str()) : std::nullopt;
}

std::filesystem::path DefaultSessionPath(const std::filesystem::path& Directory)
{
    return Directory / std::format("default{}", SessionExtension);
}

std::vector<FSession> ReadSavedSessions(const std::filesystem::path& Directory)
{
    std::vector<FSession> Sessions;
    for (const auto& [Number, Path] : NumberedSessions(Directory))
    {
        if (std::optional<FSession> Session = ReadSessionFile(Path))
        {
            Sessions.push_back(std::move(*Session));
        }
    }

    return Sessions;
}

bool WriteSavedSession(const std::filesystem::path& Directory, const FSession& Session)
{
    const std::vector<std::pair<int, std::filesystem::path>> Existing = NumberedSessions(Directory);
    const int Next = Existing.empty() ? 0 : Existing.front().first + 1;
    for (const auto& [Number, Path] : Existing)
    {
        if (Number <= Next - KeptSessions)
        {
            std::error_code Error;
            std::filesystem::remove(Path, Error);
        }
    }

    return WriteSessionFile(Directory / std::format("s{:03}{}", Next, SessionExtension), Session);
}

std::expected<std::string, std::string> VerifySessions(FResourceArchive& Archive)
{
    // Every seventh object is gone, as collected or destroyed, and every eleventh is wrecked.
    constexpr size_t RemovedEvery = 7;
    constexpr size_t WreckedEvery = 11;
    constexpr float CreatedOffset = 10.f;
    const auto Key = [](const FSessionObject& Object)
    {
        return std::tuple(Object.FileIndex, Object.ClassName, Object.X, Object.Y, Object.Direction, Object.TopPose, Object.bWrecked, Object.bRemoved, Object.Energy,
            Object.Shield, Object.DoorFlags, Object.UsageCount);
    };
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
        std::expected<FLevel, std::string> Fresh = LoadLevel(Archive, LevelPath);
        if (!Level || !Fresh || Level->Objects.empty())
        {
            return std::unexpected(std::format("{}: {}", LevelPath, !Level ? Level.error() : !Fresh ? Fresh.error() : "no objects"));
        }

        const std::string CreatedClass = Level->Objects.front().ClassName;
        const std::optional<size_t> Created = CreateLevelObject(*Level, CreatedClass, Level->Objects.front().X + CreatedOffset, Level->Objects.front().Y);
        std::vector<FObjectBody> Bodies = CreateObjectBodies(*Level);
        std::expected<std::vector<FDoor>, std::string> Doors = CreateDoors(*Level, Archive, Bodies);
        if (!Created || !Doors)
        {
            return std::unexpected(std::format("{}: {}", LevelPath, !Doors ? Doors.error() : "no object created"));
        }

        for (size_t Index = 0; Index < Level->Objects.size(); ++Index)
        {
            Level->Objects[Index].bRemoved = Index % RemovedEvery == RemovedEvery - 1;
            Level->Objects[Index].bWrecked = Index % WreckedEvery == WreckedEvery - 1;
        }

        for (FDoor& Door : *Doors)
        {
            Door.Flags ^= DoorOpen;
            ++Door.UsageCount;
        }

        std::vector<FCarriedKey> Keys = { { .Id = 1, .ObjectIndex = RemovedEvery - 1 } };
        FSession Session{ .LevelPath = LevelPath, .LevelName = Level->Name, .Player = FCarriedPlayer{ .Inventory = { .Keys = Keys } } };
        Session.Objects = CaptureObjects(*Level, Bodies, *Doors, Session.Player->Inventory.Keys);
        const std::string Text = FormatSession(Session);
        const std::optional<FSession> Read = ParseSession(Text);
        if (!Read || FormatSession(*Read) != Text)
        {
            return std::unexpected(std::format("{}: the session does not read back", LevelPath));
        }

        const size_t KeyObject = Read->Player->Inventory.Keys.front().ObjectIndex;
        if (KeyObject >= Read->Objects->size() || !(*Read->Objects)[KeyObject].bRemoved)
        {
            return std::unexpected(std::format("{}: the carried key's object is lost", LevelPath));
        }

        RestoreObjects(*Fresh, *Read->Objects);
        std::vector<FObjectBody> FreshBodies = CreateObjectBodies(*Fresh);
        std::expected<std::vector<FDoor>, std::string> FreshDoors = CreateDoors(*Fresh, Archive, FreshBodies);
        std::vector<FCarriedKey> FreshKeys = Read->Player->Inventory.Keys;
        std::vector<FSessionObject> Again = FreshDoors ? CaptureObjects(*Fresh, FreshBodies, *FreshDoors, FreshKeys) : std::vector<FSessionObject>{};
        std::vector<FSessionObject> Expected = *Read->Objects;
        std::ranges::sort(Again, {}, Key);
        std::ranges::sort(Expected, {}, Key);
        if (!std::ranges::equal(Again, Expected, {}, Key, Key))
        {
            return std::unexpected(std::format("{}: the restored level differs from its session", LevelPath));
        }

        ++Levels;
        Objects += Expected.size();
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

    return std::format("{} levels ({} objects) restored from their sessions, saving plot passed", Levels, Objects);
}
