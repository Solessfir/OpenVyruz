#include "Pg/Gaf.h"

#include "Pg/Binary.h"

#include <array>
#include <cstring>

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
