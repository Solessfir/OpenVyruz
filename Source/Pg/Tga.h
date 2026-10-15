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

// The uncompressed true-colour Targa images of the story and ending screens (FUN_00453590), as one opaque RGB555 frame; nullopt for any other kind of image.
std::optional<Gaf::FFrame> LoadTga(std::span<const uint8_t> File);
