#pragma once

#include "Game/Animation.h"
#include "Game/Panel.h"
#include "Pg/Font.h"
#include "Pg/Gaf.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

class FRandom;
class FResourceArchive;

// The menus of vzed\menu.cpp: a list of items over the menu background, chosen with the mouse or the keyboard.

// An item and its help text (MenuAddItem at 00420c30, MenuAddItemHelp at 00420c90); an item without text leaves a gap.
struct FMenuItem
{
    std::string Text;
    std::string Help;
};

// MenuCreate at 004209b0 creates a menu over a rectangle, its items stepping from its top left; the title prints at 10, 120.
struct FMenu
{
    int32_t Left = 0;
    int32_t Top = 0;
    int32_t Right = 0;
    int32_t Bottom = 0;
    int32_t StepX = 0;
    int32_t StepY = 0;
    std::string Title;
    // Menu +0x74, the title's v-menu.gaf font.
    size_t TitleFont = 1;
    // Menu +0x80 and +0x84: the fonts of the item under the cursor and of the others.
    size_t HighlightFont = 2;
    size_t ItemFont = 1;
    std::vector<FMenuItem> Items;
    // Menu +0x5c, what Escape and Backspace choose.
    std::optional<size_t> DefaultItem;
    // Menu +0x4c, the item the cursor starts on.
    size_t StartItem = 0;
    // Menu +0x54 and +0x58: after this long without a key, a button or a mouse move the menu returns IdleResult; zero waits for ever.
    uint32_t IdleMilliseconds = 0;
    size_t IdleResult = 0;
};

// The state of a running menu (RunMenu at 00421050).
struct FMenuState
{
    int32_t CursorX = 0;
    int32_t CursorY = 0;
    // State +0x0: the last item the cursor was over, which keeps its help text up and which Space or Return chooses.
    std::optional<size_t> Selected;
    // State +0x18: the item position under the cursor, drawn in the highlighted font.
    std::optional<size_t> UnderCursor;
    // State +0x14: the item the arrow keys last moved to.
    std::optional<size_t> KeyItem;
    // DAT_005237a0: the cursor shows once the mouse moves and hides again on an arrow key.
    bool bCursorShown = false;
    // State +0x20 bit 0: the VZ fire flash, drawn for one frame.
    bool bFlash = false;
    // Each step that draws the flash counts here, and MenuStep plays electric.wav with it.
    uint32_t Flashes = 0;
    FAnimationPlayback CursorPlayback;
    std::optional<size_t> Result;
};

// One frame's menu input, as PgPoll reports it.
struct FMenuInput
{
    int32_t MouseX = 0;
    int32_t MouseY = 0;
    bool bMouseMoved = false;
    bool bClick = false;
    bool bAccept = false;
    bool bCancel = false;
    bool bUp = false;
    bool bDown = false;
};

class FMenuArt
{
public:
    // MenuLoadArt at 00420560 loads the menu GAFs and starts the background, the VZ fire flash and the cursor.
    static std::expected<FMenuArt, std::string> Load(FResourceArchive& Archive);

    // RunMenu at 00421050 places the cursor on the start item.
    FMenuState Start(const FMenu& Menu) const;
    // The update pass of MenuStep at 00421780: one random draw for the flash, the cursor animation, and the input. Returns a point for the cursor
    // to jump to after an arrow key.
    std::optional<std::pair<int32_t, int32_t>> Step(const FMenu& Menu, FMenuState& State, const FMenuInput& Input, FRandom& Random, int32_t StepMilliseconds) const;
    // The draw pass of MenuStep at 00421780.
    std::vector<FHudSprite> Draw(const FMenu& Menu, FMenuState& State) const;
    // A text in a v-menu.gaf font with word wrap from Left to Right, its cursor at Left, Top, as the credits print it.
    std::vector<FHudSprite> DrawPage(size_t Font, std::string_view Text, int32_t Left, int32_t Top, int32_t Right) const;
    // One line in a v-menu.gaf font from its cursor at X, Y, the font's colour combined with Color.
    std::vector<FHudSprite> DrawLine(size_t Font, std::string_view Text, int32_t X, int32_t Y, const Font::FColor& Color) const;
    // How far a line reaches from its cursor, to the right edge of its last bitmap.
    int32_t TextWidth(size_t Font, std::string_view Text) const;

    const std::vector<Gaf::FFrame>& GetBitmaps(EHudSheet Sheet) const;

private:
    void DrawText(std::vector<FHudSprite>& Out, size_t Font, int32_t X, int32_t Y, std::string_view Text, std::optional<int32_t> WrapRight = std::nullopt,
        const Font::FColor& Color = {}) const;

    std::vector<Gaf::FFrame> Bitmaps;
    std::vector<Gaf::FFrame> VBitmaps;
    std::vector<uint32_t> VBitmapWidths;
    std::vector<Font::FFont> Fonts;
    std::optional<Gaf::FAnimation> Background;
    std::optional<Gaf::FAnimation> Flash;
    std::optional<Gaf::FAnimation> Cursor;
};
