#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Read-only access to the original .RSC archives (vz-data.rsc, Vz-slide.rsc).
// Mirrors RscOpen/RscFileOpen/RscFileRead at 004551a0..004559f0 in Vyruz.exe.
class FResourceArchive
{
public:
    bool Open(const std::filesystem::path& Path);

    // Name lookup is case-insensitive and uses backslash separators, as stored in the archive.
    std::optional<std::vector<uint8_t>> Read(std::string_view Name);

    size_t GetEntryCount() const { return Entries.size(); }

    // Every entry's name as stored, in the archive's own order, repeats included.
    const std::vector<std::string>& GetStoredNames() const { return StoredNames; }

    // Normalized (lowercase, backslash) names of every entry.
    std::vector<std::string> GetNames() const
    {
        std::vector<std::string> Names;
        Names.reserve(Entries.size());
        for (const auto& [Name, Entry] : Entries)
        {
            Names.push_back(Name);
        }
        return Names;
    }

private:
    struct FEntry
    {
        uint32_t Offset = 0;
        uint32_t Size = 0;
        uint8_t Flags = 0;
    };

    std::ifstream File;
    std::unordered_map<std::string, FEntry> Entries;
    std::vector<std::string> StoredNames;
    bool bInvertedData = false;
};

// Writes an archive RscOpen reads: the tables unencrypted (key 0) behind the "_KX_" magic, then each entry's data in order. Names keep their case.
// Returns false when the file cannot be written or the names overflow the 16-bit name offsets.
bool WriteResourceArchive(const std::filesystem::path& Path, const std::vector<std::pair<std::string, std::vector<uint8_t>>>& Files);
