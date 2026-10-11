#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace
{
constexpr uint32_t ArchiveMagic = 0x5F584B5F; // "_KX_"
constexpr uint32_t HeaderSize = 32;
constexpr uint32_t EntrySize = 12;
constexpr uint8_t EntryFlagInverted = 1;

template <typename T>
T ReadLE(const uint8_t* const Data)
{
    T Value;
    std::memcpy(&Value, Data, sizeof(T));
    return Value;
}

// RscCryptTable decrypt direction: byte i is rotated right by (i & 7), then inverted.
void DecryptTable(std::vector<uint8_t>& Table)
{
    for (size_t Index = 0; Index < Table.size(); ++Index)
    {
        Table[Index] = static_cast<uint8_t>(~std::rotr(Table[Index], static_cast<int>(Index & 7)));
    }
}

std::string NormalizeName(const std::string_view Name)
{
    std::string Result(Name);
    for (char& Char : Result)
    {
        if (Char == '/')
        {
            Char = '\\';
        }
        else if (Char >= 'A' && Char <= 'Z')
        {
            Char = static_cast<char>(Char + ('a' - 'A'));
        }
    }
    return Result;
}

bool ReadAt(std::ifstream& File, const uint64_t Offset, void* const Out, const size_t Size)
{
    File.clear();
    File.seekg(static_cast<std::streamoff>(Offset));
    File.read(static_cast<char*>(Out), static_cast<std::streamsize>(Size));
    return File.gcount() == static_cast<std::streamsize>(Size);
}
}

bool FResourceArchive::Open(const std::filesystem::path& Path)
{
    Entries.clear();
    File = std::ifstream(Path, std::ios::binary);
    std::error_code Error;
    const uint64_t FileSize = std::filesystem::file_size(Path, Error);
    uint8_t Header[HeaderSize];
    if (!File || Error || !ReadAt(File, 0, Header, HeaderSize))
    {
        return false;
    }

    const uint16_t EntryCount = ReadLE<uint16_t>(Header + 0x00);
    const bool bHasMagic = Header[0x02] != 0;
    // Key 1 means the tables are encrypted and flagged entries are stored bitwise-inverted.
    const uint8_t Key = Header[0x03];
    const uint32_t EntryTableOffset = ReadLE<uint32_t>(Header + 0x04);
    const uint32_t EntryTableSize = ReadLE<uint32_t>(Header + 0x08);
    const uint32_t NameTableOffset = ReadLE<uint32_t>(Header + 0x0C);
    const uint32_t NameTableSize = ReadLE<uint32_t>(Header + 0x10);
    const uint32_t DataOffset = ReadLE<uint32_t>(Header + 0x14);
    const uint32_t Magic = ReadLE<uint32_t>(Header + 0x1C);

    if ((bHasMagic && Magic != ArchiveMagic) || EntryTableSize < uint64_t{ EntryCount } * EntrySize ||
        uint64_t{ EntryTableOffset } + EntryTableSize > FileSize || uint64_t{ NameTableOffset } + NameTableSize > FileSize)
    {
        return false;
    }

    std::vector<uint8_t> EntryTable(EntryTableSize);
    std::vector<uint8_t> NameTable(NameTableSize);
    if (!ReadAt(File, EntryTableOffset, EntryTable.data(), EntryTableSize) || !ReadAt(File, NameTableOffset, NameTable.data(), NameTableSize))
    {
        return false;
    }
    if (Key != 0)
    {
        DecryptTable(EntryTable);
        DecryptTable(NameTable);
    }
    bInvertedData = Key == 1;

    for (uint32_t Index = 0; Index < EntryCount; ++Index)
    {
        const uint8_t* const Record = EntryTable.data() + Index * EntrySize;
        const int16_t NameOffset = ReadLE<int16_t>(Record + 0);
        const FEntry Entry{
            .Offset = ReadLE<uint32_t>(Record + 4),
            .Size = ReadLE<uint32_t>(Record + 8),
            .Flags = Record[3],
        };

        const auto NameEnd = NameOffset >= 0 ? std::find(NameTable.begin() + NameOffset, NameTable.end(), uint8_t{ 0 }) : NameTable.end();
        if (NameOffset < 0 || NameEnd == NameTable.end() || uint64_t{ DataOffset } + Entry.Offset + Entry.Size > FileSize)
        {
            Entries.clear();
            return false;
        }

        const std::string_view Name(reinterpret_cast<const char*>(NameTable.data() + NameOffset), NameEnd - (NameTable.begin() + NameOffset));
        FEntry Stored = Entry;
        Stored.Offset += DataOffset;
        // vz-data.rsc stores v-turret-gun-udg.gaf twice with identical bytes; the first copy wins.
        Entries.emplace(NormalizeName(Name), Stored);
    }
    return true;
}

std::optional<std::vector<uint8_t>> FResourceArchive::Read(const std::string_view Name)
{
    const auto It = Entries.find(NormalizeName(Name));
    if (It == Entries.end())
    {
        return std::nullopt;
    }

    const FEntry& Entry = It->second;
    std::vector<uint8_t> Data(Entry.Size);
    if (!ReadAt(File, Entry.Offset, Data.data(), Data.size()))
    {
        return std::nullopt;
    }
    if (bInvertedData && (Entry.Flags & EntryFlagInverted))
    {
        for (uint8_t& Byte : Data)
        {
            Byte = static_cast<uint8_t>(~Byte);
        }
    }
    return Data;
}
