#include "Game/Message.h"

#include "Game/Engine.h"

#include <algorithm>

namespace
{
constexpr int32_t TypeIntervalMilliseconds = 50;
// Session +0x2744 holds 14 / step, by which the velocities are divided.
constexpr float VelocityStepMilliseconds = 14.f;

bool IsAt(const FMessage& Message, const int32_t X, const int32_t Y)
{
    return TruncateLow(Message.X) == X && TruncateLow(Message.Y) == Y;
}
}

uint32_t FMessages::Add(FMessage Message, const EMessagePlacement Placement)
{
    // MessageAddText drops an empty text or a missing font; FUN_004310b0 a missing animation.
    if (Message.Animation ? Message.Animation->Frames.empty() : Message.Text.empty() || Message.Font == 0)
    {
        return 0;
    }

    if (bForwarding)
    {
        Forwarded.emplace_back(std::move(Message), Placement);
        return 0;
    }

    const int32_t X = TruncateLow(Message.X);
    const int32_t Y = TruncateLow(Message.Y);
    if (Placement == EMessagePlacement::KeepExisting && Exists(X, Y))
    {
        return 0;
    }

    if (Placement == EMessagePlacement::Replace)
    {
        for (size_t Index = Slots.size(); Index-- > 0;)
        {
            if (Slots[Index] && IsAt(*Slots[Index], X, Y))
            {
                Slots[Index].reset();
                FreeSlot = Index;
            }
        }
    }

    Message.Typed = 0;
    Message.TypeTimer = TypeIntervalMilliseconds;
    if (Message.Animation)
    {
        Message.Playback = StartAnimation(*Message.Animation);
    }

    Message.Id = NextId++;
    const uint32_t Id = Message.Id;
    if (FreeSlot)
    {
        Slots[*FreeSlot] = std::move(Message);
        FreeSlot.reset();
        return Id;
    }

    Slots.push_back(std::move(Message));
    return Id;
}

void FMessages::Remove(const uint32_t Id)
{
    for (size_t Index = Slots.size(); Index-- > 0;)
    {
        if (Slots[Index] && Slots[Index]->Id == Id)
        {
            Slots[Index].reset();
            FreeSlot = Index;
            return;
        }
    }
}

void FMessages::RemoveAt(const int32_t X, const int32_t Y)
{
    for (size_t Index = Slots.size(); Index-- > 0;)
    {
        if (Slots[Index] && IsAt(*Slots[Index], X, Y))
        {
            Slots[Index].reset();
            FreeSlot = Index;
            return;
        }
    }
}

bool FMessages::Exists(const int32_t X, const int32_t Y) const
{
    return std::ranges::any_of(Slots, [&](const std::optional<FMessage>& Slot) { return Slot && IsAt(*Slot, X, Y); });
}

void FMessages::SetLife(const int32_t X, const int32_t Y, const uint32_t LifeMilliseconds)
{
    for (std::optional<FMessage>& Slot : Slots)
    {
        if (Slot && IsAt(*Slot, X, Y))
        {
            Slot.value().LifeMilliseconds = LifeMilliseconds;
        }
    }
}

void FMessages::Update(const int32_t StepMilliseconds, const int32_t OriginX, const int32_t OriginY, const FDraw* const Draw)
{
    // ponytail: only text messages are ported; the bitmap and animation types, blinking and the typing cursor have no caller yet.
    for (size_t Index = Slots.size(); Index-- > 0;)
    {
        if (!Slots[Index])
        {
            FreeSlot = Index;
            continue;
        }

        FMessage& Message = *Slots[Index];
        // Either pass removes an expired message; only the update pass ages it.
        if (uint32_t(StepMilliseconds) >= Message.LifeMilliseconds)
        {
            Slots[Index].reset();
            FreeSlot = Index;
            continue;
        }

        if (!Draw)
        {
            Message.LifeMilliseconds -= uint32_t(StepMilliseconds);
        }

        if (Message.BlinkPeriod != 0)
        {
            if (Message.BlinkTimer < StepMilliseconds)
            {
                if (!Message.bFade)
                {
                    Message.bHidden = !Message.bHidden;
                }
                else
                {
                    // FUN_00431970 bounces the alpha between 0 and 254 by 4 at first (DAT_0050d730).
                    constexpr int32_t FirstFadeStep = 4;
                    constexpr int32_t MaxFadeAlpha = 0xfe;
                    Message.FadeStep = Message.FadeStep == 0 ? FirstFadeStep : Message.FadeStep;
                    const int32_t Next = Message.FadeAlpha + Message.FadeStep;
                    if (Next < 0 || MaxFadeAlpha < Next)
                    {
                        Message.FadeStep = -Message.FadeStep;
                        Message.FadeAlpha += Message.FadeStep;
                    }
                    else
                    {
                        Message.FadeAlpha = Next;
                    }

                    Message.bFading = true;
                }

                Message.BlinkTimer = Message.BlinkPeriod;
            }
            else
            {
                Message.BlinkTimer -= StepMilliseconds;
            }

            if (Message.bHidden)
            {
                continue;
            }
        }

        // Before the first step the draw pass has no step to move by.
        if (StepMilliseconds != 0)
        {
            const float Scale = VelocityStepMilliseconds / float(StepMilliseconds);
            Message.X = Message.VelocityX / Scale + Message.X;
            Message.Y = Message.VelocityY / Scale + Message.Y;
        }

        if (Message.Animation)
        {
            UpdateAnimation(Message.Playback, *Message.Animation, StepMilliseconds);
            if (Draw)
            {
                (*Draw)(Message, TruncateLow(Message.X) - (Message.bWorld ? OriginX : 0), TruncateLow(Message.Y) - (Message.bWorld ? OriginY : 0), {});
            }

            continue;
        }

        Message.TypeTimer -= StepMilliseconds;
        const size_t Shown = Message.bTypewriter ? Message.Typed : Message.Text.size();
        if (Draw && Shown != 0)
        {
            const int32_t X = TruncateLow(Message.X) - (Message.bWorld ? OriginX : 0);
            const int32_t Y = TruncateLow(Message.Y) - (Message.bWorld ? OriginY : 0);
            (*Draw)(Message, X, Y, std::string_view(Message.Text).substr(0, Shown));
        }

        if (Message.bTypewriter && Message.Typed < Message.Text.size() && Message.TypeTimer < 1)
        {
            ++Message.Typed;
        }

        if (Message.TypeTimer < 1)
        {
            Message.TypeTimer += TypeIntervalMilliseconds;
        }
    }
}
