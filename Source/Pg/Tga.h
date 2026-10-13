#pragma once

#include "Pg/Gaf.h"

#include <cstdint>
#include <optional>
#include <span>

// The uncompressed true-colour Targa images of the story and ending screens (FUN_00453590), as one opaque RGB555 frame; nullopt for any other kind of image.
std::optional<Gaf::FFrame> LoadTga(std::span<const uint8_t> File);
