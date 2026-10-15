#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

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

// A code is the original's key number: below 0x100 a key (PgPoll's codes, named by the table at 00517a94), 0x100 + n mouse input n, 0x200 to 0x3ff the joypad, and from 0x400 the port's own inputs.
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
    // The port's key settings: the binding moves to the action and leaves any other action that had it unbound.
    // A fixed binding, the action's own or another's with the same key, refuses it.
    bool BindAlone(EInputDevice Device, EAction Action, FBinding Binding);
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

// The wheel binds up and down apart, as port codes from 0x400 (with the fifth mouse button); the original's Z axis is either way.
// WatchMouseWheel counts its notches; StepMouseWheel, once per event poll, reads one notch as down for that poll.
void WatchMouseWheel(GLFWwindow* Window);
void StepMouseWheel();
// An Xbox-style pad through GLFW's gamepad mappings: the first joystick GLFW knows as a gamepad. The original's joypad device is not ported; the pad has a fixed modern layout instead.
// StepGamepad reads it once per event poll; Back and Start act on a tap when released and on a hold once held long enough.
void StepGamepad(uint32_t NowMilliseconds);
// Whether the pad was touched since the last call, which hands the steering to it.
bool TakeGamepadActivity();
// Whether the pad, rather than the mouse, was used last, so a menu starts on its first item; moving the mouse clears it.
bool IsGamepadLastUsed();
void NoteMouseUsed();
// The pad's part of an action: A or the right trigger fire, RB the next weapon, Y the inventory, B the map, Back taps the panels and holds their help,
// Start taps the game menu and holds the help. The movement actions are not buttons on the pad.
bool IsGamepadActionDown(EAction Action);
// LB, the previous weapon, which the original's actions lack.
bool IsGamepadPreviousWeaponDown();
// The D-pad's strafe: -1 left, 1 right, 0 neither.
int GamepadStrafe();
// The left stick past its dead zone, x right and y down, each from -1 to 1.
std::optional<std::pair<float, float>> GamepadMoveStick();
// How far the left stick is pushed, from 0 at its dead zone to 1 at full travel, which scales the ship's top speed.
float GamepadThrottle();
// The right stick's direction for the one poll in which it is flicked out past most of its travel, to switch the aim's target.
std::optional<std::pair<float, float>> GamepadAimFlick();
// The menus' reading of the pad: the D-pad or the left stick, up and down for the poll they are pressed in and left and right while held (the sliders), A to choose and B to go back.
struct FGamepadMenuInput
{
    bool bUp = false;
    bool bDown = false;
    bool bLeftHeld = false;
    bool bRightHeld = false;
    bool bAccept = false;
    bool bBack = false;
};
FGamepadMenuInput GamepadMenuInput();
// Whether any pad button or trigger is down, which the screens that wait for any key take as one.
bool IsGamepadConfirmDown();
// Whether a code is down now; joypad codes never are.
bool IsCodeDown(GLFWwindow* Window, uint32_t Code);
// InputActionDown at 0041d650: the action's modifier, if any, and its code are down.
bool IsActionDown(GLFWwindow* Window, const FBindingTable& Bindings, EAction Action);
// The code of the first key or mouse button down, for the key settings' wait.
std::optional<uint32_t> FirstCodeDown(GLFWwindow* Window);
