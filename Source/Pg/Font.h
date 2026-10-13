#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// pgfont (pg\lib\pg\pgfont.cpp): the bitmap fonts stored as FNT_ entries of a GAF, whose glyphs are the GAF's GOB bitmaps.
namespace Font
{
// Flags at font +0x18.
inline constexpr uint32_t FontProportional = 0x1;
inline constexpr uint32_t FontDrawSettings = 0x2;
inline constexpr uint32_t FontShadow = 0x4;

struct FGlyphLayer
{
    // 1-based GOB bitmap; zero draws nothing.
    uint32_t BitmapIndex = 0;
    int32_t OffsetX = 0;
    int32_t OffsetY = 0;
};

// A character's bitmap and the second bitmap drawn over it.
struct FGlyph
{
    FGlyphLayer Base;
    FGlyphLayer Overlay;
};

// A pgbmp colour: alpha, a diffuse RGB that modulates the bitmap, and a specular RGB added to it.
struct FColor
{
    uint32_t Alpha = 0xff;
    uint32_t Diffuse = 0xffffff;
    uint32_t Specular = 0;

    // ColorCombine at 0044f310: the diffuse multiplied, the specular added with saturation, the alpha multiplied.
    FColor Combine(const FColor& Other) const;
};

struct FFont
{
    uint32_t Flags = 0;
    int32_t LineGap = 0;
    // Added to a proportional glyph's width.
    int32_t CharSpacing = 0;
    // A fixed-width glyph's advance, and a space's.
    int32_t Advance = 0;
    int32_t LineHeight = 0;
    // A font without draw settings gets mode 1 and white from PgFontSelect (0044c3c0).
    uint32_t DrawMode = 1;
    FColor Color;
    // Indexed by character code minus one.
    std::vector<FGlyph> Glyphs;
};

// Every FNT_ font of a GAF file, in their 1-based order; nullopt when the entry is malformed.
std::optional<std::vector<FFont>> LoadFonts(std::span<const uint8_t> File);

struct FPlacedGlyph
{
    uint32_t BitmapIndex = 0;
    // Top-left of the trimmed bitmap.
    int32_t X = 0;
    int32_t Y = 0;
};

// The glyphs of a text from the cursor at X, Y (PgFontPrint at 0044bd70), given the GAF's bitmap widths.
// The text stops at the first control character other than a line break or tab.
// With WrapRight, word wrap is on and X is the clip's left edge.
std::vector<FPlacedGlyph> LayoutText(const FFont& Font, std::string_view Text, int32_t X, int32_t Y, std::span<const uint32_t> BitmapWidths,
    std::optional<int32_t> WrapRight = std::nullopt);
}
