#include "Game/Panel.h"

#include "Game/Level.h"
#include "Pg/ResourceArchive.h"

#include <algorithm>
#include <format>

namespace
{
// PanelLoad at 00434d40 loads these with VzLoadGaf; a zone's own Data\Panel would come first, but only Common has one.
constexpr std::string_view PanelPath = "common\\data\\panel\\panel.gaf";
constexpr std::string_view VPanelPath = "common\\data\\panel\\v-panel.gaf";
// FUN_00434910 loads one icon per weapon slot and plays its first animation.
constexpr std::array<std::string_view, 9> WeaponIconPaths = {
    "common\\data\\object\\item\\icon-gatling.gaf",
    "common\\data\\object\\item\\icon-grenade.gaf",
    "common\\data\\object\\item\\icon-rocket.gaf",
    "common\\data\\object\\item\\icon-fire.gaf",
    "common\\data\\object\\item\\icon-saw.gaf",
    "common\\data\\object\\item\\icon-missile.gaf",
    "common\\data\\object\\item\\icon-laser.gaf",
    "common\\data\\object\\item\\icon-pulsar.gaf",
    "common\\data\\object\\item\\icon-mine.gaf",
};

// FUN_0041b830 loads the surprise icons.
constexpr std::string_view SurprisePath = "common\\data\\object\\item\\surprise.gaf";

// MessageLoadFonts at 00430d80 loads the message fonts; Data\Extra\v-bonus.gaf falls back to Common's, the only one there is.
constexpr std::string_view BonusPath = "common\\data\\extra\\v-bonus.gaf";

// 0-based slot indices into FPanel::Slots; slot N plays animation N + 1 of its GAF.
constexpr size_t EnergyWarningSlot = 0;
constexpr size_t WeaponBaseSlot = 1;
constexpr size_t ScreenSlot = 2;
// Slot 3 (Weapons4) replaces slot 4 (Weapons3) while the player's acceleration differs from its class value.
constexpr size_t BoostSlot = 3;
constexpr size_t WeaponIdleSlot = 4;
// The TV borders: hbar2 along the top, hbar1 the bottom, vbar2 the left and vbar1 the right.
constexpr size_t TopBorderSlot = 6;
constexpr size_t BottomBorderSlot = 7;
constexpr size_t LeftBorderSlot = 8;
constexpr size_t RightBorderSlot = 9;
constexpr size_t WeaponLightsSlot = 5;
constexpr size_t ShieldBarSlot = 10;
constexpr size_t EnergyBarSlot = 11;
constexpr size_t InfosSlot = 12;
constexpr size_t InfosBaseSlot = 13;
constexpr size_t ScoreSlot = 14;
constexpr size_t ScoreBaseSlot = 15;
constexpr size_t SpeedSlot = 16;
constexpr size_t SpeedTopSlot = 17;
constexpr size_t SpeedSideSlot = 18;
// The border pieces and black_square come from v-panel.gaf.
constexpr std::array<size_t, 6> VPanelSlots = { ScreenSlot, 6, 7, 8, 9, 21 };

// 1-based font indices of PgFontSelect (0044c3c0).
constexpr size_t ScoreFont = 1;
constexpr size_t SpeedFont = 2;
constexpr size_t ScreenFont = 3;

// The least panel mode of each block: the borders and text line from 1, the bars from 2, the score and weapon from 3, the speedometer and radar from 4 and the screen at 5.
constexpr int TextMode = 1;
constexpr int BarsMode = 2;
constexpr int ScoreMode = 3;
constexpr int SpeedMode = 4;
constexpr int ScreenMode = 5;

// While the weapon in hand has rounds (its bound ammo stock, object +0x248, holds some), PanelUpdate shows the weapon lights only on one RandomBelow(53) draw of the Misc stream in 53, in either pass.
bool WeaponLightsShown(const FPanelValues& Values, FRandom& Misc)
{
    constexpr int FlickerOdds = 0x35;
    constexpr int ShownDraw = 1;
    return Values.Ammo == 0.f || Misc.Below(FlickerOdds) == ShownDraw;
}

// "DANGER: SHIELD IS LOW" and the blinking warning start at 1 percent shield.
constexpr float ShieldLowRatio = 0.01f;
// The screen slot is drawn with draw mode 1, or 5 with option flag 0x10 ("Translucent Panel"): DrawTexturedPolygon gives mode 5 a fixed alpha of 0x80.
constexpr uint32_t OpaqueScreenDrawMode = 1;
constexpr uint32_t ScreenDrawMode = 5;
constexpr float ScreenAlpha = float(0x80) / 255.f;

bool ShieldLow(const FPanelValues& Values)
{
    return Values.ShieldMax != 0.f && Values.Shield / Values.ShieldMax <= ShieldLowRatio;
}

// The x87 multiply-then-divide of a bar, truncated by __ftol.
int32_t BarColumn(const uint32_t Width, const float Value, const float Max)
{
    return Max == 0.f ? 0 : int32_t(double(Width) * double(Value) / double(Max));
}
}

std::optional<std::vector<Font::FFont>> LoadGafFonts(FResourceArchive& Archive, const std::string_view Path)
{
    const std::optional<std::vector<uint8_t>> File = Archive.Read(Path);
    return File ? Font::LoadFonts(*File) : std::nullopt;
}

std::vector<uint32_t> BitmapWidthsOf(const std::vector<Gaf::FFrame>& Bitmaps)
{
    std::vector<uint32_t> Widths;
    for (const Gaf::FFrame& Bitmap : Bitmaps)
    {
        Widths.push_back(Bitmap.Width);
    }

    return Widths;
}
std::expected<std::pair<std::vector<Gaf::FFrame>, std::vector<Gaf::FAnimation>>, std::string> LoadHudGaf(FResourceArchive& Archive, const std::string_view Path)
{
    const std::optional<std::vector<uint8_t>> File = Archive.Read(Path);
    std::optional<std::vector<Gaf::FFrame>> Bitmaps = File ? Gaf::LoadFrames(*File) : std::nullopt;
    std::optional<std::vector<Gaf::FAnimation>> Animations = Bitmaps ? Gaf::LoadAnimations(*File, Bitmaps->size()) : std::nullopt;
    if (!Animations)
    {
        return std::unexpected(std::format("Cannot load {}", Path));
    }

    return std::pair(std::move(*Bitmaps), std::move(*Animations));
}

void AppendText(std::vector<FHudSprite>& Out, const EHudSheet Sheet, const Font::FFont& Font, const std::span<const Gaf::FFrame> Bitmaps,
    const std::span<const uint32_t> BitmapWidths, const int32_t X, const int32_t Y, const std::string_view Text, const Font::FColor& Color,
    const std::optional<int32_t> WrapRight)
{
    // DrawTexturedPolygon gives modes 4 and 5 a fixed alpha of 0x80.
    constexpr uint32_t HalfAlphaDrawMode = 4;
    constexpr uint32_t ColorKeyBit = 1;
    constexpr float HalfAlpha = float(0x80) / 255.f;
    const Font::FColor Combined = Font.Color.Combine(Color);
    const bool bHalfAlpha = (Font.DrawMode & ~ColorKeyBit) == HalfAlphaDrawMode;
    for (const Font::FPlacedGlyph& Glyph : Font::LayoutText(Font, Text, X, Y, BitmapWidths, WrapRight))
    {
        if (Glyph.BitmapIndex == 0 || Glyph.BitmapIndex > Bitmaps.size())
        {
            continue;
        }

        // The sprite draw adds the bitmap's trim, which the glyph position already includes.
        const Gaf::FFrame& Bitmap = Bitmaps[Glyph.BitmapIndex - 1];
        Out.push_back({
            .Sheet = Sheet,
            .Frame = { .BitmapIndex = Glyph.BitmapIndex, .OffsetX = -Bitmap.TrimX, .OffsetY = -Bitmap.TrimY, .DrawFlags = Font.DrawMode },
            .X = float(Glyph.X),
            .Y = float(Glyph.Y),
            .Diffuse = Combined.Diffuse,
            .Specular = Combined.Specular,
            .Alpha = bHalfAlpha ? HalfAlpha : 1.f,
        });
    }
}

namespace
{
constexpr uint32_t FirstFrame = 1;
constexpr uint32_t CurrentFrame = 0;
}

std::expected<FPanel, std::string> FPanel::Load(FResourceArchive& Archive)
{
    auto Panel = LoadHudGaf(Archive, PanelPath);
    auto VPanel = Panel ? LoadHudGaf(Archive, VPanelPath) : std::unexpected(Panel.error());
    auto Bonus = VPanel ? LoadHudGaf(Archive, BonusPath) : std::unexpected(VPanel.error());
    if (!Bonus)
    {
        return std::unexpected(Bonus.error());
    }

    std::optional<std::vector<Font::FFont>> Fonts = LoadGafFonts(Archive, PanelPath);
    std::optional<std::vector<Font::FFont>> BonusFonts = LoadGafFonts(Archive, BonusPath);
    constexpr size_t FontCount = 3;
    constexpr size_t BonusFontCount = 6;
    if (!Fonts || Fonts->size() < FontCount || !BonusFonts || BonusFonts->size() < BonusFontCount)
    {
        return std::unexpected(std::format("{} or {} lacks its fonts", PanelPath, BonusPath));
    }

    FPanel Result;
    Result.Bitmaps = std::move(Panel->first);
    Result.VBitmaps = std::move(VPanel->first);
    Result.BonusBitmaps = std::move(Bonus->first);
    Result.Fonts = std::move(*Fonts);
    Result.BonusFonts = std::move(*BonusFonts);
    Result.BitmapWidths = BitmapWidthsOf(Result.Bitmaps);
    Result.BonusBitmapWidths = BitmapWidthsOf(Result.BonusBitmaps);
    if (auto Surprise = LoadHudGaf(Archive, SurprisePath))
    {
        Result.SurpriseBitmaps = std::move(Surprise->first);
    }

    static_assert(WeaponIconPaths.size() == WeaponIconCount);
    for (size_t Icon = 0; Icon < WeaponIconCount; ++Icon)
    {
        auto Gaf = LoadHudGaf(Archive, WeaponIconPaths[Icon]);
        if (!Gaf || Gaf->second.empty())
        {
            continue;
        }

        // The icons share one sheet, so each animation's bitmap indices move past the icons before it.
        const uint32_t Offset = uint32_t(Result.IconBitmaps.size());
        Gaf::FAnimation Animation = Gaf->second.front();
        for (Gaf::FAnimationFrame& Frame : Animation.Frames)
        {
            Frame.BitmapIndex += Frame.BitmapIndex != 0 ? Offset : 0;
        }

        Result.IconBitmaps.insert(Result.IconBitmaps.end(), Gaf->first.begin(), Gaf->first.end());
        Result.Icons[Icon] = { .Sheet = EHudSheet::Icons, .Animation = Animation, .Playback = StartAnimation(Animation) };
    }

    for (size_t Slot = 0; Slot < SlotCount; ++Slot)
    {
        const bool bVPanel = std::ranges::contains(VPanelSlots, Slot);
        const std::vector<Gaf::FAnimation>& Animations = bVPanel ? VPanel->second : Panel->second;
        if (Slot < Animations.size())
        {
            Result.Slots[Slot] = {
                .Sheet = bVPanel ? EHudSheet::VPanel : EHudSheet::Panel,
                .Animation = Animations[Slot],
                .Playback = StartAnimation(Animations[Slot]),
            };
        }
    }

    return Result;
}

void FPanel::Step(const FPanelValues& Values, const int32_t StepMilliseconds, FRandom& Misc)
{
    const auto Update = [&](const size_t Slot) { UpdateAnimation(Slots[Slot].Playback, Slots[Slot].Animation, StepMilliseconds); };
    if (Mode >= ScoreMode)
    {
        Update(ScoreSlot);
    }

    if (Mode >= BarsMode && ShieldLow(Values))
    {
        Update(EnergyWarningSlot);
    }

    if (Mode >= ScoreMode)
    {
        Update(Values.bBoost ? BoostSlot : WeaponIdleSlot);
        if (WeaponLightsShown(Values, Misc))
        {
            Update(WeaponLightsSlot);
        }

        // Only the icon of the slot in hand plays.
        if (Values.WeaponSlot >= 1 && Values.WeaponSlot <= WeaponIconCount)
        {
            FSlot& Icon = Icons[Values.WeaponSlot - 1];
            UpdateAnimation(Icon.Playback, Icon.Animation, StepMilliseconds);
        }
    }

    // The infos light plays while the player is locked.
    if (Mode >= ScreenMode && Values.bLocked)
    {
        Update(InfosSlot);
    }
}

void FPanel::CyclePanels(const bool bReverse)
{
    constexpr int TopMode = ScreenMode;
    const int Next = Mode + (bReverse ? -ModeStep : ModeStep);
    Mode = Next < 0 ? -Next : Next > TopMode ? 2 * TopMode - Next : Next;
    if (!bReverse && (Mode == 0 || Mode == TopMode))
    {
        ModeStep = -ModeStep;
    }
}

void FPanel::ShowMap(const bool bShown)
{
    if (bShown)
    {
        SavedMode = Mode;
        Mode = TextMode;
        return;
    }

    Mode = SavedMode;
}

std::optional<FHudSprite> FPanel::DrawAnimation(const size_t Animation, const float X, const float Y) const
{
    std::vector<FHudSprite> Out;
    if (Animation >= 1 && Animation <= SlotCount)
    {
        DrawSlot(Out, Animation - 1, X, Y, CurrentFrame);
    }

    return Out.empty() ? std::nullopt : std::optional(Out.front());
}

void FPanel::DrawSlot(std::vector<FHudSprite>& Out, const size_t Slot, const float X, const float Y, const uint32_t Frame, const uint32_t VisibleColumns) const
{
    const FSlot& Source = Slots[Slot];
    const uint32_t Shown = Frame != CurrentFrame ? Frame : Source.Playback.bEnded ? 0 : Source.Playback.Frame;
    if (Shown == 0 || Shown > Source.Animation.Frames.size())
    {
        return;
    }

    Out.push_back({ .Sheet = Source.Sheet, .Frame = Source.Animation.Frames[Shown - 1], .X = X, .Y = Y, .VisibleColumns = VisibleColumns });
}

const std::vector<Gaf::FFrame>& FPanel::GetBitmaps(const EHudSheet Sheet) const
{
    return Sheet == EHudSheet::VPanel    ? VBitmaps
           : Sheet == EHudSheet::Bonus   ? BonusBitmaps
           : Sheet == EHudSheet::Icons   ? IconBitmaps
           : Sheet == EHudSheet::Surprise ? SurpriseBitmaps
                                          : Bitmaps;
}

// Selecting a font (PgFontSelect at 0044c3c0) takes its draw mode and colour, and PgFontSetColor (0044cc30) combines the caller's colour into that.
void FPanel::DrawText(std::vector<FHudSprite>& Out, const EHudSheet Sheet, const size_t Font, const int32_t X, const int32_t Y, const std::string_view Text,
    const Font::FColor& Color, const std::optional<int32_t> WrapRight) const
{
    const bool bBonus = Sheet == EHudSheet::Bonus;
    const std::vector<Font::FFont>& SheetFonts = bBonus ? BonusFonts : Fonts;
    if (Font != 0 && Font <= SheetFonts.size())
    {
        AppendText(Out, Sheet, SheetFonts[Font - 1], GetBitmaps(Sheet), bBonus ? BonusBitmapWidths : BitmapWidths, X, Y, Text, Color, WrapRight);
    }
}

void FPanel::DrawMessage(std::vector<FHudSprite>& Out, const FMessage& Message, const int32_t X, const int32_t Y, const std::string_view Text) const
{
    // An animation message draws its frame in the frame's own mode, or in mode 3 by its fading alpha.
    if (Message.Animation)
    {
        constexpr float MaxAlpha = 255.f;
        const uint32_t Frame = Message.Playback.bEnded ? 0 : Message.Playback.Frame;
        if (Frame >= 1 && Frame <= Message.Animation->Frames.size())
        {
            Out.push_back({
                .Sheet = EHudSheet::Surprise,
                .Frame = Message.Animation->Frames[Frame - 1],
                .X = float(X),
                .Y = float(Y),
                .Alpha = Message.bFading ? float(Message.FadeAlpha) / MaxAlpha : 1.f,
            });
        }

        return;
    }

    DrawText(Out, Message.Gaf == EMessageGaf::Bonus ? EHudSheet::Bonus : EHudSheet::Panel, Message.Font, X, Y, Text, Message.Color);
}

std::vector<FHudSprite> FPanel::DrawShade(const int ViewWidth, const int ViewHeight) const
{
    constexpr size_t BlackSquareSlot = 21;
    constexpr int ShadePasses = 2;
    constexpr int ShadeLeft = 32;
    constexpr int ShadeTop = 16;
    std::vector<FHudSprite> Out;
    const FSlot& Square = Slots[BlackSquareSlot];
    if (Square.Animation.Frames.empty())
    {
        return Out;
    }

    const Gaf::FAnimationFrame& Frame = Square.Animation.Frames.front();
    const std::vector<Gaf::FFrame>& SheetBitmaps = GetBitmaps(Square.Sheet);
    if (Frame.BitmapIndex == 0 || Frame.BitmapIndex > SheetBitmaps.size())
    {
        return Out;
    }

    const int Width = int(SheetBitmaps[Frame.BitmapIndex - 1].Width);
    const int Height = int(SheetBitmaps[Frame.BitmapIndex - 1].Height);
    if (Width <= 0 || Height <= 0)
    {
        return Out;
    }

    for (int Pass = 0; Pass < ShadePasses; ++Pass)
    {
        for (int Y = ShadeTop; Y + Height < ViewHeight; Y += Height)
        {
            for (int X = ShadeLeft; X + Width < ViewWidth; X += Width)
            {
                Out.push_back({ .Sheet = Square.Sheet, .Frame = Frame, .X = float(X), .Y = float(Y) });
            }
        }
    }

    return Out;
}

std::vector<FHudSprite> FPanel::DrawTypedPage(const size_t Font, const std::string_view Text, const size_t Typed, const int ViewWidth, const int ViewHeight) const
{
    // Proportions of the 640x480 screen: the text starts at 64, 80 and its lines are 25 apart.
    constexpr int ReferenceWidth = 640;
    constexpr int ReferenceHeight = 480;
    constexpr int Left = 64;
    constexpr int Top = 80;
    constexpr int LineStep = 25;
    constexpr int32_t TabStop = 64;
    std::vector<FHudSprite> Out;
    if (Font == 0 || Font > BonusFonts.size())
    {
        return Out;
    }

    const Font::FFont& PageFont = BonusFonts[Font - 1];
    const int32_t LineLeft = ViewWidth * Left / ReferenceWidth;
    const int32_t LineAdvance = ViewHeight * LineStep / ReferenceHeight;
    int32_t Line = ViewHeight * Top / ReferenceHeight;
    int32_t Cursor = LineLeft;
    for (size_t Index = 0; Index < std::min(Typed, Text.size()); ++Index)
    {
        const char Character = Text[Index];
        if (Character == '\n')
        {
            Line += LineAdvance;
            Cursor = LineLeft;
            continue;
        }

        if (Character == '\t')
        {
            Cursor = (Cursor & ~(TabStop - 1)) + TabStop;
        }

        AppendText(Out, EHudSheet::Bonus, PageFont, BonusBitmaps, BonusBitmapWidths, Cursor, Line, std::string_view(&Text[Index], 1), {}, std::nullopt);
        int32_t End = Cursor;
        Font::LayoutText(PageFont, std::string_view(&Text[Index], 1), Cursor, Line, BonusBitmapWidths, std::nullopt, &End);
        Cursor = End;
    }

    return Out;
}

std::vector<FHudSprite> FPanel::DrawPage(const size_t Font, const std::string_view Text, const int32_t Left, const int32_t Top, const int32_t Right) const
{
    // The text page's callback (00402640) sets the cursor to the font's baseline (font +0x28) below the page's top.
    std::vector<FHudSprite> Out;
    const int32_t Baseline = Font >= 1 && Font <= BonusFonts.size() ? BonusFonts[Font - 1].Baseline : 0;
    DrawText(Out, EHudSheet::Bonus, Font, Left, Top + Baseline, Text, {}, Right);
    return Out;
}

void FPanel::PostWarnings(const FPanelValues& Values, FMessages& Messages) const
{
    // Both need option flag 8 ("HUD Green Info"). The lock warning shows in every mode but 0, the shield warning with the bars.
    if (!Details.bGreenInfo)
    {
        return;
    }

    constexpr int32_t WarningX = 100;
    constexpr int32_t LockWarningY = 90;
    constexpr int32_t ShieldWarningY = 100;
    constexpr uint32_t WarningLifeMilliseconds = 500;
    const auto Warn = [&](const int32_t Y, const std::string_view Text)
    {
        if (Messages.Exists(WarningX, Y))
        {
            Messages.SetLife(WarningX, Y, WarningLifeMilliseconds);
            return;
        }

        Messages.Add({
            .Text = std::string(Text),
            .X = float(WarningX),
            .Y = float(Y),
            .LifeMilliseconds = WarningLifeMilliseconds,
            .bTypewriter = true,
            .Gaf = EMessageGaf::Panel,
            .Font = ScreenFont,
        }, EMessagePlacement::Replace);
    };
    if (Mode >= TextMode && Values.bLocked)
    {
        Warn(LockWarningY, "WARNING : YOU ARE LOCKED");
    }

    if (Mode >= BarsMode && ShieldLow(Values))
    {
        Warn(ShieldWarningY, "DANGER : SHIELD IS LOW");
    }
}

std::vector<FHudSprite> FPanel::Draw(const FPanelValues& Values, const std::span<const FRadarDot> Radar, const int ViewWidth, const int ViewHeight,
    FRandom& Misc) const
{
    std::vector<FHudSprite> Out;
    const float Left = 0.f;
    const float Top = 0.f;
    const float Right = float(ViewWidth - 1);
    const float Bottom = float(ViewHeight - 1);
    constexpr uint32_t MillisecondsPerMinute = 60000;
    constexpr uint32_t MillisecondsPerSecond = 1000;
    if (Mode >= TextMode && Details.bTvBorders)
    {
        // The TV borders (option flag 0x20): each bar stretched along its edge of the view.
        const auto FrameSize = [&](const size_t Slot)
        {
            const FSlot& Source = Slots[Slot];
            const uint32_t Bitmap = Source.Animation.Frames.empty() ? 0 : Source.Animation.Frames.front().BitmapIndex;
            const std::vector<Gaf::FFrame>& SheetBitmaps = GetBitmaps(Source.Sheet);
            return Bitmap != 0 && Bitmap <= SheetBitmaps.size() ? std::pair(SheetBitmaps[Bitmap - 1].Width, SheetBitmaps[Bitmap - 1].Height) : std::pair(0u, 0u);
        };
        const auto Stretched = [&](const size_t Slot, const float X, const float Y, const float ScaleX, const float ScaleY)
        {
            const size_t First = Out.size();
            DrawSlot(Out, Slot, X, Y, CurrentFrame);
            for (size_t Index = First; Index < Out.size(); ++Index)
            {
                Out[Index].ScaleX = ScaleX;
                Out[Index].ScaleY = ScaleY;
            }
        };
        const uint32_t BorderWidth = FrameSize(TopBorderSlot).first;
        const uint32_t BorderHeight = FrameSize(LeftBorderSlot).second;
        const float ScaleX = BorderWidth == 0 ? 1.f : float(ViewWidth) / float(BorderWidth);
        const float ScaleY = BorderHeight == 0 ? 1.f : float(ViewHeight) / float(BorderHeight);
        Stretched(TopBorderSlot, Left, Top, ScaleX, 1.f);
        Stretched(LeftBorderSlot, Left, Top, 1.f, ScaleY);
        Stretched(RightBorderSlot, Right, Top, 1.f, ScaleY);
        Stretched(BottomBorderSlot, Left, Bottom, ScaleX, 1.f);
    }

    if (Mode >= ScoreMode)
    {
        // Score, top left.
        constexpr int32_t ScoreTextX = 0x24;
        constexpr int32_t ScoreTextY = 0x1e;
        DrawSlot(Out, ScoreSlot, Left, Top, FirstFrame);
        DrawSlot(Out, ScoreSlot, Left, Top, CurrentFrame);
        DrawSlot(Out, ScoreBaseSlot, Left, Top, FirstFrame);
        DrawText(Out, EHudSheet::Panel, ScoreFont, ScoreTextX, ScoreTextY, std::format("{:08} $", int32_t(Values.Money)));
    }

    if (Mode >= BarsMode)
    {
        // Shield and energy bars, top centre; each bitmap is clipped to its ratio (pgbmp FUN_0044ed60 with an inclusive right edge).
        const float Centre = float(ViewWidth >> 1);
        DrawSlot(Out, EnergyWarningSlot, Centre, Top, FirstFrame);
        if (ShieldLow(Values))
        {
            DrawSlot(Out, EnergyWarningSlot, Centre, Top, CurrentFrame);
        }

        const auto BarColumns = [&](const size_t Slot, const float Value, const float Max)
        {
            const FSlot& Bar = Slots[Slot];
            const uint32_t Bitmap = Bar.Animation.Frames.empty() ? 0 : Bar.Animation.Frames.front().BitmapIndex;
            const uint32_t Width = Bitmap != 0 && Bitmap <= Bitmaps.size() ? Bitmaps[Bitmap - 1].Width : 0;
            return uint32_t(std::clamp(BarColumn(Width, Value, Max) + 1, 0, int32_t(Width)));
        };
        DrawSlot(Out, ShieldBarSlot, Centre, Top, CurrentFrame, BarColumns(ShieldBarSlot, Values.Shield, Values.ShieldMax));
        DrawSlot(Out, EnergyBarSlot, Centre, Top, CurrentFrame, BarColumns(EnergyBarSlot, Values.Energy, Values.EnergyMax));
    }

    if (Mode >= ScoreMode)
    {
        // Weapon block, top right, with the icon of the slot in hand on its screen.
        constexpr int32_t AmmoTextX = -0x6b;
        constexpr int32_t AmmoTextY = 9;
        constexpr float IconX = -0x3c;
        constexpr float IconY = 0x46;
        DrawSlot(Out, WeaponBaseSlot, Right, Top, FirstFrame);
        DrawSlot(Out, Values.bBoost ? BoostSlot : WeaponIdleSlot, Right, Top, CurrentFrame);
        if (WeaponLightsShown(Values, Misc))
        {
            DrawSlot(Out, WeaponLightsSlot, Right, Top, CurrentFrame);
        }

        if (Values.WeaponSlot >= 1 && Values.WeaponSlot <= WeaponIconCount)
        {
            const FSlot& Icon = Icons[Values.WeaponSlot - 1];
            const uint32_t Frame = Icon.Playback.bEnded ? 0 : Icon.Playback.Frame;
            if (Frame >= 1 && Frame <= Icon.Animation.Frames.size())
            {
                Out.push_back({ .Sheet = EHudSheet::Icons, .Frame = Icon.Animation.Frames[Frame - 1], .X = Right + IconX, .Y = Top + IconY });
            }
        }

        DrawText(Out, EHudSheet::Panel, ScoreFont, int32_t(Right) + AmmoTextX, AmmoTextY, std::format("{:08}", int32_t(Values.Ammo)));
    }

    if (Mode >= SpeedMode)
    {
        // Speedometer and radar, bottom left.
        DrawSlot(Out, SpeedSlot, Left, Bottom, CurrentFrame);
        DrawSlot(Out, SpeedTopSlot, Left, Bottom, CurrentFrame);
        DrawSlot(Out, SpeedSideSlot, Left, Bottom, CurrentFrame);
        for (const FRadarDot& Dot : Radar)
        {
            Out.push_back({ .Sheet = EHudSheet::Colors, .Frame = { .BitmapIndex = uint32_t(Dot.Kind) + 1 }, .X = float(Dot.X), .Y = float(Dot.Y) });
        }

        // The speed is clamped to three digits before it is scaled by 100 and wrapped.
        constexpr float SpeedLimit = 1000.f;
        constexpr float ClampedSpeed = 999.f;
        constexpr float SpeedScale = 100.f;
        constexpr int32_t SpeedModulo = 1000;
        constexpr int32_t SpeedTextX = 0x69;
        constexpr int32_t SpeedTextY = -4;
        const float Speed = !(Values.Speed < SpeedLimit) ? ClampedSpeed : Values.Speed <= -SpeedLimit ? -ClampedSpeed : Values.Speed;
        DrawText(Out, EHudSheet::Panel, SpeedFont, SpeedTextX, int32_t(Bottom) + SpeedTextY, std::format("{:03}", int32_t(double(Speed) * SpeedScale) % SpeedModulo));
    }

    if (Mode >= ScreenMode)
    {
        // Screen, bottom right: position, kills, enemies left and the level time, unless a surprise is in effect.
        Gaf::FAnimationFrame Screen = Slots[ScreenSlot].Animation.Frames.empty() ? Gaf::FAnimationFrame{} : Slots[ScreenSlot].Animation.Frames.front();
        Screen.DrawFlags = Details.bTranslucentPanel ? ScreenDrawMode : OpaqueScreenDrawMode;
        Out.push_back({ .Sheet = Slots[ScreenSlot].Sheet, .Frame = Screen, .X = Right, .Y = Bottom, .Alpha = Details.bTranslucentPanel ? ScreenAlpha : 1.f });
        if (!Values.bSurprise)
        {
            constexpr int32_t ScreenTextX = -0x6b;
            constexpr int32_t ScreenTextSecondX = -0x48;
            constexpr int32_t PositionY = -0x46;
            constexpr int32_t KilledY = -0x3c;
            constexpr int32_t EnemyY = -0x32;
            constexpr int32_t TimeY = -0x28;
            constexpr uint32_t MillisecondsPerHour = 3600000;
            const int32_t ScreenX = int32_t(Right);
            const int32_t ScreenY = int32_t(Bottom);
            DrawText(Out, EHudSheet::Panel, ScreenFont, ScreenX + ScreenTextX, ScreenY + PositionY, std::format("{:04}", int32_t(Values.Position.X)));
            DrawText(Out, EHudSheet::Panel, ScreenFont, ScreenX + ScreenTextSecondX, ScreenY + PositionY, std::format("{:04}", int32_t(Values.Position.Y)));
            DrawText(Out, EHudSheet::Panel, ScreenFont, ScreenX + ScreenTextX, ScreenY + KilledY, std::format("KILLED {}", Values.Kills));
            DrawText(Out, EHudSheet::Panel, ScreenFont, ScreenX + ScreenTextX, ScreenY + EnemyY, std::format("ENEMY {}", Values.Enemies));
            const uint32_t Hours = Values.TimeMilliseconds / MillisecondsPerHour;
            const uint32_t Minutes = (Values.TimeMilliseconds - Hours * MillisecondsPerHour) / MillisecondsPerMinute;
            const uint32_t Seconds = (Values.TimeMilliseconds - Hours * MillisecondsPerHour - Minutes * MillisecondsPerMinute) / MillisecondsPerSecond;
            DrawText(Out, EHudSheet::Panel, ScreenFont, ScreenX + ScreenTextX, ScreenY + TimeY, std::format("{:02}:{:02}:{:02}", Hours, Minutes, Seconds));
        }

        DrawSlot(Out, InfosSlot, Right, Bottom, FirstFrame);
        if (Values.bLocked)
        {
            DrawSlot(Out, InfosSlot, Right, Bottom, CurrentFrame);
        }

        DrawSlot(Out, InfosBaseSlot, Right, Bottom, FirstFrame);
    }

    // Mode 1 prints one line in panel font 3 at 70, 20 instead (session +0x20a4 and +0x20a8); the percentages divide whole numbers.
    if (Mode == TextMode)
    {
        constexpr int32_t TextX = 70;
        constexpr int32_t TextY = 20;
        constexpr int32_t Percent = 100;
        const auto Ratio = [](const float Value, const float Max) { return int32_t(Max) == 0 ? 0 : int32_t(Value) * Percent / int32_t(Max); };
        DrawText(Out, EHudSheet::Panel, ScreenFont, TextX, TextY,
            std::format("SHIELD : {} / 100      ENERGY : {} / 100      AMMO : {}      CASH : {} $", Ratio(Values.Shield, Values.ShieldMax),
                Ratio(Values.Energy, Values.EnergyMax), int32_t(Values.Ammo), int32_t(Values.Money)));
    }

    // In any mode, a surprise in effect shows the minutes and seconds each started surprise has left at 100, 30 from the bottom right.
    if (Values.bSurprise)
    {
        constexpr int32_t TimerX = -100;
        constexpr int32_t TimerY = -0x1e;
        constexpr int32_t SecondsPerMinute = 60;
        for (const int32_t Time : Values.SurpriseTimes)
        {
            const int32_t Minutes = Time / int32_t(MillisecondsPerMinute);
            DrawText(Out, EHudSheet::Panel, ScreenFont, ViewWidth + TimerX, ViewHeight + TimerY,
                std::format("{}:{}", Minutes, Time / int32_t(MillisecondsPerSecond) - Minutes * SecondsPerMinute));
        }
    }

    return Out;
}

std::vector<FRadarDot> FindRadarDots(const FLevel& Level, const std::span<const FObjectBody> Bodies, const FCollisionPoint Player, const size_t PlayerIndex,
    const int ViewHeight)
{
    // Kinds 0x10 to 0x15 are left out, except lifts and items that carry a key.
    constexpr std::array<std::string_view, 4> HiddenKinds = { "ammo", "fx", "gate", "thing" };
    constexpr float RangeSquared = 921600.f;
    constexpr double DotScale = 0.03125;
    constexpr int RadarX = 0x48;
    constexpr int RadarY = -0x3b;
    constexpr int CellRangeX = 0xf;
    constexpr int CellRangeY = 0x1e;
    std::vector<const FObjectBody*> BodyOf(Level.Objects.size(), nullptr);
    for (const FObjectBody& Body : Bodies)
    {
        BodyOf[Body.ObjectIndex] = &Body;
    }

    std::vector<FRadarDot> Dots;
    const int PlayerCellX = int(Player.X) / int(BlockWidth);
    const int PlayerCellY = int(Player.Y) / int(BlockHeight);
    for (size_t Index = 0; Index < Level.Objects.size(); ++Index)
    {
        const FLevelObject& Object = Level.Objects[Index];
        const FObjectBody* const Body = BodyOf[Index];
        const FCollisionPoint Position = Index == PlayerIndex ? Player : Body ? Body->State.Position : FCollisionPoint{ .X = Object.X, .Y = Object.Y };
        const bool bItem = Object.SubType == "item";
        if (Object.bRemoved || std::ranges::contains(HiddenKinds, Object.SubType) || (Body && Body->bPassable) ||
            std::abs(int(Position.X) / int(BlockWidth) - PlayerCellX) > CellRangeX || std::abs(int(Position.Y) / int(BlockHeight) - PlayerCellY) > CellRangeY)
        {
            continue;
        }

        const float DeltaX = Position.X - Player.X;
        const float DeltaY = Position.Y - Player.Y;
        if (!(DeltaX * DeltaX + DeltaY * DeltaY < RangeSquared))
        {
            continue;
        }

        const bool bKey = bItem && std::ranges::any_of(Object.Properties.Children, [](const FLevelPropertyBlock& Child) { return Child.Kind == "TYPE" && Child.Name == "Key"; });
        if (bItem && !bKey)
        {
            continue;
        }

        Dots.push_back({
            .X = int(double(DeltaX) * DotScale) + RadarX,
            .Y = ViewHeight + RadarY + int(double(DeltaY) * DotScale),
            .Kind = Index == PlayerIndex ? ERadarDot::Player : Object.SubType == "lift" ? ERadarDot::Lift : bKey ? ERadarDot::Key : ERadarDot::Other,
        });
    }

    return Dots;
}
