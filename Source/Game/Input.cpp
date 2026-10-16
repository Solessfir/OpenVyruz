#include "Game/Input.h"

#include "Game/Language.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <ranges>
#include <utility>

namespace
{
struct FKeyCode
{
    uint32_t Code = 0;
    int GlfwKey = GLFW_KEY_UNKNOWN;
    std::string_view Name;
};

// The keys of the name table at 00517a94 (entry code - 1) that a PC keyboard has.
constexpr std::array KeyCodes{
    FKeyCode{ 0x01, GLFW_KEY_ESCAPE, "ESCAPE" },
    FKeyCode{ 0x02, GLFW_KEY_KP_0, "0" },
    FKeyCode{ 0x03, GLFW_KEY_KP_1, "1" },
    FKeyCode{ 0x04, GLFW_KEY_KP_2, "2" },
    FKeyCode{ 0x05, GLFW_KEY_KP_3, "3" },
    FKeyCode{ 0x06, GLFW_KEY_KP_4, "4" },
    FKeyCode{ 0x07, GLFW_KEY_KP_5, "5" },
    FKeyCode{ 0x08, GLFW_KEY_KP_6, "6" },
    FKeyCode{ 0x09, GLFW_KEY_KP_7, "7" },
    FKeyCode{ 0x0a, GLFW_KEY_KP_8, "8" },
    FKeyCode{ 0x0b, GLFW_KEY_KP_9, "9" },
    FKeyCode{ 0x0c, GLFW_KEY_NUM_LOCK, "NUM LOCK" },
    FKeyCode{ 0x0d, GLFW_KEY_SCROLL_LOCK, "SCROLL LOCK" },
    FKeyCode{ 0x0e, GLFW_KEY_BACKSPACE, "BACKSPACE" },
    FKeyCode{ 0x0f, GLFW_KEY_TAB, "TABULATION" },
    FKeyCode{ 0x10, GLFW_KEY_F1, "F1" },
    FKeyCode{ 0x11, GLFW_KEY_F2, "F2" },
    FKeyCode{ 0x12, GLFW_KEY_F3, "F3" },
    FKeyCode{ 0x13, GLFW_KEY_F4, "F4" },
    FKeyCode{ 0x14, GLFW_KEY_F5, "F5" },
    FKeyCode{ 0x15, GLFW_KEY_F6, "F6" },
    FKeyCode{ 0x16, GLFW_KEY_F7, "F7" },
    FKeyCode{ 0x17, GLFW_KEY_F8, "F8" },
    FKeyCode{ 0x18, GLFW_KEY_F9, "F9" },
    FKeyCode{ 0x19, GLFW_KEY_F10, "F10" },
    FKeyCode{ 0x1a, GLFW_KEY_F11, "F11" },
    FKeyCode{ 0x1b, GLFW_KEY_F12, "F12" },
    FKeyCode{ 0x1c, GLFW_KEY_LEFT_ALT, "LEFT ALT" },
    FKeyCode{ 0x1d, GLFW_KEY_RIGHT_ALT, "RIGHT ALT" },
    FKeyCode{ 0x1e, GLFW_KEY_LEFT_SHIFT, "LEFT SHIFT" },
    FKeyCode{ 0x1f, GLFW_KEY_RIGHT_SHIFT, "RIGHT SHIFT" },
    FKeyCode{ 0x20, GLFW_KEY_SPACE, "SPACE BAR" },
    FKeyCode{ 0x21, GLFW_KEY_ENTER, "RETURN" },
    FKeyCode{ 0x22, GLFW_KEY_LEFT_CONTROL, "LEFT CTRL" },
    FKeyCode{ 0x23, GLFW_KEY_RIGHT_CONTROL, "RIGHT CTRL" },
    FKeyCode{ 0x24, GLFW_KEY_SLASH, "SLASH /" },
    FKeyCode{ 0x25, GLFW_KEY_PERIOD, "POINT ." },
    FKeyCode{ 0x26, GLFW_KEY_CAPS_LOCK, "CAPS LOCK" },
    FKeyCode{ 0x27, GLFW_KEY_APOSTROPHE, "APOSTROPHE '" },
    FKeyCode{ 0x2a, GLFW_KEY_KP_MULTIPLY, "NUMPAD *" },
    FKeyCode{ 0x2b, GLFW_KEY_KP_ADD, "NUMPAD +" },
    FKeyCode{ 0x2c, GLFW_KEY_COMMA, "COMMA ," },
    FKeyCode{ 0x2d, GLFW_KEY_KP_SUBTRACT, "NUMPAD -" },
    FKeyCode{ 0x2e, GLFW_KEY_KP_DECIMAL, "NUMPAD ." },
    FKeyCode{ 0x2f, GLFW_KEY_KP_DIVIDE, "NUMPAD /" },
    FKeyCode{ 0x30, GLFW_KEY_0, "0" },
    FKeyCode{ 0x31, GLFW_KEY_1, "1" },
    FKeyCode{ 0x32, GLFW_KEY_2, "2" },
    FKeyCode{ 0x33, GLFW_KEY_3, "3" },
    FKeyCode{ 0x34, GLFW_KEY_4, "4" },
    FKeyCode{ 0x35, GLFW_KEY_5, "5" },
    FKeyCode{ 0x36, GLFW_KEY_6, "6" },
    FKeyCode{ 0x37, GLFW_KEY_7, "7" },
    FKeyCode{ 0x38, GLFW_KEY_8, "8" },
    FKeyCode{ 0x39, GLFW_KEY_9, "9" },
    FKeyCode{ 0x3b, GLFW_KEY_SEMICOLON, "SEMICOLON ;" },
    FKeyCode{ 0x3d, GLFW_KEY_EQUAL, "EQUAL =" },
    FKeyCode{ 0x40, GLFW_KEY_MINUS, "MINUS -" },
    FKeyCode{ 0x41, GLFW_KEY_A, "A" },
    FKeyCode{ 0x42, GLFW_KEY_B, "B" },
    FKeyCode{ 0x43, GLFW_KEY_C, "C" },
    FKeyCode{ 0x44, GLFW_KEY_D, "D" },
    FKeyCode{ 0x45, GLFW_KEY_E, "E" },
    FKeyCode{ 0x46, GLFW_KEY_F, "F" },
    FKeyCode{ 0x47, GLFW_KEY_G, "G" },
    FKeyCode{ 0x48, GLFW_KEY_H, "H" },
    FKeyCode{ 0x49, GLFW_KEY_I, "I" },
    FKeyCode{ 0x4a, GLFW_KEY_J, "J" },
    FKeyCode{ 0x4b, GLFW_KEY_K, "K" },
    FKeyCode{ 0x4c, GLFW_KEY_L, "L" },
    FKeyCode{ 0x4d, GLFW_KEY_M, "M" },
    FKeyCode{ 0x4e, GLFW_KEY_N, "N" },
    FKeyCode{ 0x4f, GLFW_KEY_O, "O" },
    FKeyCode{ 0x50, GLFW_KEY_P, "P" },
    FKeyCode{ 0x51, GLFW_KEY_Q, "Q" },
    FKeyCode{ 0x52, GLFW_KEY_R, "R" },
    FKeyCode{ 0x53, GLFW_KEY_S, "S" },
    FKeyCode{ 0x54, GLFW_KEY_T, "T" },
    FKeyCode{ 0x55, GLFW_KEY_U, "U" },
    FKeyCode{ 0x56, GLFW_KEY_V, "V" },
    FKeyCode{ 0x57, GLFW_KEY_W, "W" },
    FKeyCode{ 0x58, GLFW_KEY_X, "X" },
    FKeyCode{ 0x59, GLFW_KEY_Y, "Y" },
    FKeyCode{ 0x5a, GLFW_KEY_Z, "Z" },
    FKeyCode{ 0x5b, GLFW_KEY_LEFT_BRACKET, "LEFT BRACKET [" },
    FKeyCode{ 0x5c, GLFW_KEY_BACKSLASH, "BACKSLASH \\" },
    FKeyCode{ 0x5d, GLFW_KEY_RIGHT_BRACKET, "RIGHT BRACKET ]" },
    FKeyCode{ 0x60, GLFW_KEY_GRAVE_ACCENT, "ACCENT `" },
    FKeyCode{ 0x64, GLFW_KEY_F13, "F13" },
    FKeyCode{ 0x65, GLFW_KEY_F14, "F14" },
    FKeyCode{ 0x66, GLFW_KEY_F15, "F15" },
    FKeyCode{ 0x8d, GLFW_KEY_KP_EQUAL, "NUMPAD =" },
    FKeyCode{ 0x9c, GLFW_KEY_KP_ENTER, "NUMPAD ENTER" },
    FKeyCode{ 0xb7, GLFW_KEY_PRINT_SCREEN, "SYS REQUEST" },
    FKeyCode{ 0xc7, GLFW_KEY_HOME, "HOME" },
    FKeyCode{ 0xc8, GLFW_KEY_UP, "UP ARROW" },
    FKeyCode{ 0xc9, GLFW_KEY_PAGE_UP, "PAGE UP" },
    FKeyCode{ 0xcb, GLFW_KEY_LEFT, "LEFT ARROW" },
    FKeyCode{ 0xcd, GLFW_KEY_RIGHT, "RIGHT ARROW" },
    FKeyCode{ 0xcf, GLFW_KEY_END, "END" },
    FKeyCode{ 0xd0, GLFW_KEY_DOWN, "DOWN ARROW" },
    FKeyCode{ 0xd1, GLFW_KEY_PAGE_DOWN, "PAGE DOWN" },
    FKeyCode{ 0xd2, GLFW_KEY_INSERT, "INSERT" },
    FKeyCode{ 0xd3, GLFW_KEY_DELETE, "DELETE" },
    FKeyCode{ 0xdb, GLFW_KEY_LEFT_SUPER, "LEFT WINDOWS" },
    FKeyCode{ 0xdc, GLFW_KEY_RIGHT_SUPER, "RIGHT WINDOWS" },
    FKeyCode{ 0xdd, GLFW_KEY_MENU, "APPS MENU" },
};

// The mouse inputs of the table at 005178cc: three axes, then the buttons from 0x103.
constexpr std::array<std::string_view, 7> MouseNames{
    "MOUSE X AXIS",
    "MOUSE Y AXIS",
    "MOUSE Z AXIS",
    "MOUSE LEFT BUTTON",
    "MOUSE RIGHT BUTTON",
    "MOUSE CENTER BUTTON",
    "MOUSE EXTRA BUTTON",
};
constexpr uint32_t FirstMouseButton = 3;
constexpr std::array MouseButtons{ GLFW_MOUSE_BUTTON_LEFT, GLFW_MOUSE_BUTTON_RIGHT, GLFW_MOUSE_BUTTON_MIDDLE, GLFW_MOUSE_BUTTON_4 };
// The Z axis is the wheel turning either way, as the original reads it.
constexpr uint32_t MouseZAxisCode = MouseCodeBase + 2;

// The port's own inputs from 0x400, which the original reads as never down (PgInputState at 004695d0) and names with nothing (FUN_00469580), so a shared vzconf.txt stays harmless to it.
constexpr uint32_t PortCodeBase = 0x400;
constexpr uint32_t WheelUpCode = PortCodeBase;
constexpr uint32_t WheelDownCode = PortCodeBase + 1;
constexpr uint32_t FifthMouseButtonCode = PortCodeBase + 2;
constexpr std::array<std::string_view, 3> PortNames{ "MOUSE WHEEL UP", "MOUSE WHEEL DOWN", "MOUSE EXTRA BUTTON 2" };

// The notches each way that wait to be read, and the one the current poll reads, if any.
double WheelUpTurns = 0.;
double WheelDownTurns = 0.;
std::optional<uint32_t> WheelNotch;

constexpr FBinding Fixed(const uint32_t Code)
{
    return { .Code = FixedBinding | Code, .Modifier = FixedBinding };
}

constexpr uint32_t LeftShift = 0x1e;
constexpr uint32_t JoypadButton5 = 0x205;

// The bindings every device shares from the next weapon on (DAT_00509298 from index 8).
constexpr FBindingTable WithCommonKeys(FBindingTable Table)
{
    Table[size_t(EAction::NextWeapon)] = { .Code = 0x0e };
    Table[size_t(EAction::ToggleSpeed)] = { .Code = 0xd1 };
    Table[size_t(EAction::Map)] = { .Code = 0x4d };
    Table[size_t(EAction::Inventory)] = { .Code = 0x49 };
    Table[size_t(EAction::Help)] = Fixed(0x10);
    Table[size_t(EAction::Pause)] = { .Code = 0x50 };
    Table[size_t(EAction::CyclePanels)] = { .Code = 0x0f };
    Table[size_t(EAction::Menu)] = { .Code = 0x01 };
    constexpr uint32_t DigitOne = 0x31;
    for (size_t Slot = 0; Slot <= size_t(EAction::Weapon9) - size_t(EAction::Weapon1); ++Slot)
    {
        Table[size_t(EAction::Weapon1) + Slot] = Fixed(DigitOne + uint32_t(Slot));
    }

    Table[size_t(EAction::PanelHelp)] = Fixed(0x11);
    Table[0] = { .Code = FixedBinding, .Modifier = FixedBinding };
    return Table;
}

// DAT_00509298 and DAT_00509300.
constexpr FBindingTable DefaultKeyboard = []
{
    FBindingTable Table{};
    Table[size_t(EAction::Fire)] = { .Code = 0x20 };
    Table[size_t(EAction::Forward)] = { .Code = 0xc8 };
    Table[size_t(EAction::Backward)] = { .Code = 0xd0 };
    Table[size_t(EAction::Left)] = { .Code = 0xcb };
    Table[size_t(EAction::Right)] = { .Code = 0xcd };
    Table[size_t(EAction::StrafeLeft)] = { .Code = 0xcb, .Modifier = LeftShift };
    Table[size_t(EAction::StrafeRight)] = { .Code = 0xcd, .Modifier = LeftShift };
    return WithCommonKeys(Table);
}();

// DAT_00509368 and DAT_005093d0: the sight turns, so Left and Right have no binding.
constexpr FBindingTable DefaultMouse = []
{
    FBindingTable Table{};
    Table[size_t(EAction::Fire)] = { .Code = 0x103 };
    Table[size_t(EAction::Forward)] = { .Code = 0x104 };
    Table[size_t(EAction::Backward)] = { .Code = 0xd0 };
    Table[size_t(EAction::Left)] = { .Code = FixedBinding, .Modifier = FixedBinding };
    Table[size_t(EAction::Right)] = { .Code = FixedBinding, .Modifier = FixedBinding };
    Table[size_t(EAction::StrafeLeft)] = { .Code = 0xcb };
    Table[size_t(EAction::StrafeRight)] = { .Code = 0xcd };
    return WithCommonKeys(Table);
}();

// DAT_00509438 and DAT_005094a0: the stick's directions are fixed, its fifth button makes them strafe.
constexpr FBindingTable DefaultJoypad = []
{
    FBindingTable Table{};
    Table[size_t(EAction::Fire)] = { .Code = 0x204 };
    Table[size_t(EAction::Forward)] = { .Code = FixedBinding | 0x202, .Modifier = FixedBinding };
    Table[size_t(EAction::Backward)] = { .Code = FixedBinding | 0x203, .Modifier = FixedBinding };
    Table[size_t(EAction::Left)] = { .Code = FixedBinding | 0x200, .Modifier = FixedBinding };
    Table[size_t(EAction::Right)] = { .Code = FixedBinding | 0x201, .Modifier = FixedBinding };
    Table[size_t(EAction::StrafeLeft)] = { .Code = FixedBinding | 0x200, .Modifier = JoypadButton5 };
    Table[size_t(EAction::StrafeRight)] = { .Code = FixedBinding | 0x201, .Modifier = JoypadButton5 };
    Table = WithCommonKeys(Table);
    Table[size_t(EAction::NextWeapon)] = { .Code = 0x206 };
    Table[size_t(EAction::ToggleSpeed)] = { .Code = 0x207 };
    return Table;
}();

constexpr uint8_t KeyboardSetting = 1;
constexpr uint8_t MouseSetting = 2;
constexpr uint8_t JoypadSetting = 4;
constexpr uint8_t AllDevices = KeyboardSetting | MouseSetting | JoypadSetting;

// FUN_0041de20's list: without its third argument it starts with Help, Panel Help and the nine weapons, which the key settings leave out.
constexpr size_t HelpOnlyActions = 11;
constexpr std::array Settings{
    FActionSetting{ EAction::Help, "Help", AllDevices },
    FActionSetting{ EAction::PanelHelp, "Panel Help", AllDevices },
    FActionSetting{ EAction::Weapon1, "Weapon #1", KeyboardSetting },
    FActionSetting{ EAction(size_t(EAction::Weapon1) + 1), "Weapon #2", KeyboardSetting },
    FActionSetting{ EAction(size_t(EAction::Weapon1) + 2), "Weapon #3", KeyboardSetting },
    FActionSetting{ EAction(size_t(EAction::Weapon1) + 3), "Weapon #4", KeyboardSetting },
    FActionSetting{ EAction(size_t(EAction::Weapon1) + 4), "Weapon #5", KeyboardSetting },
    FActionSetting{ EAction(size_t(EAction::Weapon1) + 5), "Weapon #6", KeyboardSetting },
    FActionSetting{ EAction(size_t(EAction::Weapon1) + 6), "Weapon #7", KeyboardSetting },
    FActionSetting{ EAction(size_t(EAction::Weapon1) + 7), "Weapon #8", KeyboardSetting },
    FActionSetting{ EAction::Weapon9, "Weapon #9", KeyboardSetting },
    FActionSetting{ EAction::Fire, "Fire", AllDevices },
    FActionSetting{ EAction::NextWeapon, "Next Weapon", AllDevices },
    FActionSetting{ EAction::Inventory, "Inventory", AllDevices },
    FActionSetting{ EAction::Map, "Map", AllDevices },
    FActionSetting{ EAction::Menu, "Menu", AllDevices },
    FActionSetting{ EAction::Pause, "Pause", AllDevices },
    FActionSetting{ EAction::ToggleSpeed, "High/Low Speed", AllDevices },
    FActionSetting{ EAction::Forward, "Forward", KeyboardSetting | MouseSetting },
    FActionSetting{ EAction::Backward, "Backward", KeyboardSetting | MouseSetting },
    FActionSetting{ EAction::Left, "Left", KeyboardSetting },
    FActionSetting{ EAction::Right, "Right", KeyboardSetting },
    FActionSetting{ EAction::StrafeLeft, "Strafe Left", AllDevices },
    FActionSetting{ EAction::StrafeRight, "Strafe Right", AllDevices },
    FActionSetting{ EAction::CyclePanels, "Panel Config.", AllDevices },
};

constexpr std::array<std::string_view, ActionCount> ActionKeywords{
    "",
    "action-fire",
    "action-forward",
    "action-backward",
    "action-left",
    "action-right",
    "action-strafe-left",
    "action-strafe-right",
    "action-next-weapon",
    "action-toggle-hilo-speed",
    "action-map",
    "action-inventory",
    "action-help",
    "action-pause",
    "action-cycle-panels",
    "action-menu",
    "action-weapon-1",
    "action-weapon-2",
    "action-weapon-3",
    "action-weapon-4",
    "action-weapon-5",
    "action-weapon-6",
    "action-weapon-7",
    "action-weapon-8",
    "action-weapon-9",
    "action-pannel-help",
};
}

FKeyBindings FKeyBindings::Defaults()
{
    return { .Keyboard = DefaultKeyboard, .Mouse = DefaultMouse, .Joypad = DefaultJoypad };
}

const FBindingTable& FKeyBindings::DefaultTable(const EInputDevice Device)
{
    return Device == EInputDevice::Mouse ? DefaultMouse : Device == EInputDevice::Joypad ? DefaultJoypad : DefaultKeyboard;
}

FBindingTable& FKeyBindings::Table(const EInputDevice Device)
{
    return Device == EInputDevice::Mouse ? Mouse : Device == EInputDevice::Joypad ? Joypad : Keyboard;
}

const FBindingTable& FKeyBindings::Table(const EInputDevice Device) const
{
    return Device == EInputDevice::Mouse ? Mouse : Device == EInputDevice::Joypad ? Joypad : Keyboard;
}

void FKeyBindings::Bind(const EInputDevice Device, const EAction Action, const FBinding Binding)
{
    FBinding& Slot = Table(Device)[size_t(Action)];
    if ((Slot.Code & FixedBinding) == 0)
    {
        Slot = Binding;
    }
}

bool FKeyBindings::BindAlone(const EInputDevice Device, const EAction Action, const FBinding Binding)
{
    FBindingTable& Bindings = Table(Device);
    const auto Same = [&](const FBinding& Other)
    { return (Other.Code & BindingCodeMask) == (Binding.Code & BindingCodeMask) && (Other.Modifier & BindingCodeMask) == (Binding.Modifier & BindingCodeMask); };
    if ((Bindings[size_t(Action)].Code & FixedBinding) != 0 ||
        std::ranges::any_of(Bindings, [&](const FBinding& Other) { return (Other.Code & FixedBinding) != 0 && Same(Other); }))
    {
        return false;
    }

    for (FBinding& Other : Bindings)
    {
        Other = Same(Other) ? FBinding{} : Other;
    }

    Bindings[size_t(Action)] = Binding;
    return true;
}

std::span<const FActionSetting> ActionSettings()
{
    return std::span(Settings).subspan(HelpOnlyActions);
}

std::span<const FActionSetting> HelpActions()
{
    return Settings;
}

bool ListsSetting(const FActionSetting& Setting, const EInputDevice Device)
{
    const uint8_t Bit = Device == EInputDevice::Keyboard ? KeyboardSetting : Device == EInputDevice::Mouse ? MouseSetting : JoypadSetting;
    return (Setting.Devices & Bit) != 0;
}

std::string CodeName(const uint32_t Code)
{
    if (Code == 0)
    {
        return {};
    }

    if (Code < MouseCodeBase)
    {
        const auto Key = std::ranges::find(KeyCodes, Code, &FKeyCode::Code);
        return Key != KeyCodes.end() ? std::string(Localize(Key->Name)) : std::format("0x{:02X}", Code);
    }

    if (Code < JoypadCodeBase)
    {
        return Code - MouseCodeBase < MouseNames.size() ? std::string(Localize(MouseNames[Code - MouseCodeBase])) : std::string();
    }

    if (Code >= PortCodeBase)
    {
        return Code - PortCodeBase < PortNames.size() ? std::string(PortNames[Code - PortCodeBase]) : std::string();
    }

    // ponytail: the joypad's names come from DirectInput's device; the port has no joypad yet.
    return std::format("JOYPAD {}", Code - JoypadCodeBase);
}

std::string BindingName(const FBinding& Binding)
{
    const uint32_t Code = Binding.Code & BindingCodeMask;
    const uint32_t Modifier = Binding.Modifier & BindingCodeMask;
    return Modifier == 0 ? CodeName(Code) : std::format("{} + {}", CodeName(Modifier), CodeName(Code));
}

std::string_view ActionKeyword(const size_t Action)
{
    return Action < ActionKeywords.size() ? ActionKeywords[Action] : std::string_view();
}

std::string_view DeviceKeyword(const EInputDevice Device)
{
    return Device == EInputDevice::Mouse ? "Mouse-Settings" : Device == EInputDevice::Joypad ? "Joypad-Settings" : "Keyboard-Settings";
}

namespace
{
// The pad as the last two polls read it, with the taps and holds of Back and Start, and the right stick's flick.
struct FGamepad
{
    GLFWgamepadstate Now{};
    GLFWgamepadstate Before{};
    bool bConnected = false;
    bool bActivity = false;
    bool bLastUsed = false;
    uint32_t BackDownAt = 0;
    uint32_t StartDownAt = 0;
    bool bBackHeldOut = false;
    bool bStartHeldOut = false;
    bool bBackTap = false;
    bool bBackHold = false;
    bool bStartTap = false;
    bool bStartHold = false;
};

FGamepad Pad;

constexpr float StickDeadZone = 0.2f;
// A push on the right stick turns the ship slowly, so a resting thumb barely moves it.
constexpr float AimDeadZone = 0.25f;
constexpr float TriggerPressed = 0.5f;
constexpr float MenuStickOut = 0.6f;
constexpr uint32_t HoldMilliseconds = 500;

bool PadDown(const int Button)
{
    return Pad.Now.buttons[Button] == GLFW_PRESS;
}

bool PadPressed(const int Button)
{
    return Pad.Now.buttons[Button] == GLFW_PRESS && Pad.Before.buttons[Button] != GLFW_PRESS;
}

float StickLength(const float X, const float Y)
{
    return std::sqrt(X * X + Y * Y);
}
}

void StepGamepad(const uint32_t NowMilliseconds)
{
    Pad.Before = Pad.Now;
    Pad.bConnected = false;
    for (int Joystick = GLFW_JOYSTICK_1; Joystick <= GLFW_JOYSTICK_LAST && !Pad.bConnected; ++Joystick)
    {
        Pad.bConnected = glfwJoystickIsGamepad(Joystick) == GLFW_TRUE && glfwGetGamepadState(Joystick, &Pad.Now) == GLFW_TRUE;
    }

    if (!Pad.bConnected)
    {
        Pad.Now = {};
    }

    // A tap acts as the button comes up; a hold acts once, as it passes the hold time.
    const auto TapOrHold = [&](const int Button, uint32_t& DownAt, bool& bHeldOut, bool& bTap, bool& bHold)
    {
        bTap = false;
        bHold = false;
        const bool bWasDown = Pad.Before.buttons[Button] == GLFW_PRESS;
        if (PadPressed(Button))
        {
            DownAt = NowMilliseconds;
            bHeldOut = false;
        }

        if (PadDown(Button) && !bHeldOut && NowMilliseconds - DownAt >= HoldMilliseconds)
        {
            bHold = true;
            bHeldOut = true;
        }

        bTap = !PadDown(Button) && bWasDown && !bHeldOut;
    };
    TapOrHold(GLFW_GAMEPAD_BUTTON_BACK, Pad.BackDownAt, Pad.bBackHeldOut, Pad.bBackTap, Pad.bBackHold);
    TapOrHold(GLFW_GAMEPAD_BUTTON_START, Pad.StartDownAt, Pad.bStartHeldOut, Pad.bStartTap, Pad.bStartHold);

    const float RightX = Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
    const float RightY = Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
    const float Right = StickLength(RightX, RightY);
    bool bTouched = std::ranges::any_of(std::views::iota(0, GLFW_GAMEPAD_BUTTON_LAST + 1), [](const int Button) { return PadDown(Button); });
    bTouched = bTouched || StickLength(Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_X], Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]) >= StickDeadZone || Right >= StickDeadZone;
    bTouched = bTouched || Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] >= TriggerPressed || Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] >= TriggerPressed;
    Pad.bActivity = Pad.bActivity || bTouched;
    Pad.bLastUsed = Pad.bLastUsed || bTouched;
}

bool TakeGamepadActivity()
{
    return std::exchange(Pad.bActivity, false);
}

bool IsGamepadLastUsed()
{
    return Pad.bLastUsed;
}

void NoteMouseUsed()
{
    Pad.bLastUsed = false;
}

bool IsGamepadActionDown(const EAction Action)
{
    switch (Action)
    {
    case EAction::Fire:
        return PadDown(GLFW_GAMEPAD_BUTTON_A) || Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] >= TriggerPressed;
    case EAction::NextWeapon:
        return PadDown(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER);
    case EAction::Inventory:
        return PadDown(GLFW_GAMEPAD_BUTTON_Y);
    case EAction::Map:
        return PadDown(GLFW_GAMEPAD_BUTTON_B);
    case EAction::CyclePanels:
        return Pad.bBackTap;
    case EAction::PanelHelp:
        return Pad.bBackHold;
    case EAction::Menu:
        return Pad.bStartTap;
    case EAction::Help:
        return Pad.bStartHold;
    default:
        return false;
    }
}

bool IsGamepadPreviousWeaponDown()
{
    return PadDown(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER);
}

std::optional<std::pair<float, float>> GamepadMoveStick()
{
    const float X = Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_X];
    const float Y = Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_Y];
    return StickLength(X, Y) >= StickDeadZone ? std::optional(std::pair(X, Y)) : std::nullopt;
}

float GamepadAimPush()
{
    const float Length = StickLength(Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_X], Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y]);
    return std::clamp((Length - AimDeadZone) / (1.f - AimDeadZone), 0.f, 1.f);
}

float GamepadThrottle()
{
    const float Length = StickLength(Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_X], Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_Y]);
    return std::clamp((Length - StickDeadZone) / (1.f - StickDeadZone), 0.f, 1.f);
}

std::optional<std::pair<float, float>> GamepadAimStick()
{
    const float X = Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_X];
    const float Y = Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y];
    return StickLength(X, Y) >= AimDeadZone ? std::optional(std::pair(X, Y)) : std::nullopt;
}

FGamepadMenuInput GamepadMenuInput()
{
    const auto StickCrossed = [](const int Axis, const float Sign)
    { return Pad.Now.axes[Axis] * Sign >= MenuStickOut && Pad.Before.axes[Axis] * Sign < MenuStickOut; };
    return {
        .bUp = PadPressed(GLFW_GAMEPAD_BUTTON_DPAD_UP) || StickCrossed(GLFW_GAMEPAD_AXIS_LEFT_Y, -1.f),
        .bDown = PadPressed(GLFW_GAMEPAD_BUTTON_DPAD_DOWN) || StickCrossed(GLFW_GAMEPAD_AXIS_LEFT_Y, 1.f),
        .bLeftHeld = PadDown(GLFW_GAMEPAD_BUTTON_DPAD_LEFT) || Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_X] <= -MenuStickOut,
        .bRightHeld = PadDown(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT) || Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_X] >= MenuStickOut,
        .bAccept = PadPressed(GLFW_GAMEPAD_BUTTON_A),
        .bBack = PadPressed(GLFW_GAMEPAD_BUTTON_B),
    };
}

bool IsGamepadConfirmDown()
{
    return std::ranges::any_of(std::views::iota(0, GLFW_GAMEPAD_BUTTON_LAST + 1), [](const int Button) { return PadDown(Button); }) ||
           Pad.Now.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] >= TriggerPressed || Pad.Now.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] >= TriggerPressed;
}

void WatchMouseWheel(GLFWwindow* const Window)
{
    glfwSetScrollCallback(Window, [](GLFWwindow*, double, const double Turn) { (Turn > 0. ? WheelUpTurns : WheelDownTurns) += std::abs(Turn); });
}

void StepMouseWheel()
{
    // A notch reads as down for one poll and up for the next, so every notch is a fresh press; a touchpad's fractions add up to notches.
    constexpr double Notch = 1.;
    if (WheelNotch)
    {
        WheelNotch.reset();
        return;
    }

    if (WheelUpTurns >= Notch)
    {
        WheelUpTurns -= Notch;
        WheelNotch = WheelUpCode;
    }
    else if (WheelDownTurns >= Notch)
    {
        WheelDownTurns -= Notch;
        WheelNotch = WheelDownCode;
    }
}

bool IsCodeDown(GLFWwindow* const Window, const uint32_t Code)
{
    if (Code == MouseZAxisCode || Code == WheelUpCode || Code == WheelDownCode)
    {
        return WheelNotch && (Code == MouseZAxisCode || Code == *WheelNotch);
    }

    if (Code == FifthMouseButtonCode)
    {
        return glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_5) == GLFW_PRESS;
    }

    if (Code < MouseCodeBase)
    {
        const auto Key = std::ranges::find(KeyCodes, Code, &FKeyCode::Code);
        return Key != KeyCodes.end() && glfwGetKey(Window, Key->GlfwKey) == GLFW_PRESS;
    }

    if (Code < JoypadCodeBase && Code - MouseCodeBase >= FirstMouseButton && Code - MouseCodeBase - FirstMouseButton < MouseButtons.size())
    {
        return glfwGetMouseButton(Window, MouseButtons[Code - MouseCodeBase - FirstMouseButton]) == GLFW_PRESS;
    }

    return false;
}

bool IsActionDown(GLFWwindow* const Window, const FBindingTable& Bindings, const EAction Action)
{
    const FBinding& Binding = Bindings[size_t(Action)];
    const uint32_t Modifier = Binding.Modifier & BindingCodeMask;
    const uint32_t Code = Binding.Code & BindingCodeMask;
    return (Modifier == 0 || IsCodeDown(Window, Modifier)) && Code != 0 && IsCodeDown(Window, Code);
}

std::optional<uint32_t> FirstCodeDown(GLFWwindow* const Window)
{
    for (const FKeyCode& Key : KeyCodes)
    {
        if (glfwGetKey(Window, Key.GlfwKey) == GLFW_PRESS)
        {
            return Key.Code;
        }
    }

    for (size_t Button = 0; Button < MouseButtons.size(); ++Button)
    {
        if (glfwGetMouseButton(Window, MouseButtons[Button]) == GLFW_PRESS)
        {
            return MouseCodeBase + FirstMouseButton + uint32_t(Button);
        }
    }

    if (glfwGetMouseButton(Window, GLFW_MOUSE_BUTTON_5) == GLFW_PRESS)
    {
        return FifthMouseButtonCode;
    }

    return WheelNotch;
}
