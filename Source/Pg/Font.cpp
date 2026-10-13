#include "Pg/Font.h"

#include "Pg/Binary.h"
#include "Pg/Gaf.h"

#include <algorithm>

namespace
{
// FNT_ slot 0: one record per font. GafParseSlot0 (0047b830) keeps the first word as the glyph count and copies the rest into the runtime
// font from +0x18 on.
struct FFontRecord
{
    uint32_t GlyphCount;
    uint32_t Flags;
    int32_t LineGap;
    int32_t CharSpacing;
    int32_t Reserved0;
    // Lines whose top lies this far past the clip bottom are not drawn.
    int32_t ClipMargin;
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
    if (!Slots || (*Slots)[FontRecordSlot].size() % sizeof(FFontRecord) != 0)
    {
        return std::nullopt;
    }

    std::vector<FFont> Fonts;
    size_t GlyphOffset = 0;
    for (size_t Offset = 0; Offset < (*Slots)[FontRecordSlot].size(); Offset += sizeof(FFontRecord))
    {
        const FFontRecord Record = *ReadStruct<FFontRecord>((*Slots)[FontRecordSlot], Offset);
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
    const std::span<const uint32_t> BitmapWidths)
{
    constexpr char FirstPrintable = 0x20;
    constexpr char Space = ' ';
    // The format strings write a literal percent sign as a tilde.
    constexpr char PercentStandIn = '~';
    constexpr char NewLine = '\n';
    constexpr char CarriageReturn = '\r';
    std::vector<FPlacedGlyph> Placed;
    int32_t Cursor = X;
    int32_t Line = Y;
    for (char Character : Text)
    {
        // The level start turns word wrap off (FUN_0043be00 calls PgFontSetWordWrap), so a new line goes back to the starting column.
        if (Character == NewLine)
        {
            Cursor = X;
            Line += Font.LineHeight + ((Font.Flags & FontProportional) != 0 ? Font.LineGap : 0);
            continue;
        }

        if (Character == CarriageReturn)
        {
            continue;
        }

        if (uint8_t(Character) < uint8_t(FirstPrintable))
        {
            break;
        }

        Character = Character == PercentStandIn ? '%' : Character;
        const size_t Index = size_t(uint8_t(Character)) - 1;
        const FGlyph* const Glyph = Character != Space && Index < Font.Glyphs.size() ? &Font.Glyphs[Index] : nullptr;
        if (!Glyph || Glyph->Base.BitmapIndex == 0)
        {
            Cursor += Font.Advance;
            continue;
        }

        Placed.push_back({ .BitmapIndex = Glyph->Base.BitmapIndex, .X = Cursor + Glyph->Base.OffsetX, .Y = Line + Glyph->Base.OffsetY });
        if (Glyph->Overlay.BitmapIndex != 0)
        {
            Placed.push_back({ .BitmapIndex = Glyph->Overlay.BitmapIndex, .X = Cursor + Glyph->Overlay.OffsetX, .Y = Line + Glyph->Overlay.OffsetY });
        }

        const uint32_t Width = Glyph->Base.BitmapIndex <= BitmapWidths.size() ? BitmapWidths[Glyph->Base.BitmapIndex - 1] : 0;
        Cursor += Width == 0 || (Font.Flags & FontProportional) == 0 ? Font.Advance : Font.CharSpacing + int32_t(Width);
    }

    return Placed;
}
