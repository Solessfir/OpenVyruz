#pragma once

#include "Game/Collision.h"
#include "Game/Engine.h"
#include "Pg/Font.h"
#include "Pg/Gaf.h"

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

class FResourceArchive;
struct FLevel;

// The GAFs the panel draws from: Common\Data\Panel\panel.gaf, its v-panel.gaf companion, and solid colours for the radar dots.
enum class EHudSheet : uint8_t
{
    Panel,
    VPanel,
    Colors,
};

// Radar dot colours, as the indices FUN_00436080 plots.
enum class ERadarDot : uint8_t
{
    Other,
    Player,
    Key,
    Lift,
};

struct FHudSprite
{
    EHudSheet Sheet = EHudSheet::Panel;
    // Drawn like an object frame: the hotspot offset is added to X, Y.
    Gaf::FAnimationFrame Frame;
    float X = 0.f;
    float Y = 0.f;
    // A bar shows only this many columns of its bitmap from the left.
    uint32_t VisibleColumns = UINT32_MAX;
};

// What the panel shows of the player and the level.
struct FPanelValues
{
    float Shield = 0.f;
    float ShieldMax = 0.f;
    float Energy = 0.f;
    float EnergyMax = 0.f;
    float Money = 0.f;
    float Ammo = 0.f;
    float Speed = 0.f;
    FCollisionPoint Position;
    int Kills = 0;
    int Enemies = 0;
    uint32_t TimeMilliseconds = 0;
};

struct FRadarDot
{
    int X = 0;
    int Y = 0;
    ERadarDot Kind = ERadarDot::Other;
};

// The panel of EngineRunLevel (FUN_00435000) in its full mode 5: score, shield and energy bars, weapon block, speed and radar, and the
// position, kill, enemy and time screen. Corners are taken from the view size, which the original keeps at 640x480.
class FPanel
{
public:
    // FUN_00434d40 loads the panel GAFs and starts one animation per slot.
    static std::expected<FPanel, std::string> Load(FResourceArchive& Archive);

    // The update pass (param 0): the animated slots advance by the step.
    void Step(const FPanelValues& Values, int32_t StepMilliseconds);
    // The draw pass, back to front.
    std::vector<FHudSprite> Draw(const FPanelValues& Values, std::span<const FRadarDot> Radar, int ViewWidth, int ViewHeight) const;

    const std::vector<Gaf::FFrame>& GetBitmaps(EHudSheet Sheet) const { return Sheet == EHudSheet::VPanel ? VBitmaps : Bitmaps; }

    static constexpr size_t SlotCount = 26;

private:
    struct FSlot
    {
        EHudSheet Sheet = EHudSheet::Panel;
        Gaf::FAnimation Animation;
        FAnimationPlayback Playback;
    };

    void DrawSlot(std::vector<FHudSprite>& Out, size_t Slot, float X, float Y, uint32_t Frame, uint32_t VisibleColumns = UINT32_MAX) const;
    void DrawText(std::vector<FHudSprite>& Out, size_t Font, int32_t X, int32_t Y, const std::string& Text) const;

    std::vector<Gaf::FFrame> Bitmaps;
    std::vector<Gaf::FFrame> VBitmaps;
    std::vector<uint32_t> BitmapWidths;
    std::vector<Font::FFont> Fonts;
    std::array<FSlot, SlotCount> Slots;
};

// FUN_00436080: the objects within 960 pixels of the player as dots in the radar beside the speedometer.
std::vector<FRadarDot> FindRadarDots(const FLevel& Level, std::span<const FObjectBody> Bodies, FCollisionPoint Player, size_t PlayerIndex, int ViewHeight);
