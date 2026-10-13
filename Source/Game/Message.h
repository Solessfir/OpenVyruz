#pragma once

#include "Pg/Font.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The GAF whose fonts a message is printed in.
enum class EMessageGaf : uint8_t
{
    // Common\Data\Extra\v-bonus.gaf, session +0x20b0.
    Bonus,
    // Common\Data\Panel\panel.gaf, session +0x20a4.
    Panel,
};

// How a new message treats one already at the same whole-pixel position (the mode of MessageCreate at 004311a0).
enum class EMessagePlacement : uint8_t
{
    Always,
    Replace,
    KeepExisting,
};

// A floating text message (type 3 of g_Messages).
struct FMessage
{
    std::string Text;
    float X = 0.f;
    float Y = 0.f;
    // Pixels per 14 milliseconds.
    float VelocityX = 0.f;
    float VelocityY = 0.f;
    uint32_t LifeMilliseconds = 0;
    // World messages scroll with the map; the others stay on the screen.
    bool bWorld = false;
    // The text appears one character per 50 milliseconds.
    bool bTypewriter = false;
    EMessageGaf Gaf = EMessageGaf::Bonus;
    // 1-based font of the GAF.
    uint32_t Font = 0;
    Font::FColor Color;
    size_t Typed = 0;
    int32_t TypeTimer = 0;
};

// The message list: MessageAddText (004312e0) adds a text, MessagesUpdate (004315f0) updates and draws them all.
class FMessages
{
public:
    using FDraw = std::function<void(const FMessage& Message, int32_t X, int32_t Y, std::string_view Text)>;

    void Add(FMessage Message, EMessagePlacement Placement);
    // MessageExists (00431050) and MessageSetLife (004319f0) find messages by their whole-pixel position.
    bool Exists(int32_t X, int32_t Y) const;
    void SetLife(int32_t X, int32_t Y, uint32_t LifeMilliseconds);
    // Both passes move and type the messages by the step; the update pass also ages them and the draw pass prints them with the scroll origin.
    void Update(int32_t StepMilliseconds, int32_t OriginX, int32_t OriginY, const FDraw* Draw);

private:
    // Removed messages leave an empty slot, which the next message takes when it is the last one freed or visited (g_MessageFreeSlot).
    std::vector<std::optional<FMessage>> Slots;
    std::optional<size_t> FreeSlot;
};
