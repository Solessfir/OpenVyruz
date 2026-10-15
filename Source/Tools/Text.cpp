#include "Tools/Text.h"

namespace
{
constexpr uint8_t HighStart = 0x80;

constexpr FCodePage Cp1252High = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
    0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6, 0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF,
    0x00B0, 0x00B1, 0x00B2, 0x00B3, 0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF,
    0x00C0, 0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD, 0x00CE, 0x00CF,
    0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA, 0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF,
    0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7, 0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF,
    0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4, 0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF,
};

FCodePage MakeCp1251()
{
    FCodePage Table = {
        0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A, 0x040C, 0x040B, 0x040F,
        0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122, 0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F,
        0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6, 0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407,
        0x00B0, 0x00B1, 0x0406, 0x0456, 0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457,
    };
    // 0xC0 to 0xFF run straight through the Cyrillic capitals and small letters.
    constexpr size_t CyrillicStart = 0x40;
    constexpr char16_t CyrillicCapitalA = 0x0410;
    for (size_t Index = CyrillicStart; Index < Table.size(); ++Index)
    {
        Table[Index] = char16_t(CyrillicCapitalA + (Index - CyrillicStart));
    }

    return Table;
}

const FCodePage Cp1251High = MakeCp1251();

void AppendUtf8(std::string& Out, const char32_t Code)
{
    constexpr char32_t OneByte = 0x80;
    constexpr char32_t TwoBytes = 0x800;
    if (Code < OneByte)
    {
        Out += char(Code);
    }
    else if (Code < TwoBytes)
    {
        Out += char(0xC0 | Code >> 6);
        Out += char(0x80 | (Code & 0x3F));
    }
    else
    {
        Out += char(0xE0 | Code >> 12);
        Out += char(0x80 | (Code >> 6 & 0x3F));
        Out += char(0x80 | (Code & 0x3F));
    }
}
}

const FCodePage& Cp1252()
{
    return Cp1252High;
}

const FCodePage& Cp1251()
{
    return Cp1251High;
}

const FCodePage& CodePageNamed(const std::string_view Name)
{
    return Name == "windows-1251" ? Cp1251High : Cp1252High;
}

char32_t CodePoint(const uint8_t Byte, const FCodePage& Page)
{
    return Byte < HighStart ? char32_t(Byte) : char32_t(Page[Byte - HighStart]);
}

std::string ToUtf8(const std::span<const uint8_t> Bytes, const FCodePage& Page)
{
    std::string Out;
    Out.reserve(Bytes.size());
    for (const uint8_t Byte : Bytes)
    {
        AppendUtf8(Out, CodePoint(Byte, Page));
    }

    return Out;
}

std::optional<std::vector<uint8_t>> FromUtf8(const std::string_view Text, const FCodePage& Page)
{
    std::vector<uint8_t> Out;
    Out.reserve(Text.size());
    for (size_t Index = 0; Index < Text.size();)
    {
        const uint8_t Lead = uint8_t(Text[Index]);
        char32_t Code = 0;
        size_t Length = 1;
        if (Lead < 0x80)
        {
            Code = Lead;
        }
        else if ((Lead & 0xE0) == 0xC0)
        {
            Code = Lead & 0x1F;
            Length = 2;
        }
        else if ((Lead & 0xF0) == 0xE0)
        {
            Code = Lead & 0x0F;
            Length = 3;
        }
        else
        {
            return std::nullopt;
        }

        if (Index + Length > Text.size())
        {
            return std::nullopt;
        }

        for (size_t Continuation = 1; Continuation < Length; ++Continuation)
        {
            const uint8_t Byte = uint8_t(Text[Index + Continuation]);
            if ((Byte & 0xC0) != 0x80)
            {
                return std::nullopt;
            }

            Code = Code << 6 | (Byte & 0x3F);
        }

        Index += Length;
        if (Code < HighStart)
        {
            Out.push_back(uint8_t(Code));
            continue;
        }

        size_t Found = Page.size();
        for (size_t Slot = 0; Slot < Page.size() && Found == Page.size(); ++Slot)
        {
            Found = char32_t(Page[Slot]) == Code ? Slot : Found;
        }

        if (Found == Page.size())
        {
            return std::nullopt;
        }

        Out.push_back(uint8_t(HighStart + Found));
    }

    return Out;
}

uint64_t HashBytes(const std::span<const uint8_t> Bytes)
{
    constexpr uint64_t Offset = 0xcbf29ce484222325;
    constexpr uint64_t Prime = 0x100000001b3;
    uint64_t Hash = Offset;
    for (const uint8_t Byte : Bytes)
    {
        Hash = (Hash ^ Byte) * Prime;
    }

    return Hash;
}
