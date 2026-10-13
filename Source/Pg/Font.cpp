#include "Pg/Font.h"

#include "Pg/Binary.h"
#include "Pg/Gaf.h"

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

std::vector<Font::FPlacedGlyph> Font::LayoutText(const FFont& Font, const std::string_view Text, const int32_t X, const int32_t Y,
    const std::span<const uint32_t> BitmapWidths)
{
    constexpr char FirstPrintable = 0x20;
    constexpr char Space = ' ';
    // The format strings write a literal percent sign as a tilde.
    constexpr char PercentStandIn = '~';
    std::vector<FPlacedGlyph> Placed;
    int32_t Cursor = X;
    for (char Character : Text)
    {
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

        Placed.push_back({ .BitmapIndex = Glyph->Base.BitmapIndex, .X = Cursor + Glyph->Base.OffsetX, .Y = Y + Glyph->Base.OffsetY });
        if (Glyph->Overlay.BitmapIndex != 0)
        {
            Placed.push_back({ .BitmapIndex = Glyph->Overlay.BitmapIndex, .X = Cursor + Glyph->Overlay.OffsetX, .Y = Y + Glyph->Overlay.OffsetY });
        }

        const uint32_t Width = Glyph->Base.BitmapIndex <= BitmapWidths.size() ? BitmapWidths[Glyph->Base.BitmapIndex - 1] : 0;
        Cursor += Width == 0 || (Font.Flags & FontProportional) == 0 ? Font.Advance : Font.CharSpacing + int32_t(Width);
    }

    return Placed;
}
