#include "Pg/Font.h"

#include "Pg/Binary.h"
#include "Pg/Gaf.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace
{
// FNT_ slot 0: one record per font.
// GafParseSlot0 (0047b830) keeps the first word as the glyph count and copies the rest into the runtime font from +0x18 on.
struct FFontRecord
{
    uint32_t GlyphCount;
    uint32_t Flags;
    int32_t LineGap;
    int32_t CharSpacing;
    int32_t Reserved0;
    // The baseline's depth below a line's top (runtime font +0x28), where a text page puts its first line.
    int32_t Baseline;
    int32_t Advance;
    int32_t LineHeight;
    uint32_t DrawMode;
    uint32_t Color[3];
    uint32_t ShadowDrawMode;
    uint32_t ShadowColor[3];
    int32_t ShadowX;
    int32_t ShadowY;
};
static_assert(sizeof(FFontRecord) == 72);

// FNT_ slot 2: each font's glyph table, one record per character.
struct FGlyphRecord
{
    uint32_t BaseBitmap;
    int32_t BaseX;
    int32_t BaseY;
    uint32_t OverlayBitmap;
    int32_t OverlayX;
    int32_t OverlayY;
};
static_assert(sizeof(FGlyphRecord) == 24);

constexpr size_t FontRecordSlot = 0;
constexpr size_t GlyphSlot = 2;
constexpr size_t MaxGlyphs = 256;
}

std::optional<std::vector<Font::FFont>> Font::LoadFonts(const std::span<const uint8_t> File)
{
    const std::optional<Gaf::FSlots> Slots = Gaf::ReadEntrySlots(File, Gaf::Tag("FNT_"));
    // v-inventory.gaf's single font has a 64-byte record, without the shadow fields; the exe leaves what a short record lacks undefined, so it reads as zero here.
    const uint32_t RecordSize = Gaf::ReadEntryRecordSize(File, Gaf::Tag("FNT_")).value_or(sizeof(FFontRecord));
    constexpr uint32_t ShortestRecord = offsetof(FFontRecord, DrawMode);
    if (!Slots || RecordSize < ShortestRecord || (*Slots)[FontRecordSlot].size() % RecordSize != 0)
    {
        return std::nullopt;
    }

    std::vector<FFont> Fonts;
    size_t GlyphOffset = 0;
    for (size_t Offset = 0; Offset < (*Slots)[FontRecordSlot].size(); Offset += RecordSize)
    {
        FFontRecord Record{};
        std::memcpy(&Record, (*Slots)[FontRecordSlot].data() + Offset, std::min<size_t>(RecordSize, sizeof(FFontRecord)));
        if (Record.GlyphCount > MaxGlyphs)
        {
            return std::nullopt;
        }

        FFont Font{
            .Flags = Record.Flags,
            .LineGap = Record.LineGap,
            .CharSpacing = Record.CharSpacing,
            .Advance = Record.Advance,
            .LineHeight = Record.LineHeight,
            .Baseline = Record.Baseline,
        };
        if ((Record.Flags & FontDrawSettings) != 0)
        {
            Font.DrawMode = Record.DrawMode;
            Font.Color = { .Alpha = Record.Color[0], .Diffuse = Record.Color[1], .Specular = Record.Color[2] };
        }

        for (uint32_t Index = 0; Index < Record.GlyphCount; ++Index, GlyphOffset += sizeof(FGlyphRecord))
        {
            const std::optional<FGlyphRecord> Glyph = ReadStruct<FGlyphRecord>((*Slots)[GlyphSlot], GlyphOffset);
            if (!Glyph)
            {
                return std::nullopt;
            }

            Font.Glyphs.push_back({
                .Base = { .BitmapIndex = Glyph->BaseBitmap, .OffsetX = Glyph->BaseX, .OffsetY = Glyph->BaseY },
                .Overlay = { .BitmapIndex = Glyph->OverlayBitmap, .OffsetX = Glyph->OverlayX, .OffsetY = Glyph->OverlayY },
            });
        }

        Fonts.push_back(std::move(Font));
    }

    return Fonts;
}

bool Font::WriteFonts(std::vector<Gaf::FContainerEntry>& Entries, const std::span<const FFont> Fonts)
{
    const auto Entry = std::ranges::find(Entries, Gaf::Tag("FNT_"), &Gaf::FContainerEntry::Tag);
    if (Entry == Entries.end() || Entry->Head.RecordSize == 0 || Entry->Slots[FontRecordSlot].size() != Fonts.size() * Entry->Head.RecordSize)
    {
        return false;
    }

    // Each record is patched in place, so a short record keeps its length and what it never had stays out.
    const size_t RecordSize = std::min<size_t>(Entry->Head.RecordSize, sizeof(FFontRecord));
    size_t GlyphOffset = 0;
    for (size_t Index = 0; Index < Fonts.size(); ++Index)
    {
        const FFont& Font = Fonts[Index];
        uint8_t* const Bytes = Entry->Slots[FontRecordSlot].data() + Index * Entry->Head.RecordSize;
        FFontRecord Record{};
        std::memcpy(&Record, Bytes, RecordSize);
        if (Record.GlyphCount != Font.Glyphs.size() || Entry->Slots[GlyphSlot].size() < GlyphOffset + Font.Glyphs.size() * sizeof(FGlyphRecord))
        {
            return false;
        }

        Record.Flags = Font.Flags;
        Record.LineGap = Font.LineGap;
        Record.CharSpacing = Font.CharSpacing;
        Record.Advance = Font.Advance;
        Record.LineHeight = Font.LineHeight;
        Record.Baseline = Font.Baseline;
        if ((Font.Flags & FontDrawSettings) != 0)
        {
            Record.DrawMode = Font.DrawMode;
            Record.Color[0] = Font.Color.Alpha;
            Record.Color[1] = Font.Color.Diffuse;
            Record.Color[2] = Font.Color.Specular;
        }

        std::memcpy(Bytes, &Record, RecordSize);
        for (const FGlyph& Glyph : Font.Glyphs)
        {
            const FGlyphRecord Out{
                .BaseBitmap = Glyph.Base.BitmapIndex,
                .BaseX = Glyph.Base.OffsetX,
                .BaseY = Glyph.Base.OffsetY,
                .OverlayBitmap = Glyph.Overlay.BitmapIndex,
                .OverlayX = Glyph.Overlay.OffsetX,
                .OverlayY = Glyph.Overlay.OffsetY,
            };
            std::memcpy(Entry->Slots[GlyphSlot].data() + GlyphOffset, &Out, sizeof(Out));
            GlyphOffset += sizeof(Out);
        }
    }

    return true;
}

Font::FColor Font::FColor::Combine(const FColor& Other) const
{
    constexpr uint32_t ChannelMax = 0xff;
    constexpr int ChannelBits = 8;
    constexpr int ChannelCount = 3;
    FColor Result;
    Result.Diffuse = 0;
    Result.Specular = 0;
    for (int Channel = 0; Channel < ChannelCount; ++Channel)
    {
        const int Shift = Channel * ChannelBits;
        const uint32_t Base = (Diffuse >> Shift) & ChannelMax;
        // ColorModulate (0044e040) darkens by the complement of the other colour: c - (255 - m + 1) * c / 256.
        const uint32_t Inverse = ChannelMax - ((Other.Diffuse >> Shift) & ChannelMax);
        Result.Diffuse |= (Base - (((Inverse + 1) * Base) >> ChannelBits)) << Shift;
        // ColorAddSaturate (0044dfe0) adds with saturation.
        Result.Specular |= std::min(ChannelMax, ((Specular >> Shift) & ChannelMax) + ((Other.Specular >> Shift) & ChannelMax)) << Shift;
    }

    Result.Alpha = std::min(ChannelMax, Alpha * Other.Alpha / ChannelMax);
    return Result;
}

std::vector<Font::FPlacedGlyph> Font::LayoutText(const FFont& Font, const std::string_view Text, const int32_t X, const int32_t Y,
    const std::span<const uint32_t> BitmapWidths, const std::optional<int32_t> WrapRight, int32_t* const EndCursor)
{
    constexpr char FirstPrintable = 0x20;
    constexpr char Space = ' ';
    constexpr char Tab = '\t';
    constexpr int32_t TabColumns = 8;
    // The format strings write a literal percent sign as a tilde.
    constexpr char PercentStandIn = '~';
    constexpr char NewLine = '\n';
    constexpr char CarriageReturn = '\r';
    const bool bProportional = (Font.Flags & FontProportional) != 0;
    const auto GlyphOf = [&](char Character) -> const FGlyph*
    {
        Character = Character == PercentStandIn ? '%' : Character;
        const size_t Index = size_t(uint8_t(Character)) - 1;
        const FGlyph* const Glyph = Character != Space && Index < Font.Glyphs.size() ? &Font.Glyphs[Index] : nullptr;
        return Glyph && Glyph->Base.BitmapIndex != 0 ? Glyph : nullptr;
    };
    const auto WidthOf = [&](const FGlyph& Glyph) { return Glyph.Base.BitmapIndex <= BitmapWidths.size() ? int32_t(BitmapWidths[Glyph.Base.BitmapIndex - 1]) : 0; };
    const auto AdvanceOf = [&](const FGlyph* const Glyph)
    {
        const int32_t Width = Glyph ? WidthOf(*Glyph) : 0;
        return Width == 0 || !bProportional ? Font.Advance : Font.CharSpacing + Width;
    };
    const auto IsWordEnd = [](const char Character) { return Character == Space || Character == Tab || uint8_t(Character) < uint8_t(FirstPrintable); };

    std::vector<FPlacedGlyph> Placed;
    int32_t Cursor = X;
    int32_t Line = Y;
    // In game word wrap is off (FUN_0043be00 calls PgFontSetWordWrap), so a new line goes back to the starting column; with it on, a new line starts at the clip's left edge, which callers pass as X.
    const auto BreakLine = [&]
    {
        Cursor = X;
        Line += Font.LineHeight + (bProportional ? Font.LineGap : 0);
    };
    for (size_t Index = 0; Index < Text.size(); ++Index)
    {
        const char Character = Text[Index];
        if (Character == NewLine)
        {
            BreakLine();
            continue;
        }

        if (Character == CarriageReturn)
        {
            continue;
        }

        // A tab moves to the next multiple of eight advances.
        if (Character == Tab)
        {
            const int32_t Stop = TabColumns * Font.Advance;
            Cursor += Stop == 0 ? 0 : Stop - Cursor % Stop;
            continue;
        }

        if (uint8_t(Character) < uint8_t(FirstPrintable))
        {
            break;
        }

        // PgFontPrint measures each word before printing it and moves it to a new line when one of its bitmaps would cross the clip's right edge.
        if (WrapRight && Character != Space && (Index == 0 || IsWordEnd(Text[Index - 1])) && Cursor != X)
        {
            int32_t Measure = Cursor - 1;
            for (size_t Next = Index; Next < Text.size() && !IsWordEnd(Text[Next]); ++Next)
            {
                if (const FGlyph* const Glyph = GlyphOf(Text[Next]))
                {
                    if (*WrapRight < Glyph->Base.OffsetX + WidthOf(*Glyph) + Measure)
                    {
                        BreakLine();
                        break;
                    }

                    Measure += AdvanceOf(Glyph);
                }
            }
        }

        const FGlyph* const Glyph = GlyphOf(Character);
        if (!Glyph)
        {
            Cursor += Font.Advance;
            continue;
        }

        Placed.push_back({ .BitmapIndex = Glyph->Base.BitmapIndex, .X = Cursor + Glyph->Base.OffsetX, .Y = Line + Glyph->Base.OffsetY });
        if (Glyph->Overlay.BitmapIndex != 0)
        {
            Placed.push_back({ .BitmapIndex = Glyph->Overlay.BitmapIndex, .X = Cursor + Glyph->Overlay.OffsetX, .Y = Line + Glyph->Overlay.OffsetY });
        }

        Cursor += AdvanceOf(Glyph);
    }

    if (EndCursor)
    {
        *EndCursor = Cursor;
    }

    return Placed;
}
