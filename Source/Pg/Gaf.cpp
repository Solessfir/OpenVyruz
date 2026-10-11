#include "Pg/Gaf.h"

#include <array>
#include <cstring>

namespace
{
constexpr uint32_t RecordHeaderSize = 16;
constexpr uint32_t VersionMagic = 0x19990208;
constexpr uint32_t SlotCount = 4;
constexpr uint32_t SlotDescriptorSize = 24;
constexpr uint32_t MethodStored = 0;
constexpr uint32_t MethodLzss = 0x100;
constexpr uint32_t FrameRecordSize = 44;
constexpr uint32_t BitmapHeaderSize = 0x44;
constexpr uint32_t MaxFrameDimension = 4096;

using Gaf::Tag;

template <typename T>
T ReadLE(const std::span<const uint8_t> Data, const size_t Offset)
{
    T Value;
    std::memcpy(&Value, Data.data() + Offset, sizeof(T));
    return Value;
}

struct FRecord
{
    uint32_t Tag = 0;
    uint32_t Extra = 0;
    std::span<const uint8_t> Payload;
};

std::optional<FRecord> ReadRecord(const std::span<const uint8_t> Data, const size_t Offset)
{
    if (Offset + RecordHeaderSize > Data.size() || ReadLE<uint32_t>(Data, Offset + 8) != VersionMagic)
    {
        return std::nullopt;
    }
    const uint32_t Size = ReadLE<uint32_t>(Data, Offset + 4);
    if (Size > Data.size() - Offset - RecordHeaderSize)
    {
        return std::nullopt;
    }
    return FRecord{ ReadLE<uint32_t>(Data, Offset), ReadLE<uint32_t>(Data, Offset + 12), Data.subspan(Offset + RecordHeaderSize, Size) };
}

// GAF_ -> HEAD (table of contents, skipped) -> DATA -> first child with the wanted tag.
// A well-formed file without that child yields an empty record (Tag 0), as in the 48-byte stubs BONUS.GAF and BULLET.GAF.
std::optional<FRecord> FindDataEntry(const std::span<const uint8_t> File, const uint32_t WantedTag)
{
    const std::optional<FRecord> Root = ReadRecord(File, 0);
    if (!Root || Root->Tag != Tag("GAF_"))
    {
        return std::nullopt;
    }
    const std::optional<FRecord> Head = ReadRecord(File, RecordHeaderSize);
    if (!Head || Head->Tag != Tag("HEAD"))
    {
        return std::nullopt;
    }
    const size_t DataOffset = RecordHeaderSize * 2 + Head->Payload.size();
    const std::optional<FRecord> Data = ReadRecord(File, DataOffset);
    if (!Data || Data->Tag != Tag("DATA"))
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
        Offset += RecordHeaderSize + Entry->Payload.size();
    }
    return FRecord{};
}

// Entry payload (GOB_, BLK_, MAP_): 4 slot descriptors, then each non-empty slot's stream on a 4-byte boundary (GafParseSlots at 0047bf00).
std::optional<Gaf::FSlots> ReadSlots(const std::span<const uint8_t> Entry)
{
    if (Entry.size() < SlotCount * SlotDescriptorSize)
    {
        return std::nullopt;
    }

    Gaf::FSlots Slots;
    size_t Cursor = SlotCount * SlotDescriptorSize;
    for (uint32_t Slot = 0; Slot < SlotCount; ++Slot)
    {
        const size_t Descriptor = Slot * SlotDescriptorSize;
        const uint32_t Method = ReadLE<uint32_t>(Entry, Descriptor + 0x00);
        const uint32_t StoredSize = ReadLE<uint32_t>(Entry, Descriptor + 0x0C);
        const uint32_t DecodedSize = ReadLE<uint32_t>(Entry, Descriptor + 0x10);
        if (DecodedSize == 0)
        {
            continue;
        }

        Cursor = (Cursor + 3) & ~size_t{ 3 };
        const size_t InputSize = Method == MethodStored ? DecodedSize : StoredSize;
        if (InputSize > Entry.size() || Cursor > Entry.size() - InputSize)
        {
            return std::nullopt;
        }
        const std::span<const uint8_t> Input = Entry.subspan(Cursor, InputSize);
        Cursor += InputSize;

        if (Method == MethodStored)
        {
            Slots[Slot].assign(Input.begin(), Input.end());
        }
        else if (Method == MethodLzss)
        {
            std::optional<std::vector<uint8_t>> Decoded = Gaf::LzssDecode(Input, DecodedSize);
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
    constexpr uint32_t RingStart = 0xFEE;

    // The original only clears the first 0xFEE bytes to spaces and leaves the tail from the previous call.
    std::array<uint8_t, RingSize> Ring{};
    std::memset(Ring.data(), ' ', RingStart);
    uint32_t RingPos = RingStart;

    std::vector<uint8_t> Output;
    Output.reserve(OutputSize);
    size_t In = 0;
    uint32_t Flags = 0;
    while (Output.size() < OutputSize)
    {
        Flags >>= 1;
        // The high byte marks how many flag bits are left in the current control byte.
        if ((Flags & 0x100) == 0)
        {
            if (In >= Input.size())
            {
                break;
            }
            Flags = Input[In++] | 0xFF00;
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
            uint32_t MatchPos = Low | (High & 0xF0) << 4;
            const uint32_t MatchLength = (High & 0x0F) + 3;
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
        if (Offset + BitmapHeaderSize > Bitmaps.size() || ReadLE<uint32_t>(Bitmaps, Offset) != Tag("BMP_"))
        {
            return std::nullopt;
        }
        FFrame Frame;
        Frame.Width = ReadLE<uint32_t>(Bitmaps, Offset + 0x20);
        Frame.Height = ReadLE<uint32_t>(Bitmaps, Offset + 0x24);
        Frame.ColorKey = ReadLE<uint16_t>(Bitmaps, Offset + 0x3C);
        // The size field is the pixel byte count padded up to a multiple of 4.
        const uint32_t PaddedBytes = ReadLE<uint32_t>(Bitmaps, Offset + 0x04);
        if (Frame.Width == 0 || Frame.Height == 0 || Frame.Width > MaxFrameDimension || Frame.Height > MaxFrameDimension ||
            PaddedBytes < Frame.Width * Frame.Height * 2 || PaddedBytes > Bitmaps.size() - Offset - BitmapHeaderSize)
        {
            return std::nullopt;
        }
        Frame.Pixels.resize(Frame.Width * Frame.Height);
        std::memcpy(Frame.Pixels.data(), Bitmaps.data() + Offset + BitmapHeaderSize, Frame.Pixels.size() * 2);
        Offset += BitmapHeaderSize + PaddedBytes;
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
    std::optional<std::vector<FFrame>> Frames = ParseBitmaps((*Slots)[1]);
    if (!Frames)
    {
        return std::nullopt;
    }
    const std::span<const uint8_t> Metadata = (*Slots)[0];
    for (size_t Index = 0; Index < Frames->size(); ++Index)
    {
        const size_t Record = Index * FrameRecordSize;
        if (Record + FrameRecordSize <= Metadata.size())
        {
            (*Frames)[Index].TrimX = ReadLE<int32_t>(Metadata, Record + 0x10);
            (*Frames)[Index].TrimY = ReadLE<int32_t>(Metadata, Record + 0x14);
        }
    }
    return Frames;
}
