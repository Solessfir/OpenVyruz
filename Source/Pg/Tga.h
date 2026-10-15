#pragma once

#include "Pg/Gaf.h"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A true-colour Targa image at full precision: 8-bit RGBA, rows top to bottom.
struct FTgaImage
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    std::vector<uint8_t> Pixels;
};

// Uncompressed or run-length true-colour Targa (16, 24 or 32 bits); nullopt for any other kind of image.
std::optional<FTgaImage> DecodeTga(std::span<const uint8_t> File);
// The image as a Targa in the original file's form: its ID field, row order, run-length coding and depth (16-bit stays A1R5G5B5, anything else becomes 24-bit).
std::optional<std::vector<uint8_t>> EncodeTga(const FTgaImage& Image, std::span<const uint8_t> Original);

// The uncompressed true-colour Targa images of the story and ending screens (FUN_00453590), as one opaque RGB555 frame; nullopt for any other kind of image.
std::optional<Gaf::FFrame> LoadTga(std::span<const uint8_t> File);
