#include "Game/Misc.h"

#include "Game/Language.h"

#include <algorithm>
#include <ranges>

namespace
{
// The lists share the scroll still to skip, the rows still to show and the row's place, as their pointer arguments do.
struct FInfoCursor
{
    int32_t Skip = 0;
    int32_t Rows = 0;
    int32_t Y = 0;
};

// A row below the scroll: laid out, drawn while the list's draw flag holds, which runs out with the window's rows.
void AddRow(FInfoLayout& Layout, FInfoCursor& Cursor, bool& bDraw, const EInfoIcon Icon, const size_t IconIndex, std::string Text)
{
    Layout.Rows.push_back({ .Icon = Icon, .IconIndex = IconIndex, .Text = std::move(Text), .Y = Cursor.Y, .bDrawn = bDraw });
    Cursor.Y += InfoRowHeight;
    if (Cursor.Rows > 0 && --Cursor.Rows == 0)
    {
        bDraw = false;
    }
}

// The empty row after a list with any rows; it takes a row whatever the scroll.
void AddGap(int32_t& Count, FInfoCursor& Cursor)
{
    if (Count == 0)
    {
        return;
    }

    ++Count;
    Cursor.Y += InfoRowHeight;
    if (Cursor.Rows > 0)
    {
        --Cursor.Rows;
    }
}

// FUN_0043a120: one row per code, newest first, with its parts' Code-Strings in part order, "???" for a missing one. Each pass takes the first code not listed yet and gathers its parts.
int32_t LayoutCodes(FInfoLayout& Layout, FInfoCursor& Cursor, const std::span<const FCarriedKey> Keys)
{
    int32_t Count = 0;
    bool bDraw = true;
    std::vector<int> Listed;
    bool bMore = true;
    while (bMore)
    {
        bMore = false;
        const FCarriedKey* First = nullptr;
        std::vector<const FCarriedKey*> Parts;
        for (const FCarriedKey& Key : Keys | std::views::reverse)
        {
            if (!Key.bCode)
            {
                continue;
            }

            const bool bListed = std::ranges::find(Listed, Key.Id) != Listed.end();
            if (!First && !bListed)
            {
                Listed.push_back(Key.Id);
                First = &Key;
                Parts.assign(size_t(std::max(Key.PartCount, 0)), nullptr);
            }
            else if (!First || Key.Id != First->Id)
            {
                bMore = bMore || (First && !bListed);
                continue;
            }

            // ponytail: the original drops the whole list on a part outside its code ("Invalid code description."); no level has one.
            if (Key.PartId >= 1 && size_t(Key.PartId) <= Parts.size())
            {
                Parts[size_t(Key.PartId - 1)] = &Key;
            }
        }

        if (!First)
        {
            break;
        }

        if (Cursor.Skip >= 1)
        {
            --Cursor.Skip;
            continue;
        }

        std::string Text = Format("Code #%d : ", First->Id);
        for (size_t Part = 0; Part < Parts.size(); ++Part)
        {
            Text += Parts[Part] ? Parts[Part]->CodeString : "???";
            if (Part + 1 < Parts.size())
            {
                Text += '-';
            }
        }

        ++Count;
        AddRow(Layout, Cursor, bDraw, EInfoIcon::Item, First->ObjectIndex, std::move(Text));
    }

    AddGap(Count, Cursor);
    return Count;
}

// FUN_0043a540: one row per key that is not a code part, newest first; every key counts, shown or scrolled away.
int32_t LayoutKeys(FInfoLayout& Layout, FInfoCursor& Cursor, const std::span<const FCarriedKey> Keys)
{
    int32_t Count = 0;
    bool bDraw = true;
    for (const FCarriedKey& Key : Keys | std::views::reverse)
    {
        if (Key.bCode)
        {
            continue;
        }

        ++Count;
        if (Cursor.Skip >= 1)
        {
            --Cursor.Skip;
            continue;
        }

        AddRow(Layout, Cursor, bDraw, EInfoIcon::Item, Key.ObjectIndex, Format("Key #%d", Key.Id));
    }

    AddGap(Count, Cursor);
    return Count;
}

// FUN_004376a0 from a slot: the next slot round from it whose bound stock holds a round, or the slot itself.
size_t NextLoadedSlot(const std::array<FInventoryWeapon, InfoWeaponSlots>& Weapons, const size_t Slot)
{
    for (size_t Next = Slot % InfoWeaponSlots + 1; Next != Slot; Next = Next % InfoWeaponSlots + 1)
    {
        if (Weapons[Next - 1].bBound && Weapons[Next - 1].Count >= 1)
        {
            return Next;
        }
    }

    return Slot;
}

// FUN_0043a6e0: the slots from the first loaded one round to it again, each counted; a slot with a bound stock shows "name (count/max)".
int32_t LayoutWeapons(FInfoLayout& Layout, FInfoCursor& Cursor, const std::array<FInventoryWeapon, InfoWeaponSlots>& Weapons)
{
    Layout.bWalkedWeapons = true;
    int32_t Count = 0;
    bool bDraw = true;
    const size_t First = NextLoadedSlot(Weapons, InfoWeaponSlots);
    size_t Slot = First;
    do
    {
        ++Count;
        if (Cursor.Skip >= 1)
        {
            --Cursor.Skip;
        }
        else if (const FInventoryWeapon& Weapon = Weapons[Slot - 1]; Weapon.bBound)
        {
            AddRow(Layout, Cursor, bDraw, EInfoIcon::Weapon, Slot, Format("%s (%d/%d)", Weapon.Name, Weapon.Count, Weapon.Max));
        }

        Slot = NextLoadedSlot(Weapons, Slot);
    } while (Slot != First);

    return Count;
}
}

FInfoLayout LayoutInventory(const std::span<const FCarriedKey> Keys, const std::array<FInventoryWeapon, InfoWeaponSlots>& Weapons, const int32_t Scroll,
    const int32_t VisibleRows, const int32_t Top)
{
    FInfoLayout Layout;
    FInfoCursor Cursor{ .Skip = Scroll, .Rows = VisibleRows, .Y = Top + InfoRowHeight };
    // Each list runs only while the window has rows left.
    if (Cursor.Rows > 0)
    {
        Layout.Count += LayoutCodes(Layout, Cursor, Keys);
    }

    if (Cursor.Rows > 0)
    {
        Layout.Count += LayoutKeys(Layout, Cursor, Keys);
    }

    if (Cursor.Rows > 0)
    {
        Layout.Count += LayoutWeapons(Layout, Cursor, Weapons);
    }

    return Layout;
}

FInfoLayout LayoutHelp(const FBindingTable& Bindings, const int32_t Scroll, const int32_t VisibleRows, const int32_t Top)
{
    FInfoLayout Layout;
    FInfoCursor Cursor{ .Skip = Scroll, .Rows = VisibleRows, .Y = Top + InfoRowHeight };
    bool bDraw = true;
    for (const FActionSetting& Setting : HelpActions())
    {
        const FBinding& Binding = Bindings[size_t(Setting.Action)];
        const uint32_t Code = Binding.Code & BindingCodeMask;
        const uint32_t Modifier = Binding.Modifier & BindingCodeMask;
        if (Code == 0)
        {
            continue;
        }

        ++Layout.Count;
        if (Cursor.Skip >= 1)
        {
            --Cursor.Skip;
            continue;
        }

        // "%ss" makes "keys" in English; the Russian word takes the same s.
        std::string Text = Modifier == 0 ? Format("%s : %s %s", Localize(Setting.Name), Localize("key"), CodeName(Code))
                                         : Format("%s : %ss %s %s %s", Localize(Setting.Name), Localize("key"), CodeName(Modifier), Localize("and"), CodeName(Code));
        const bool bWeapon = Setting.Action >= EAction::Weapon1 && Setting.Action <= EAction::Weapon9;
        AddRow(Layout, Cursor, bDraw, bWeapon ? EInfoIcon::Weapon : EInfoIcon::None, bWeapon ? size_t(Setting.Action) - size_t(EAction::Weapon1) + 1 : 0, std::move(Text));
    }

    return Layout;
}
