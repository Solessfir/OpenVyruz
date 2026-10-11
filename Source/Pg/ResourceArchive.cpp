#include "Pg/ResourceArchive.h"

#include "Pg/Binary.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace
{
constexpr uint32_t ArchiveMagic = 0x5F584B5F; // "_KX_"
constexpr uint8_t EntryFlagInverted = 1;
// Any non-zero key encrypts the tables; key 1 also stores flagged entries bitwise-inverted.
constexpr uint8_t KeyInvertsData = 1;
constexpr size_t RotationMask = 7;

struct FArchiveHeader
{
    uint16_t EntryCount;
    uint8_t bHasMagic;
    uint8_t Key;
    uint32_t EntryTableOffset;
    uint32_t EntryTableSize;
    uint32_t NameTableOffset;
    uint32_t NameTableSize;
    uint32_t DataOffset;
    uint32_t DataSize;
    uint32_t Magic;
};
static_assert(sizeof(FArchiveHeader) == 32);

struct FEntryRecord
{
    int16_t NameOffset;
    uint8_t Unused;
    uint8_t Flags;
    uint32_t Offset;
    uint32_t Size;
};
static_assert(sizeof(FEntryRecord) == 12);

// RscCryptTable decrypt direction: byte i is rotated right by (i & 7), then inverted.
void DecryptTable(std::vector<uint8_t>& Table)
{
    for (size_t Index = 0; Index < Table.size(); ++Index)
    {
        Table[Index] = static_cast<uint8_t>(~std::rotr(Table[Index], static_cast<int>(Index & RotationMask)));
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
    FArchiveHeader Header;
    if (!File || Error || !ReadAt(File, 0, &Header, sizeof(Header)))
    {
        return false;
    }

    if ((Header.bHasMagic && Header.Magic != ArchiveMagic) || Header.EntryTableSize < uint64_t{ Header.EntryCount } * sizeof(FEntryRecord) ||
        uint64_t{ Header.EntryTableOffset } + Header.EntryTableSize > FileSize || uint64_t{ Header.NameTableOffset } + Header.NameTableSize > FileSize)
    {
        return false;
    }

    std::vector<uint8_t> EntryTable(Header.EntryTableSize);
    std::vector<uint8_t> NameTable(Header.NameTableSize);
    if (!ReadAt(File, Header.EntryTableOffset, EntryTable.data(), EntryTable.size()) || !ReadAt(File, Header.NameTableOffset, NameTable.data(), NameTable.size()))
    {
        return false;
    }
    if (Header.Key != 0)
    {
        DecryptTable(EntryTable);
        DecryptTable(NameTable);
    }
    bInvertedData = Header.Key == KeyInvertsData;

    for (uint32_t Index = 0; Index < Header.EntryCount; ++Index)
    {
        // The size check above guarantees every record fits.
        const FEntryRecord Record = *ReadStruct<FEntryRecord>(EntryTable, Index * sizeof(FEntryRecord));
        if (Record.NameOffset < 0 || size_t(Record.NameOffset) >= NameTable.size() || uint64_t{ Header.DataOffset } + Record.Offset + Record.Size > FileSize)
        {
            Entries.clear();
            return false;
        }
        const auto NameStart = NameTable.begin() + Record.NameOffset;
        const auto NameEnd = std::find(NameStart, NameTable.end(), uint8_t{ 0 });
        if (NameEnd == NameTable.end())
        {
            Entries.clear();
            return false;
        }

        const std::string_view Name(reinterpret_cast<const char*>(&*NameStart), NameEnd - NameStart);
        // vz-data.rsc stores v-turret-gun-udg.gaf twice with identical bytes; the first copy wins.
        Entries.emplace(NormalizeName(Name), FEntry{
            .Offset = Header.DataOffset + Record.Offset,
            .Size = Record.Size,
            .Flags = Record.Flags,
        });
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
