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

struct FFont
{
    uint32_t Flags = 0;
    int32_t LineGap = 0;
    // Added to a proportional glyph's width.
    int32_t CharSpacing = 0;
    // A fixed-width glyph's advance, and a space's.
    int32_t Advance = 0;
    int32_t LineHeight = 0;
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

// The glyphs of one line of text from the cursor at X, Y (FUN_0044bd70), given the GAF's bitmap widths. The text stops at the first control
// character; the original's word wrap at the clip edge is not needed by single-line texts.
std::vector<FPlacedGlyph> LayoutText(const FFont& Font, std::string_view Text, int32_t X, int32_t Y, std::span<const uint32_t> BitmapWidths);
}
