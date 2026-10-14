#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

struct GLFWwindow;

// The action keys of vzed\input.cpp: each device binds every action to a key code and an optional modifier that must be held with it.

// The device the player plays with (session +0x165c).
enum class EInputDevice : uint8_t
{
    Keyboard = 1,
    Joypad = 2,
    Mouse = 3,
};

// The action ids, indexes of the binding tables and of the vzconf.txt names at PTR_DAT_0050d64c.
enum class EAction : uint8_t
{
    Fire = 1,
    Forward,
    Backward,
    Left,
    Right,
    StrafeLeft,
    StrafeRight,
    NextWeapon,
    ToggleSpeed,
    Map,
    Inventory,
    Help,
    Pause,
    CyclePanels,
    Menu,
    Weapon1,
    Weapon9 = Weapon1 + 8,
    PanelHelp,
};

inline constexpr size_t ActionCount = size_t(EAction::PanelHelp) + 1;

// A code is the original's key number: below 0x100 a key (PgPoll's codes, named by the table at 00517a94), 0x100 + n mouse input n, 0x200 and up the joypad.
// Bit 31 marks a fixed binding, which the key settings never change.
inline constexpr uint32_t FixedBinding = 0x80000000;
inline constexpr uint32_t BindingCodeMask = ~FixedBinding;
inline constexpr uint32_t MouseCodeBase = 0x100;
inline constexpr uint32_t JoypadCodeBase = 0x200;

struct FBinding
{
    uint32_t Code = 0;
    uint32_t Modifier = 0;

    bool operator==(const FBinding&) const = default;
};

using FBindingTable = std::array<FBinding, ActionCount>;

// The three devices' tables (DAT_0052346c and DAT_00523378 for the keyboard, DAT_00523000 and DAT_005233e0 for the mouse, DAT_00522c6c and DAT_005234d4 for the joypad).
struct FKeyBindings
{
    FBindingTable Keyboard;
    FBindingTable Mouse;
    FBindingTable Joypad;

    // InputSetDefaultKeys at 0041d420 and its mouse and joypad counterparts (0041d450, 0041d480).
    static FKeyBindings Defaults();
    static const FBindingTable& DefaultTable(EInputDevice Device);

    FBindingTable& Table(EInputDevice Device);
    const FBindingTable& Table(EInputDevice Device) const;
    // FUN_0041dc90: a fixed binding stays.
    void Bind(EInputDevice Device, EAction Action, FBinding Binding);
};

// An entry of the key settings (FUN_0041de20 with its third argument set): the action, its name, and the devices that list it (1 keyboard, 2 mouse, 4 joypad).
struct FActionSetting
{
    EAction Action = EAction::Fire;
    std::string_view Name;
    uint8_t Devices = 0;
};

std::span<const FActionSetting> ActionSettings();
// The help screen's list (FUN_0041de20 without its third argument): Help, Panel Help and the nine weapons, then the key settings' entries.
std::span<const FActionSetting> HelpActions();
// Whether a device lists an action setting.
bool ListsSetting(const FActionSetting& Setting, EInputDevice Device);

// FUN_00469580: a code's name in the language in use, empty for none.
std::string CodeName(uint32_t Code);
// A binding's code with its modifier first, as the key settings list it.
std::string BindingName(const FBinding& Binding);
// The vzconf.txt name of an action (action-fire and so on).
std::string_view ActionKeyword(size_t Action);
// The section header of a device's bindings in vzconf.txt (Keyboard-Settings, Mouse-Settings, Joypad-Settings).
std::string_view DeviceKeyword(EInputDevice Device);

// Whether a code is down now; joypad codes never are.
bool IsCodeDown(GLFWwindow* Window, uint32_t Code);
// InputActionDown at 0041d650: the action's modifier, if any, and its code are down.
bool IsActionDown(GLFWwindow* Window, const FKeyBindings& Bindings, EInputDevice Device, EAction Action);
// The code of the first key or mouse button down, for the key settings' wait.
std::optional<uint32_t> FirstCodeDown(GLFWwindow* Window);
