#include "Game/Menu.h"

#include "Pg/Random.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <format>

namespace
{
// MenuLoadArt at 00420560 loads both from the menu data; Common holds the only copies.
constexpr std::string_view MenuPath = "common\\data\\menu\\menu.gaf";
constexpr std::string_view VMenuPath = "common\\data\\menu\\v-menu.gaf";
// 1-based animations: MENU_BG00 and cursor of menu.gaf, VZfire of v-menu.gaf.
constexpr size_t BackgroundAnimation = 4;
constexpr size_t CursorAnimation = 8;
constexpr size_t FlashAnimation = 6;
// MenuStep at 00421780 flashes the VZ fire on one random draw in 87.
constexpr uint32_t FlashChance = 87;

// v-menu.gaf fonts: 4 for help texts and 5 for the build date.
constexpr size_t HelpFont = 4;
constexpr size_t DateFont = 5;
constexpr size_t FontCount = DateFont;

// Menu +0x10 and +0x14: where the title prints.
constexpr int32_t TitleX = 10;
constexpr int32_t TitleY = 120;
// Menu +0x28 to +0x34: the help text's clip, whose top left is the text's origin.
constexpr int32_t HelpLeft = 400;
constexpr int32_t HelpTop = 360;
constexpr int32_t HelpRight = 635;
// FUN_004220e0's bar: 20 marks, 100 pixels below the label, and the arrow keys' step and repeat in MenuStep.
constexpr int32_t SliderBarDrop = 100;
constexpr int32_t SliderMarks = 20;
constexpr int32_t SliderMax = 100;
constexpr int32_t SliderKeyStep = 5;
constexpr int32_t SliderRepeatMilliseconds = 150;
// MenuStep takes the mouse onto the bar between these heights below the menu's top.
constexpr int32_t SliderMouseTop = 0x5c;
constexpr int32_t SliderMouseBottom = 0x82;
// MenuDrawBuildDate at 00422340 prints the build date 15 pixels above the view's last row.
constexpr int32_t DateX = 4;
constexpr int32_t DateY = 479 - 15;
constexpr std::string_view BuildDate = "Apr 10 2000 17:52:06";

const Gaf::FAnimation* FindAnimation(const std::vector<Gaf::FAnimation>& Animations, const size_t Index)
{
    return Index >= 1 && Index <= Animations.size() && !Animations[Index - 1].Frames.empty() ? &Animations[Index - 1] : nullptr;
}

bool IsGap(const FMenu& Menu, const size_t Item)
{
    return Item >= Menu.Items.size() || Menu.Items[Item].Text.empty();
}

// The item row an arrow key starts from: rows lie StepY apart and the cursor is over row N between their tops and baselines.
int32_t CursorRow(const FMenu& Menu, const int32_t CursorY)
{
    return (CursorY - Menu.Top + Menu.StepY) / Menu.StepY;
}

// The arrow keys of MenuStep at 00421780: the item above or below the one under the cursor; when the keys already chose it, or it is a gap, the next item in that direction that has a text, wrapping around.
std::optional<size_t> ArrowItem(const FMenu& Menu, const FMenuState& State, const bool bDown)
{
    const int32_t Count = int32_t(Menu.Items.size());
    if (Count == 0)
    {
        return std::nullopt;
    }

    int32_t Item = 0;
    if (State.CursorY > Menu.Bottom)
    {
        Item = Count - 1;
    }
    else if (State.CursorY >= Menu.Top - Menu.StepY)
    {
        Item = CursorRow(Menu, State.CursorY);
        if (Item >= Count)
        {
            Item = Count - 1;
        }
        else if (bDown ? Item < Count - 1 : Item > 0)
        {
            Item += bDown ? 1 : -1;
        }
    }

    if ((State.KeyItem && *State.KeyItem == size_t(Item)) || IsGap(Menu, size_t(Item)))
    {
        const int32_t Direction = bDown ? 1 : -1;
        const auto Search = [&](int32_t From) -> std::optional<int32_t>
        {
            for (; From >= 0 && From < Count; From += Direction)
            {
                if (!IsGap(Menu, size_t(From)))
                {
                    return From;
                }
            }

            return std::nullopt;
        };
        const std::optional<int32_t> Found = Search(Item + Direction);
        const std::optional<int32_t> Wrapped = Found ? Found : Search(bDown ? 0 : Count - 1);
        Item = Wrapped.value_or(Item);
    }

    return size_t(Item);
}
}

std::expected<FMenuArt, std::string> FMenuArt::Load(FResourceArchive& Archive)
{
    auto Menu = LoadHudGaf(Archive, MenuPath);
    auto VMenu = Menu ? LoadHudGaf(Archive, VMenuPath) : std::unexpected(Menu.error());
    std::optional<std::vector<Font::FFont>> Fonts = VMenu ? LoadGafFonts(Archive, VMenuPath) : std::nullopt;
    if (!VMenu)
    {
        return std::unexpected(VMenu.error());
    }

    if (!Fonts || Fonts->size() < FontCount)
    {
        return std::unexpected(std::format("{} lacks its fonts", VMenuPath));
    }

    FMenuArt Result;
    const auto Copy = [](const Gaf::FAnimation* const Animation) { return Animation ? std::optional(*Animation) : std::nullopt; };
    Result.Background = Copy(FindAnimation(Menu->second, BackgroundAnimation));
    Result.Cursor = Copy(FindAnimation(Menu->second, CursorAnimation));
    Result.Flash = Copy(FindAnimation(VMenu->second, FlashAnimation));
    Result.Animations = Menu->second;
    Result.Bitmaps = std::move(Menu->first);
    Result.VBitmaps = std::move(VMenu->first);
    Result.VBitmapWidths = BitmapWidthsOf(Result.VBitmaps);
    Result.Fonts = std::move(*Fonts);
    return Result;
}

FMenuState FMenuArt::Start(const FMenu& Menu) const
{
    FMenuState State{
        .CursorX = (Menu.Right - Menu.Left) / 2 + Menu.Left,
        .CursorY = Menu.StepY * int32_t(Menu.StartItem) - Menu.StepY / 2 + Menu.Top,
        .SliderValue = Menu.SliderValue,
    };
    if (Cursor)
    {
        State.CursorPlayback = StartAnimation(*Cursor);
    }

    return State;
}

std::optional<std::pair<int32_t, int32_t>> FMenuArt::Step(const FMenu& Menu, FMenuState& State, const FMenuInput& Input, FRandom& Random,
    const int32_t StepMilliseconds) const
{
    if (Flash && Random.Next() % FlashChance == 0)
    {
        State.bFlash = true;
        ++State.Flashes;
    }

    State.CursorX = Input.MouseX;
    State.CursorY = Input.MouseY;
    // The slider follows the held button over its bar, else Left and Right; the port keeps the mouse's value within the bar's range.
    if (Menu.bSlider)
    {
        if (State.SliderRepeat > 0)
        {
            State.SliderRepeat -= StepMilliseconds;
        }

        const bool bOverBar = Input.bButtonHeld && Menu.Left < Input.MouseX && Input.MouseX < Menu.Right && Menu.Top + SliderMouseTop < Input.MouseY &&
            Input.MouseY < Menu.Top + SliderMouseBottom;
        if (bOverBar && State.SliderEnd && *State.SliderEnd > Menu.Left)
        {
            State.SliderValue = std::clamp((Input.MouseX - Menu.Left) * SliderMax / (*State.SliderEnd - Menu.Left), 0, SliderMax);
        }
        else if (!bOverBar && State.SliderRepeat < 1 && (Input.bLeftHeld || Input.bRightHeld))
        {
            State.SliderRepeat = SliderRepeatMilliseconds;
            State.SliderValue = std::clamp(State.SliderValue + (Input.bLeftHeld ? -SliderKeyStep : SliderKeyStep), 0, SliderMax);
        }
    }

    State.UnderCursor.reset();
    for (size_t Item = 0; Item < Menu.Items.size(); ++Item)
    {
        const int32_t X = Menu.Left + int32_t(Item) * Menu.StepX;
        const int32_t Y = Menu.Top + int32_t(Item) * Menu.StepY;
        if (X < State.CursorX && State.CursorX < Menu.Right && Y - Menu.StepY < State.CursorY && State.CursorY < Y)
        {
            State.UnderCursor = Item;
            State.Selected = IsGap(Menu, Item) ? std::nullopt : std::optional(Item);
        }
    }

    if (State.bCursorShown && Cursor)
    {
        UpdateAnimation(State.CursorPlayback, *Cursor, StepMilliseconds);
    }

    if (Input.bClick && State.Selected)
    {
        State.Result = State.Selected;
        return std::nullopt;
    }

    if (Input.bCancel && Menu.DefaultItem)
    {
        State.Result = Menu.DefaultItem;
        return std::nullopt;
    }

    if (Input.bAccept && State.Selected)
    {
        State.Result = State.Selected;
        return std::nullopt;
    }

    if (Input.bUp || Input.bDown)
    {
        if (const std::optional<size_t> Item = ArrowItem(Menu, State, Input.bDown))
        {
            State.KeyItem = Item;
            State.bCursorShown = false;
            return std::pair((Menu.Right - Menu.Left + Menu.StepY) / 4 + Menu.Left, (int32_t(*Item) - 1) * Menu.StepY + Menu.StepY / 2 + Menu.Top);
        }
    }

    if (!State.bCursorShown && Input.bMouseMoved)
    {
        State.bCursorShown = true;
    }

    return std::nullopt;
}

std::vector<FHudSprite> FMenuArt::Draw(const FMenu& Menu, FMenuState& State) const
{
    std::vector<FHudSprite> Out;
    const auto DrawFrame = [&](const EHudSheet Sheet, const Gaf::FAnimation& Animation, const uint32_t Frame, const int32_t X, const int32_t Y)
    {
        if (Frame >= 1 && Frame <= Animation.Frames.size())
        {
            Out.push_back({ .Sheet = Sheet, .Frame = Animation.Frames[Frame - 1], .X = float(X), .Y = float(Y) });
        }
    };
    if (Background)
    {
        DrawFrame(EHudSheet::Menu, *Background, 1, 0, 0);
    }

    if (Flash && State.bFlash)
    {
        State.bFlash = false;
        DrawFrame(EHudSheet::VMenu, *Flash, 1, 0, 0);
    }

    // The path continues from where the title ends.
    constexpr size_t PathFont = 3;
    constexpr size_t SubPathFont = 4;
    int32_t TitleEnd = DrawText(Out, Menu.TitleFont, TitleX, TitleY, Menu.Title);
    TitleEnd = DrawText(Out, PathFont, TitleEnd, TitleY, Menu.Path);
    DrawText(Out, SubPathFont, TitleEnd, TitleY, Menu.SubPath);
    if (Menu.bSlider)
    {
        DrawSlider(Out, Menu, State);
    }

    for (size_t Item = 0; Item < Menu.Items.size(); ++Item)
    {
        DrawText(Out, State.UnderCursor == Item ? Menu.HighlightFont : Menu.ItemFont, Menu.Left + int32_t(Item) * Menu.StepX, Menu.Top + int32_t(Item) * Menu.StepY,
            Menu.Items[Item].Text);
    }

    if (State.Selected && !Menu.Items[*State.Selected].Help.empty())
    {
        const Font::FFont& Help = Fonts[HelpFont - 1];
        DrawText(Out, HelpFont, HelpLeft, HelpTop + Help.LineHeight - Help.LineGap, Menu.Items[*State.Selected].Help, HelpRight);
    }

    DrawText(Out, DateFont, DateX, DateY, BuildDate);
    if (Menu.Picture)
    {
        if (const Gaf::FAnimation* const Picture = FindAnimation(Animations, *Menu.Picture))
        {
            constexpr int32_t PictureY = 480;
            DrawFrame(EHudSheet::Menu, *Picture, 1, 0, PictureY);
        }
    }

    if (State.bCursorShown && Cursor)
    {
        DrawFrame(EHudSheet::Menu, *Cursor, State.CursorPlayback.bEnded ? 0 : State.CursorPlayback.Frame, State.CursorX, State.CursorY);
    }

    return Out;
}

std::vector<FHudSprite> FMenuArt::DrawBackground() const
{
    std::vector<FHudSprite> Out;
    if (Background && !Background->Frames.empty())
    {
        Out.push_back({ .Sheet = EHudSheet::Menu, .Frame = Background->Frames.front() });
    }

    return Out;
}

std::vector<FHudSprite> FMenuArt::DrawLine(const size_t Font, const std::string_view Text, const int32_t X, const int32_t Y, const Font::FColor& Color) const
{
    std::vector<FHudSprite> Out;
    DrawText(Out, Font, X, Y, Text, std::nullopt, Color);
    return Out;
}

int32_t FMenuArt::TextWidth(const size_t Font, const std::string_view Text) const
{
    int32_t Width = 0;
    if (Font >= 1 && Font <= Fonts.size())
    {
        for (const Font::FPlacedGlyph& Glyph : Font::LayoutText(Fonts[Font - 1], Text, 0, 0, VBitmapWidths))
        {
            if (Glyph.BitmapIndex >= 1 && Glyph.BitmapIndex <= VBitmapWidths.size())
            {
                Width = std::max(Width, Glyph.X + int32_t(VBitmapWidths[Glyph.BitmapIndex - 1]));
            }
        }
    }

    return Width;
}

std::vector<FHudSprite> FMenuArt::DrawPage(const size_t Font, const std::string_view Text, const int32_t Left, const int32_t Top, const int32_t Right) const
{
    std::vector<FHudSprite> Out;
    DrawText(Out, Font, Left, Top, Text, Right);
    return Out;
}

const std::vector<Gaf::FFrame>& FMenuArt::GetBitmaps(const EHudSheet Sheet) const
{
    return Sheet == EHudSheet::VMenu ? VBitmaps : Bitmaps;
}

int32_t FMenuArt::DrawText(std::vector<FHudSprite>& Out, const size_t Font, const int32_t X, const int32_t Y, const std::string_view Text,
    const std::optional<int32_t> WrapRight, const Font::FColor& Color) const
{
    if (Font < 1 || Font > Fonts.size())
    {
        return X;
    }

    AppendText(Out, EHudSheet::VMenu, Fonts[Font - 1], VBitmaps, VBitmapWidths, X, Y, Text, Color, WrapRight);
    int32_t End = X;
    Font::LayoutText(Fonts[Font - 1], Text, X, Y, VBitmapWidths, WrapRight, &End);
    return End;
}

int32_t FMenuArt::DrawText(std::vector<FHudSprite>& Out, const Font::FFont& Font, const int32_t X, const int32_t Y, const std::string_view Text) const
{
    AppendText(Out, EHudSheet::VMenu, Font, VBitmaps, VBitmapWidths, X, Y, Text, {}, std::nullopt);
    int32_t End = X;
    Font::LayoutText(Font, Text, X, Y, VBitmapWidths, std::nullopt, &End);
    return End;
}

void FMenuArt::DrawSlider(std::vector<FHudSprite>& Out, const FMenu& Menu, FMenuState& State) const
{
    constexpr size_t LabelFont = 1;
    const int32_t Value = std::clamp(State.SliderValue, 0, SliderMax);
    DrawText(Out, LabelFont, Menu.Left, TitleY, Menu.SliderLabel.empty() ? std::format("Value : {} %", Value) : std::format("{} {} %", Menu.SliderLabel, Value));
    if (Menu.HighlightFont < 1 || Menu.HighlightFont > Fonts.size() || Menu.ItemFont < 1 || Menu.ItemFont > Fonts.size())
    {
        return;
    }

    // The bar's marks print fixed width, one pixel apart, in the highlighted font up to the value and the item font beyond.
    const auto Monospaced = [&](const size_t Index)
    {
        Font::FFont Copy = Fonts[Index - 1];
        Copy.Flags &= ~Font::FontProportional;
        Copy.CharSpacing = 1;
        return Copy;
    };
    const Font::FFont Filled = Monospaced(Menu.HighlightFont);
    const Font::FFont Empty = Monospaced(Menu.ItemFont);
    const int32_t Marks = Value * SliderMarks / SliderMax;
    const int32_t Y = TitleY + SliderBarDrop;
    int32_t X = DrawText(Out, Filled, Menu.Left, Y, "[" + std::string(size_t(Marks), '|'));
    X = DrawText(Out, Empty, X, Y, std::string(size_t(SliderMarks - Marks), '|'));
    State.SliderEnd = DrawText(Out, Filled, X, Y, "]");
}
