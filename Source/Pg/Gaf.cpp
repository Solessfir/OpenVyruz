#include "Pg/Gaf.h"

#include "Pg/Binary.h"
#include "Pg/Sound.h"

#include <array>
#include <cstring>
#include <limits>

namespace
{
constexpr uint32_t VersionMagic = 0x19990208;
constexpr uint32_t MethodStored = 0;
constexpr uint32_t MethodLzss = 0x100;
constexpr uint32_t SlotAlignment = 4;
constexpr uint32_t MaxFrameDimension = 4096;
constexpr uint32_t BytesPerPixel = 2;
constexpr size_t GobFrameSlot = 0;
constexpr size_t GobBitmapSlot = 1;
constexpr size_t AnimationHeaderSlot = 0;
constexpr size_t AnimationFrameSlot = 2;
constexpr size_t NameGroupSlot = 0;
constexpr size_t NameRecordSlot = 2;
constexpr size_t NameTextSlot = 3;
constexpr uint32_t MirrorXFlag = 0x100;
constexpr uint32_t MirrorYFlag = 0x200;

// Header of every GAF record (GAF_, HEAD, DATA and each DATA entry); Size excludes the header.
struct FRecordHeader
{
    uint32_t Tag;
    uint32_t Size;
    uint32_t Version;
    uint32_t Extra;
};
static_assert(sizeof(FRecordHeader) == 16);

// One of the four slot descriptors opening a DATA entry payload.
struct FSlotDescriptor
{
    uint32_t Method;
    uint32_t Reserved0[2];
    uint32_t StoredSize;
    uint32_t DecodedSize;
    uint32_t Reserved1;
};
static_assert(sizeof(FSlotDescriptor) == 24);

// BMP_ chunk header; the RGB555 pixels follow, rows tightly packed.
struct FBitmapHeader
{
    uint32_t Tag;
    // Pixel byte count padded up to a multiple of 4.
    uint32_t PaddedSize;
    uint32_t Signature;
    uint32_t HeaderExtSize;
    uint32_t SignatureRepeat;
    uint32_t Type;
    uint32_t Depth;
    uint32_t DepthRepeat;
    uint32_t Width;
    uint32_t Height;
    // Runtime DirectDraw stride, not the on-disk one.
    uint32_t Stride;
    uint32_t Reserved0[2];
    uint32_t MaxX;
    uint32_t MaxY;
    uint16_t ColorKey;
    uint16_t Padding;
    uint32_t Reserved1;
};
static_assert(sizeof(FBitmapHeader) == 0x44);

// GOB_ slot 0 record, one per frame.
struct FFrameRecord
{
    int32_t One;
    int32_t Width;
    int32_t Height;
    int32_t Reserved0;
    int32_t TrimX;
    int32_t TrimY;
    int32_t Reserved1;
    int32_t NegativeRight;
    int32_t NegativeBottom;
    int32_t Reserved2[2];
};
static_assert(sizeof(FFrameRecord) == 44);

struct FAnimationRecord
{
    uint32_t FrameCount;
    uint32_t Flags;
    uint32_t LoopStart;
    int32_t Reserved[5];
};
static_assert(sizeof(FAnimationRecord) == 32);

struct FAnimationFrameRecord
{
    uint32_t BitmapIndex;
    int32_t OffsetX;
    int32_t OffsetY;
    uint32_t DrawFlags;
    uint32_t EventFlags;
    uint32_t EventValue;
    int32_t Duration;
};
static_assert(sizeof(FAnimationFrameRecord) == 28);

struct FNameGroupRecord
{
    uint32_t Count;
    uint32_t Tag;
};
static_assert(sizeof(FNameGroupRecord) == 8);

struct FNameRecord
{
    uint32_t Size;
    uint32_t Index;
    uint32_t Reserved;
};
static_assert(sizeof(FNameRecord) == 12);

struct FRecord
{
    uint32_t Tag = 0;
    uint32_t Extra = 0;
    std::span<const uint8_t> Payload;
};

std::optional<FRecord> ReadRecord(const std::span<const uint8_t> Data, const size_t Offset)
{
    const std::optional<FRecordHeader> Header = ReadStruct<FRecordHeader>(Data, Offset);
    if (!Header || Header->Version != VersionMagic || Header->Size > Data.size() - Offset - sizeof(FRecordHeader))
    {
        return std::nullopt;
    }
    return FRecord{
        .Tag = Header->Tag,
        .Extra = Header->Extra,
        .Payload = Data.subspan(Offset + sizeof(FRecordHeader), Header->Size),
    };
}

// GAF_ -> HEAD (table of contents, skipped) -> DATA -> first child with the wanted tag.
// A well-formed file without that child yields an empty record (Tag 0), as in the 48-byte stubs BONUS.GAF and BULLET.GAF.
std::optional<FRecord> FindDataEntry(const std::span<const uint8_t> File, const uint32_t WantedTag)
{
    const std::optional<FRecord> Root = ReadRecord(File, 0);
    if (!Root || Root->Tag != Gaf::Tag("GAF_"))
    {
        return std::nullopt;
    }
    const std::optional<FRecord> Head = ReadRecord(File, sizeof(FRecordHeader));
    if (!Head || Head->Tag != Gaf::Tag("HEAD"))
    {
        return std::nullopt;
    }
    const size_t DataOffset = sizeof(FRecordHeader) * 2 + Head->Payload.size();
    const std::optional<FRecord> Data = ReadRecord(File, DataOffset);
    if (!Data || Data->Tag != Gaf::Tag("DATA"))
    {
        return std::nullopt;
    }

    size_t Offset = 0;
    for (uint32_t Index = 0; Index < Data->Extra; ++Index)
    {
        const std::optional<FRecord> Entry = ReadRecord(Data->Payload, Offset);
        if (!Entry)
        {
            return std::nullopt;
        }
        if (Entry->Tag == WantedTag)
        {
            return Entry;
        }
        Offset += sizeof(FRecordHeader) + Entry->Payload.size();
    }
    return FRecord{};
}

// Entry payload (GOB_, BLK_, MAP_): slot descriptors, then each non-empty slot's stream on a 4-byte boundary (GafParseSlots at 0047bf00).
std::optional<Gaf::FSlots> ReadSlots(const std::span<const uint8_t> Entry)
{
    const std::optional<std::array<FSlotDescriptor, Gaf::SlotCount>> Descriptors = ReadStruct<std::array<FSlotDescriptor, Gaf::SlotCount>>(Entry);
    if (!Descriptors)
    {
        return std::nullopt;
    }

    Gaf::FSlots Slots;
    size_t Cursor = sizeof(*Descriptors);
    for (size_t Slot = 0; Slot < Gaf::SlotCount; ++Slot)
    {
        const FSlotDescriptor& Descriptor = (*Descriptors)[Slot];
        if (Descriptor.DecodedSize == 0)
        {
            continue;
        }

        Cursor = (Cursor + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 };
        const size_t InputSize = Descriptor.Method == MethodStored ? Descriptor.DecodedSize : Descriptor.StoredSize;
        if (InputSize > Entry.size() || Cursor > Entry.size() - InputSize)
        {
            return std::nullopt;
        }
        const std::span<const uint8_t> Input = Entry.subspan(Cursor, InputSize);
        Cursor += InputSize;

        if (Descriptor.Method == MethodStored)
        {
            Slots[Slot].assign(Input.begin(), Input.end());
        }
        else if (Descriptor.Method == MethodLzss)
        {
            std::optional<std::vector<uint8_t>> Decoded = Gaf::LzssDecode(Input, Descriptor.DecodedSize);
            if (!Decoded)
            {
                return std::nullopt;
            }
            Slots[Slot] = std::move(*Decoded);
        }
        else
        {
            return std::nullopt;
        }
    }
    return Slots;
}
}

std::optional<std::vector<uint8_t>> Gaf::LzssDecode(const std::span<const uint8_t> Input, const size_t OutputSize)
{
    constexpr uint32_t RingSize = 4096;
    constexpr uint8_t RingFill = ' ';
    // A control byte is loaded with this marker above its 8 flag bits; once shifted out, the next control byte is due.
    constexpr uint32_t ControlMarker = 0xFF00;
    constexpr uint32_t ControlExhausted = 0x100;
    constexpr uint8_t MatchOffsetHighMask = 0xF0;
    constexpr uint8_t MatchLengthMask = 0x0F;
    constexpr uint32_t MinMatchLength = 3;
    // Okumura layout: writing starts one maximum match length before the end of the ring (0xFEE).
    constexpr uint32_t RingStart = RingSize - (MatchLengthMask + MinMatchLength);
    static_assert(RingStart == 0xFEE);

    // The original only fills the first RingStart bytes and leaves the tail from the previous call.
    std::array<uint8_t, RingSize> Ring{};
    std::memset(Ring.data(), RingFill, RingStart);
    uint32_t RingPos = RingStart;

    std::vector<uint8_t> Output;
    Output.reserve(OutputSize);
    size_t In = 0;
    uint32_t Flags = 0;
    while (Output.size() < OutputSize)
    {
        Flags >>= 1;
        if ((Flags & ControlExhausted) == 0)
        {
            if (In >= Input.size())
            {
                break;
            }
            Flags = Input[In++] | ControlMarker;
        }

        if (Flags & 1)
        {
            if (In >= Input.size())
            {
                break;
            }
            const uint8_t Byte = Input[In++];
            Output.push_back(Byte);
            Ring[RingPos] = Byte;
            RingPos = (RingPos + 1) & (RingSize - 1);
        }
        else
        {
            if (In + 1 >= Input.size())
            {
                break;
            }
            const uint8_t Low = Input[In++];
            const uint8_t High = Input[In++];
            uint32_t MatchPos = Low | (High & MatchOffsetHighMask) << 4;
            const uint32_t MatchLength = (High & MatchLengthMask) + MinMatchLength;
            for (uint32_t Index = 0; Index < MatchLength && Output.size() < OutputSize; ++Index)
            {
                const uint8_t Byte = Ring[MatchPos & (RingSize - 1)];
                ++MatchPos;
                Output.push_back(Byte);
                Ring[RingPos] = Byte;
                RingPos = (RingPos + 1) & (RingSize - 1);
            }
        }
    }

    if (Output.size() != OutputSize)
    {
        return std::nullopt;
    }
    return Output;
}

std::optional<uint32_t> Gaf::ReadEntryRecordSize(const std::span<const uint8_t> File, const uint32_t EntryTag)
{
    // Each HEAD entry is a record whose payload starts with slot 0's record count and record size.
    struct FHeadEntry
    {
        uint32_t RecordCount;
        uint32_t RecordSize;
    };
    const std::optional<FRecord> Head = ReadRecord(File, sizeof(FRecordHeader));
    if (!Head || Head->Tag != Gaf::Tag("HEAD"))
    {
        return std::nullopt;
    }

    for (size_t Offset = 0; Offset < Head->Payload.size();)
    {
        const std::optional<FRecord> Entry = ReadRecord(Head->Payload, Offset);
        if (!Entry)
        {
            return std::nullopt;
        }

        if (Entry->Tag == EntryTag)
        {
            const std::optional<FHeadEntry> Counts = ReadStruct<FHeadEntry>(Entry->Payload);
            return Counts ? std::optional(Counts->RecordSize) : std::nullopt;
        }

        Offset += sizeof(FRecordHeader) + Entry->Payload.size();
    }

    return std::nullopt;
}

std::optional<Gaf::FSlots> Gaf::ReadEntrySlots(const std::span<const uint8_t> File, const uint32_t EntryTag)
{
    const std::optional<FRecord> Entry = FindDataEntry(File, EntryTag);
    if (!Entry)
    {
        return std::nullopt;
    }
    if (Entry->Tag == 0)
    {
        return FSlots{};
    }
    return ReadSlots(Entry->Payload);
}

std::optional<std::vector<Gaf::FFrame>> Gaf::ParseBitmaps(const std::span<const uint8_t> Bitmaps)
{
    std::vector<FFrame> Frames;
    size_t Offset = 0;
    while (Offset < Bitmaps.size())
    {
        const std::optional<FBitmapHeader> Header = ReadStruct<FBitmapHeader>(Bitmaps, Offset);
        if (!Header || Header->Tag != Gaf::Tag("BMP_") || Header->Width == 0 || Header->Height == 0 || Header->Width > MaxFrameDimension ||
            Header->Height > MaxFrameDimension || Header->PaddedSize < Header->Width * Header->Height * BytesPerPixel ||
            Header->PaddedSize > Bitmaps.size() - Offset - sizeof(FBitmapHeader))
        {
            return std::nullopt;
        }
        FFrame Frame{
            .Width = Header->Width,
            .Height = Header->Height,
            .ColorKey = Header->ColorKey,
        };
        Frame.Pixels.resize(size_t{ Frame.Width } * Frame.Height);
        std::memcpy(Frame.Pixels.data(), Bitmaps.data() + Offset + sizeof(FBitmapHeader), Frame.Pixels.size() * BytesPerPixel);
        Offset += sizeof(FBitmapHeader) + Header->PaddedSize;
        Frames.push_back(std::move(Frame));
    }
    return Frames;
}

std::optional<std::vector<Gaf::FFrame>> Gaf::LoadFrames(const std::span<const uint8_t> File)
{
    const std::optional<FSlots> Slots = ReadEntrySlots(File, Tag("GOB_"));
    if (!Slots)
    {
        return std::nullopt;
    }
    std::optional<std::vector<FFrame>> Frames = ParseBitmaps((*Slots)[GobBitmapSlot]);
    if (!Frames)
    {
        return std::nullopt;
    }
    for (size_t Index = 0; Index < Frames->size(); ++Index)
    {
        if (const std::optional<FFrameRecord> Record = ReadStruct<FFrameRecord>((*Slots)[GobFrameSlot], Index * sizeof(FFrameRecord)))
        {
            (*Frames)[Index].TrimX = Record->TrimX;
            (*Frames)[Index].TrimY = Record->TrimY;
        }
    }
    return Frames;
}

std::optional<std::vector<Gaf::FAnimation>> Gaf::LoadAnimations(const std::span<const uint8_t> File, const size_t BitmapCount)
{
    const std::optional<FSlots> Slots = ReadEntrySlots(File, Tag("ANM_"));
    const std::optional<FSlots> Names = ReadEntrySlots(File, Tag("NAM_"));
    if (!Slots || !Names || (*Slots)[AnimationHeaderSlot].size() % sizeof(FAnimationRecord) != 0 ||
        (*Slots)[AnimationFrameSlot].size() % sizeof(FAnimationFrameRecord) != 0 ||
        (*Names)[NameGroupSlot].size() % sizeof(FNameGroupRecord) != 0 || (*Names)[NameRecordSlot].size() % sizeof(FNameRecord) != 0)
    {
        return std::nullopt;
    }

    const size_t AnimationCount = (*Slots)[AnimationHeaderSlot].size() / sizeof(FAnimationRecord);
    std::vector<FAnimation> Animations(AnimationCount);
    size_t FrameOffset = 0;
    for (size_t Index = 0; Index < AnimationCount; ++Index)
    {
        const FAnimationRecord Record = *ReadStruct<FAnimationRecord>((*Slots)[AnimationHeaderSlot], Index * sizeof(FAnimationRecord));
        if (Record.FrameCount > ((*Slots)[AnimationFrameSlot].size() - FrameOffset) / sizeof(FAnimationFrameRecord))
        {
            return std::nullopt;
        }

        FAnimation& Animation = Animations[Index];
        Animation.Frames.reserve(Record.FrameCount);
        Animation.LoopStart = Record.LoopStart;
        Animation.Flags = Record.Flags;
        for (uint32_t FrameIndex = 0; FrameIndex < Record.FrameCount; ++FrameIndex)
        {
            // UpdateAnimation at 0045e7e0 negates ANM offsets before DrawGobFrame applies the mirrored GOB crop.
            const FAnimationFrameRecord Frame = *ReadStruct<FAnimationFrameRecord>((*Slots)[AnimationFrameSlot], FrameOffset);
            const bool bMirrorX = (Frame.DrawFlags & MirrorXFlag) != 0;
            const bool bMirrorY = (Frame.DrawFlags & MirrorYFlag) != 0;
            if (Frame.BitmapIndex > BitmapCount || (bMirrorX && Frame.OffsetX == std::numeric_limits<int32_t>::min()) ||
                (bMirrorY && Frame.OffsetY == std::numeric_limits<int32_t>::min()))
            {
                return std::nullopt;
            }

            Animation.Frames.push_back(FAnimationFrame{
                .BitmapIndex = Frame.BitmapIndex,
                .OffsetX = bMirrorX ? -Frame.OffsetX : Frame.OffsetX,
                .OffsetY = bMirrorY ? -Frame.OffsetY : Frame.OffsetY,
                .DrawFlags = Frame.DrawFlags,
                .bMirrorX = bMirrorX,
                .bMirrorY = bMirrorY,
                .EventFlags = Frame.EventFlags,
                .EventValue = Frame.EventValue,
                .Duration = Frame.Duration,
            });
            FrameOffset += sizeof(FAnimationFrameRecord);
        }
    }

    if (FrameOffset != (*Slots)[AnimationFrameSlot].size())
    {
        return std::nullopt;
    }

    size_t NameOffset = 0;
    size_t TextOffset = 0;
    for (size_t GroupOffset = 0; GroupOffset < (*Names)[NameGroupSlot].size(); GroupOffset += sizeof(FNameGroupRecord))
    {
        const FNameGroupRecord Group = *ReadStruct<FNameGroupRecord>((*Names)[NameGroupSlot], GroupOffset);
        if (Group.Count > ((*Names)[NameRecordSlot].size() - NameOffset) / sizeof(FNameRecord))
        {
            return std::nullopt;
        }

        for (uint32_t NameIndex = 0; NameIndex < Group.Count; ++NameIndex)
        {
            const FNameRecord Name = *ReadStruct<FNameRecord>((*Names)[NameRecordSlot], NameOffset);
            if (Name.Size == 0 || Name.Size > (*Names)[NameTextSlot].size() - TextOffset)
            {
                return std::nullopt;
            }

            const char* Text = reinterpret_cast<const char*>((*Names)[NameTextSlot].data() + TextOffset);
            const char* End = static_cast<const char*>(std::memchr(Text, '\0', Name.Size));
            if (!End || (Group.Tag == Tag("ANM_") && (Name.Index == 0 || Name.Index > AnimationCount)))
            {
                return std::nullopt;
            }

            if (Group.Tag == Tag("ANM_"))
            {
                Animations[Name.Index - 1].Name.assign(Text, End);
            }

            NameOffset += sizeof(FNameRecord);
            TextOffset += Name.Size;
        }
    }

    if (NameOffset != (*Names)[NameRecordSlot].size() || TextOffset != (*Names)[NameTextSlot].size())
    {
        return std::nullopt;
    }

    std::optional<std::vector<Sound::FSample>> Samples = Sound::DecodeGafSamples(File);
    if (!Samples)
    {
        return std::nullopt;
    }

    if (!Samples->empty())
    {
        const auto Shared = std::make_shared<const std::vector<Sound::FSample>>(std::move(*Samples));
        for (FAnimation& Animation : Animations)
        {
            Animation.Samples = Shared;
        }
    }

    return Animations;
}
