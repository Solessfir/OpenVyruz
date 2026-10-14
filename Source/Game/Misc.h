#pragma once

#include "Game/Engine.h"
#include "Game/Input.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// misc.cpp's inventory (FUN_00439b00, session +0x20d0 bit 0x200) and help (FUN_0043ab50, bit 0x400) screens: rows 40 pixels apart in a window over the frozen game.
inline constexpr int32_t InfoRowHeight = 0x28;
inline constexpr size_t InfoWeaponSlots = 9;

enum class EInfoIcon : uint8_t
{
    None,
    // A carried key's item, drawn in its own animation.
    Item,
    // A weapon slot's panel icon.
    Weapon,
};

struct FInfoRow
{
    EInfoIcon Icon = EInfoIcon::None;
    // The item's level object, or the 1-based weapon slot.
    size_t IconIndex = 0;
    std::string Text;
    int32_t Y = 0;
    // The rows past the window's last are still laid out, and their icons played, but not drawn.
    bool bDrawn = false;
};

struct FInfoLayout
{
    // The rows below the scroll, whose icons the update pass plays and whose shown ones the draw pass draws.
    std::vector<FInfoRow> Rows;
    // What the lists return, which bounds the scroll; the inventory's counts its codes only below the scroll but every key and weapon slot.
    int32_t Count = 0;
    // The weapon list ran, binding each slot in turn (FUN_004375f0), which clears every weapon's last shot.
    bool bWalkedWeapons = false;
};

// A weapon slot as the inventory binds it: the ADD Weapon's class name and the ammo stock bound to it, if any and not empty.
struct FInventoryWeapon
{
    std::string Name;
    int32_t Count = 0;
    int32_t Max = 0;
    bool bBound = false;
};

// FUN_00439f90's three lists from Top + 40: the codes (FUN_0043a120), the keys (FUN_0043a540) and the weapons (FUN_0043a6e0), each but the last followed by an empty row.
// Keys are in pickup order, as FPlayerInventory keeps them.
FInfoLayout LayoutInventory(std::span<const FCarriedKey> Keys, const std::array<FInventoryWeapon, InfoWeaponSlots>& Weapons, int32_t Scroll, int32_t VisibleRows, int32_t Top);
// FUN_0043afc0's list (FUN_0043b0f0): every bound action of the device's table with its key.
FInfoLayout LayoutHelp(const FBindingTable& Bindings, int32_t Scroll, int32_t VisibleRows, int32_t Top);
