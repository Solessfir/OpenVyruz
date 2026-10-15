#pragma once

#include "Pg/Gaf.h"

#include <cstdint>
#include <optional>
#include <vector>

// Straight 8-bit RGBA, rows top to bottom.
struct FImage
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    std::vector<uint8_t> Pixels;
};

// An RGB555 frame as RGBA: each channel widened to 8 bits as (c << 3) | (c >> 2), so narrowing back by >> 3 gives the original; the colour key turns transparent.
FImage ImageFromFrame(const Gaf::FFrame& Frame);
// Copies Source into Target with its top-left at X, Y, flipped as asked.
void Blit(FImage& Target, const FImage& Source, uint32_t X, uint32_t Y, bool bMirrorX = false, bool bMirrorY = false);

// Where an animation frame's bitmap lands relative to the sprite's anchor, as the renderer draws it: the offset plus the trim, or mirrored about the anchor.
struct FPlace
{
    int32_t X = 0;
    int32_t Y = 0;
};
FPlace FramePlace(const Gaf::FAnimationFrame& Frame, const Gaf::FFrame& Bitmap);
// The image as a PNG file's bytes; nullopt for an empty image.
std::optional<std::vector<uint8_t>> EncodePng(const FImage& Image);
