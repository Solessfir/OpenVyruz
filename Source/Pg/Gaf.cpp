#include "Pg/Gaf.h"

#include "Pg/Binary.h"
#include "Pg/Sound.h"

#include <algorithm>
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
constexpr size_t GobBoundsSlot = 2;
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

// Entry payload (GOB_, BLK_, MAP_): slot descriptors, then each non-empty slot's stream back to back (GafParseSlots at 0047bf00).
// GafReadSlotStream (0049ae30) reads a stored slot's exact size but a coded slot's size rounded up to 4 bytes.
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

        const size_t InputSize = Descriptor.Method == MethodStored ? Descriptor.DecodedSize : Descriptor.StoredSize;
        const size_t ReadSize = Descriptor.Method == MethodStored ? InputSize : (InputSize + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 };
        if (ReadSize > Entry.size() || Cursor > Entry.size() - ReadSize)
        {
            return std::nullopt;
        }
        const std::span<const uint8_t> Input = Entry.subspan(Cursor, InputSize);
        Cursor += ReadSize;

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
            const std::optional<FHeadDescriptor> Counts = ReadStruct<FHeadDescriptor>(Entry->Payload);
            return Counts ? std::optional(Counts->RecordSize) : std::nullopt;
        }

        Offset += sizeof(FRecordHeader) + Entry->Payload.size();
    }

    return std::nullopt;
}

std::vector<uint8_t> Gaf::LzssEncode(const std::span<const uint8_t> Input)
{
    constexpr size_t RingSize = 4096;
    constexpr size_t MinMatch = 3;
    constexpr size_t MaxMatch = 18;
    constexpr size_t RingStart = RingSize - MaxMatch;
    // Matches only reach back into bytes already written: the decoder's ring tail before them is left from an earlier call (LzssDecode at 0049ace0).
    constexpr size_t Window = RingSize - MaxMatch;
    constexpr size_t FlagsPerControl = 8;
    constexpr int HashBits = 15;
    constexpr size_t MaxProbes = 128;
    constexpr uint32_t HashMultiplier = 0x9E3779B1;
    constexpr uint8_t OffsetHighMask = 0xF0;
    constexpr int OffsetHighShift = 4;

    std::vector<int32_t> Heads(size_t{ 1 } << HashBits, -1);
    std::vector<int32_t> Previous(Input.size(), -1);
    const auto Hash = [&](const size_t Index)
    {
        const uint32_t Key = uint32_t(Input[Index]) | uint32_t(Input[Index + 1]) << 8 | uint32_t(Input[Index + 2]) << 16;
        return (Key * HashMultiplier) >> (32 - HashBits);
    };
    const auto Insert = [&](const size_t Index)
    {
        if (Index + MinMatch <= Input.size())
        {
            const uint32_t Bucket = Hash(Index);
            Previous[Index] = Heads[Bucket];
            Heads[Bucket] = int32_t(Index);
        }
    };

    std::vector<uint8_t> Output;
    Output.reserve(Input.size() / 2 + FlagsPerControl);
    size_t Control = 0;
    size_t Flag = FlagsPerControl;
    for (size_t Index = 0; Index < Input.size();)
    {
        if (Flag == FlagsPerControl)
        {
            Control = Output.size();
            Output.push_back(0);
            Flag = 0;
        }

        size_t BestLength = 0;
        size_t BestSource = 0;
        const size_t Longest = std::min(MaxMatch, Input.size() - Index);
        if (Longest >= MinMatch)
        {
            size_t Probes = MaxProbes;
            for (int32_t Candidate = Heads[Hash(Index)]; Candidate >= 0 && Index - size_t(Candidate) <= Window && Probes-- > 0; Candidate = Previous[size_t(Candidate)])
            {
                size_t Length = 0;
                while (Length < Longest && Input[size_t(Candidate) + Length] == Input[Index + Length])
                {
                    ++Length;
                }

                if (Length > BestLength)
                {
                    BestLength = Length;
                    BestSource = size_t(Candidate);
                    if (Length == Longest)
                    {
                        break;
                    }
                }
            }
        }

        if (BestLength >= MinMatch)
        {
            const size_t RingPosition = (RingStart + BestSource) & (RingSize - 1);
            Output.push_back(uint8_t(RingPosition));
            Output.push_back(uint8_t((RingPosition >> OffsetHighShift & OffsetHighMask) | (BestLength - MinMatch)));
            for (size_t Step = 0; Step < BestLength; ++Step)
            {
                Insert(Index + Step);
            }

            Index += BestLength;
        }
        else
        {
            Output[Control] |= uint8_t(1u << Flag);
            Output.push_back(Input[Index]);
            Insert(Index);
            ++Index;
        }

        ++Flag;
    }

    return Output;
}

std::optional<std::vector<Gaf::FContainerEntry>> Gaf::ReadContainer(const std::span<const uint8_t> File)
{
    const std::optional<FRecord> Root = ReadRecord(File, 0);
    const std::optional<FRecord> Head = ReadRecord(File, sizeof(FRecordHeader));
    if (!Root || Root->Tag != Tag("GAF_") || !Head || Head->Tag != Tag("HEAD"))
    {
        return std::nullopt;
    }

    std::vector<FContainerEntry> Entries;
    for (size_t Offset = 0; Offset < Head->Payload.size();)
    {
        const std::optional<FRecord> Item = ReadRecord(Head->Payload, Offset);
        const std::optional<FHeadDescriptor> Descriptor = Item ? ReadStruct<FHeadDescriptor>(Item->Payload) : std::nullopt;
        // GafReadData at 00453c90 seeks each entry at the offset its descriptor gives.
        const std::optional<FRecord> Data = Descriptor ? ReadRecord(File, Descriptor->DataOffset) : std::nullopt;
        std::optional<FSlots> Slots = Data && Data->Tag == Item->Tag ? ReadSlots(Data->Payload) : std::nullopt;
        if (!Slots)
        {
            return std::nullopt;
        }

        Entries.push_back({ .Tag = Item->Tag, .Head = *Descriptor, .Slots = std::move(*Slots) });
        Offset += sizeof(FRecordHeader) + Item->Payload.size();
    }

    return Entries;
}

std::vector<uint8_t> Gaf::WriteContainer(const std::span<const FContainerEntry> Entries)
{
    constexpr uint32_t ChildCount = 2;
    std::vector<std::array<std::vector<uint8_t>, SlotCount>> Coded(Entries.size());
    size_t DataSize = 0;
    for (size_t Index = 0; Index < Entries.size(); ++Index)
    {
        DataSize += sizeof(FRecordHeader) + sizeof(std::array<FSlotDescriptor, SlotCount>);
        for (size_t Slot = 0; Slot < SlotCount; ++Slot)
        {
            Coded[Index][Slot] = LzssEncode(Entries[Index].Slots[Slot]);
            DataSize += (Coded[Index][Slot].size() + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 };
        }
    }

    const size_t HeadSize = Entries.size() * (sizeof(FRecordHeader) + sizeof(FHeadDescriptor));
    const size_t DataStart = sizeof(FRecordHeader) * 2 + HeadSize;
    std::vector<uint8_t> Out;
    Out.reserve(DataStart + sizeof(FRecordHeader) + DataSize);
    const auto Append = [&](const auto& Value)
    {
        const auto* const Bytes = reinterpret_cast<const uint8_t*>(&Value);
        Out.insert(Out.end(), Bytes, Bytes + sizeof(Value));
    };
    Append(FRecordHeader{ .Tag = Tag("GAF_"), .Size = uint32_t(DataStart + DataSize), .Version = VersionMagic, .Extra = ChildCount });
    Append(FRecordHeader{ .Tag = Tag("HEAD"), .Size = uint32_t(HeadSize), .Version = VersionMagic, .Extra = uint32_t(Entries.size()) });
    size_t EntryOffset = DataStart + sizeof(FRecordHeader);
    for (size_t Index = 0; Index < Entries.size(); ++Index)
    {
        const FContainerEntry& Entry = Entries[Index];
        FHeadDescriptor Descriptor = Entry.Head;
        Descriptor.RecordCount = Descriptor.RecordSize == 0 ? Descriptor.RecordCount : uint32_t(Entry.Slots[0].size() / Descriptor.RecordSize);
        Descriptor.Slot2Count = Descriptor.Slot2RecordSize == 0 ? Descriptor.Slot2Count : uint32_t(Entry.Slots[2].size() / Descriptor.Slot2RecordSize);
        Descriptor.Slot3Size = uint32_t(Entry.Slots[3].size());
        Descriptor.DataOffset = uint32_t(EntryOffset);
        Append(FRecordHeader{ .Tag = Entry.Tag, .Size = sizeof(FHeadDescriptor), .Version = VersionMagic });
        Append(Descriptor);
        EntryOffset += sizeof(FRecordHeader) + sizeof(std::array<FSlotDescriptor, SlotCount>);
        for (const std::vector<uint8_t>& Stream : Coded[Index])
        {
            EntryOffset += (Stream.size() + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 };
        }
    }

    Append(FRecordHeader{ .Tag = Tag("DATA"), .Size = uint32_t(DataSize), .Version = VersionMagic, .Extra = uint32_t(Entries.size()) });
    for (size_t Index = 0; Index < Entries.size(); ++Index)
    {
        size_t EntrySize = sizeof(std::array<FSlotDescriptor, SlotCount>);
        for (const std::vector<uint8_t>& Stream : Coded[Index])
        {
            EntrySize += (Stream.size() + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 };
        }

        Append(FRecordHeader{ .Tag = Entries[Index].Tag, .Size = uint32_t(EntrySize), .Version = VersionMagic });
        for (size_t Slot = 0; Slot < SlotCount; ++Slot)
        {
            const bool bEmpty = Entries[Index].Slots[Slot].empty();
            Append(FSlotDescriptor{
                .Method = MethodLzss,
                .StoredSize = bEmpty ? 0 : uint32_t(Coded[Index][Slot].size()),
                .DecodedSize = uint32_t(Entries[Index].Slots[Slot].size()),
            });
        }

        for (const std::vector<uint8_t>& Stream : Coded[Index])
        {
            Out.insert(Out.end(), Stream.begin(), Stream.end());
            Out.resize((Out.size() + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 }, 0);
        }
    }

    return Out;
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

std::optional<std::vector<uint8_t>> Gaf::WriteBitmaps(const std::span<const uint8_t> Original, const std::span<const FFrame> Frames)
{
    std::vector<FBitmapHeader> Headers;
    std::vector<size_t> Offsets;
    for (size_t Offset = 0; Offset < Original.size();)
    {
        const std::optional<FBitmapHeader> Header = ReadStruct<FBitmapHeader>(Original, Offset);
        if (!Header || Header->Tag != Tag("BMP_") || Header->PaddedSize > Original.size() - Offset - sizeof(FBitmapHeader))
        {
            return std::nullopt;
        }

        Headers.push_back(*Header);
        Offsets.push_back(Offset);
        Offset += sizeof(FBitmapHeader) + Header->PaddedSize;
    }

    if (Headers.empty())
    {
        return std::nullopt;
    }

    std::vector<uint8_t> Out;
    for (size_t Index = 0; Index < Frames.size(); ++Index)
    {
        const FFrame& Frame = Frames[Index];
        FBitmapHeader Header = Index < Headers.size() ? Headers[Index] : Headers.front();
        const size_t PixelBytes = Frame.Pixels.size() * BytesPerPixel;
        if (Frame.Width == 0 || Frame.Height == 0 || Frame.Width > MaxFrameDimension || Frame.Height > MaxFrameDimension ||
            Frame.Pixels.size() != size_t{ Frame.Width } * Frame.Height)
        {
            return std::nullopt;
        }

        // A resized bitmap gets the size fields every file has: its last column and row, and rows of whole 8-byte groups.
        const bool bResized = Header.Width != Frame.Width || Header.Height != Frame.Height || Index >= Headers.size();
        if (bResized)
        {
            constexpr uint32_t StrideAlignment = 8;
            Header.Width = Frame.Width;
            Header.Height = Frame.Height;
            Header.PaddedSize = uint32_t((PixelBytes + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 });
            Header.MaxX = Frame.Width - 1;
            Header.MaxY = Frame.Height - 1;
            Header.Stride = (Frame.Width * BytesPerPixel + StrideAlignment - 1) & ~(StrideAlignment - 1);
        }

        Header.ColorKey = Frame.ColorKey;
        const auto* const HeaderBytes = reinterpret_cast<const uint8_t*>(&Header);
        Out.insert(Out.end(), HeaderBytes, HeaderBytes + sizeof(Header));
        const auto* const PixelData = reinterpret_cast<const uint8_t*>(Frame.Pixels.data());
        Out.insert(Out.end(), PixelData, PixelData + PixelBytes);
        // An existing chunk keeps its padding bytes, which the original files do not leave zero.
        if (!bResized)
        {
            const auto Padding = Original.begin() + ptrdiff_t(Offsets[Index] + sizeof(FBitmapHeader) + PixelBytes);
            Out.insert(Out.end(), Padding, Padding + ptrdiff_t(Header.PaddedSize - PixelBytes));
        }
        else
        {
            Out.resize(Out.size() + Header.PaddedSize - PixelBytes, 0);
        }
    }

    return Out;
}

bool Gaf::WriteFrames(std::vector<FContainerEntry>& Entries, const std::span<const FFrame> Frames)
{
    // GOB_ slot 2 holds each bitmap's rectangle, as 0, 0, width, height and a zero word.
    struct FBoundsRecord
    {
        int32_t Left;
        int32_t Top;
        int32_t Right;
        int32_t Bottom;
        int32_t Reserved;
    };
    static_assert(sizeof(FBoundsRecord) == 20);

    const auto Gob = std::ranges::find(Entries, Tag("GOB_"), &FContainerEntry::Tag);
    if (Gob == Entries.end() || Gob->Head.RecordSize < sizeof(FFrameRecord) || Gob->Slots[GobFrameSlot].size() != Frames.size() * Gob->Head.RecordSize)
    {
        return false;
    }

    const std::optional<std::vector<FFrame>> Old = ParseBitmaps(Gob->Slots[GobBitmapSlot]);
    std::optional<std::vector<uint8_t>> Stream = Old && Old->size() == Frames.size() ? WriteBitmaps(Gob->Slots[GobBitmapSlot], Frames) : std::nullopt;
    if (!Stream)
    {
        return false;
    }

    Gob->Slots[GobBitmapSlot] = std::move(*Stream);
    for (size_t Index = 0; Index < Frames.size(); ++Index)
    {
        const FFrame& Frame = Frames[Index];
        uint8_t* const Bytes = Gob->Slots[GobFrameSlot].data() + Index * Gob->Head.RecordSize;
        FFrameRecord Record{};
        std::memcpy(&Record, Bytes, sizeof(Record));
        if ((*Old)[Index].Width == Frame.Width && (*Old)[Index].Height == Frame.Height && Record.TrimX == Frame.TrimX && Record.TrimY == Frame.TrimY)
        {
            continue;
        }

        // A few original records disagree with their bitmaps; only the ones an edit touches are written over.
        Record.Width = int32_t(Frame.Width);
        Record.Height = int32_t(Frame.Height);
        Record.TrimX = Frame.TrimX;
        Record.TrimY = Frame.TrimY;
        Record.NegativeRight = -(Frame.TrimX + int32_t(Frame.Width));
        Record.NegativeBottom = -(Frame.TrimY + int32_t(Frame.Height));
        std::memcpy(Bytes, &Record, sizeof(Record));
        if (Gob->Head.Slot2RecordSize == sizeof(FBoundsRecord) && Gob->Slots[GobBoundsSlot].size() >= (Index + 1) * sizeof(FBoundsRecord))
        {
            const FBoundsRecord Bounds{ .Right = int32_t(Frame.Width), .Bottom = int32_t(Frame.Height) };
            std::memcpy(Gob->Slots[GobBoundsSlot].data() + Index * sizeof(FBoundsRecord), &Bounds, sizeof(Bounds));
        }
    }

    return true;
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

bool Gaf::WriteAnimations(std::vector<FContainerEntry>& Entries, const std::span<const FAnimation> Animations)
{
    const auto Anm = std::ranges::find(Entries, Tag("ANM_"), &FContainerEntry::Tag);
    const auto Nam = std::ranges::find(Entries, Tag("NAM_"), &FContainerEntry::Tag);
    const bool bNamed = std::ranges::any_of(Animations, [](const FAnimation& Animation) { return !Animation.Name.empty(); });
    if (Anm == Entries.end() || (Nam == Entries.end() && bNamed))
    {
        return Anm == Entries.end() && Animations.empty();
    }

    // Records keep the words nothing reads; new animations start from zero.
    const std::vector<uint8_t> OldRecords = Anm->Slots[AnimationHeaderSlot];
    std::vector<uint8_t> Records;
    std::vector<uint8_t> Frames;
    const auto Append = [](std::vector<uint8_t>& Out, const auto& Value)
    {
        const auto* const Bytes = reinterpret_cast<const uint8_t*>(&Value);
        Out.insert(Out.end(), Bytes, Bytes + sizeof(Value));
    };
    for (size_t Index = 0; Index < Animations.size(); ++Index)
    {
        const FAnimation& Animation = Animations[Index];
        FAnimationRecord Record = ReadStruct<FAnimationRecord>(OldRecords, Index * sizeof(FAnimationRecord)).value_or(FAnimationRecord{});
        Record.FrameCount = uint32_t(Animation.Frames.size());
        Record.Flags = Animation.Flags;
        Record.LoopStart = Animation.LoopStart;
        Append(Records, Record);
        for (const FAnimationFrame& Frame : Animation.Frames)
        {
            // LoadAnimations gives mirrored offsets as drawn; the file keeps them as UpdateAnimation negates them.
            const auto Stored = [](const int32_t Offset, const bool bMirror) { return bMirror ? int32_t(0u - uint32_t(Offset)) : Offset; };
            Append(Frames, FAnimationFrameRecord{
                .BitmapIndex = Frame.BitmapIndex,
                .OffsetX = Stored(Frame.OffsetX, Frame.bMirrorX),
                .OffsetY = Stored(Frame.OffsetY, Frame.bMirrorY),
                .DrawFlags = (Frame.DrawFlags & ~(MirrorXFlag | MirrorYFlag)) | (Frame.bMirrorX ? MirrorXFlag : 0) | (Frame.bMirrorY ? MirrorYFlag : 0),
                .EventFlags = Frame.EventFlags,
                .EventValue = Frame.EventValue,
                .Duration = Frame.Duration,
            });
        }
    }

    Anm->Slots[AnimationHeaderSlot] = std::move(Records);
    Anm->Slots[AnimationFrameSlot] = std::move(Frames);
    if (Nam == Entries.end())
    {
        return true;
    }

    // NAM_ keeps its other groups as they are and gets a new ANM_ group of the named animations.
    std::vector<uint8_t> Groups;
    std::vector<uint8_t> NameRecords;
    std::vector<uint8_t> Texts;
    const auto AppendAnimationNames = [&]
    {
        uint32_t Count = 0;
        for (size_t Index = 0; Index < Animations.size(); ++Index)
        {
            const std::string& Name = Animations[Index].Name;
            if (Name.empty())
            {
                continue;
            }

            const uint32_t Size = uint32_t((Name.size() + 1 + SlotAlignment - 1) & ~size_t{ SlotAlignment - 1 });
            Append(NameRecords, FNameRecord{ .Size = Size, .Index = uint32_t(Index + 1) });
            Texts.insert(Texts.end(), Name.begin(), Name.end());
            Texts.resize(Texts.size() + Size - Name.size(), 0);
            ++Count;
        }

        if (Count != 0)
        {
            Append(Groups, FNameGroupRecord{ .Count = Count, .Tag = Tag("ANM_") });
        }
    };

    bool bWroteAnimationNames = false;
    size_t NameOffset = 0;
    size_t TextOffset = 0;
    const FSlots& OldNames = Nam->Slots;
    for (size_t GroupOffset = 0; GroupOffset < OldNames[NameGroupSlot].size(); GroupOffset += sizeof(FNameGroupRecord))
    {
        const std::optional<FNameGroupRecord> Group = ReadStruct<FNameGroupRecord>(OldNames[NameGroupSlot], GroupOffset);
        if (!Group)
        {
            return false;
        }

        size_t GroupTextSize = 0;
        for (uint32_t Name = 0; Name < Group->Count; ++Name)
        {
            const std::optional<FNameRecord> Record = ReadStruct<FNameRecord>(OldNames[NameRecordSlot], NameOffset + Name * sizeof(FNameRecord));
            if (!Record)
            {
                return false;
            }

            GroupTextSize += Record->Size;
        }

        const size_t RecordBytes = Group->Count * sizeof(FNameRecord);
        if (OldNames[NameTextSlot].size() < TextOffset + GroupTextSize)
        {
            return false;
        }

        // Unchanged names keep the order the file had them in.
        const bool bAnimationGroup = Group->Tag == Tag("ANM_");
        std::vector<std::pair<uint32_t, std::string>> Before;
        size_t NameText = TextOffset;
        for (uint32_t Name = 0; bAnimationGroup && Name < Group->Count; ++Name)
        {
            const FNameRecord Record = *ReadStruct<FNameRecord>(OldNames[NameRecordSlot], NameOffset + Name * sizeof(FNameRecord));
            const std::string_view Text(reinterpret_cast<const char*>(OldNames[NameTextSlot].data() + NameText), Record.Size);
            Before.emplace_back(Record.Index, std::string(Text.substr(0, Text.find('\0'))));
            NameText += Record.Size;
        }

        std::ranges::sort(Before);
        std::vector<std::pair<uint32_t, std::string>> After;
        for (size_t Index = 0; bAnimationGroup && Index < Animations.size(); ++Index)
        {
            if (!Animations[Index].Name.empty())
            {
                After.emplace_back(uint32_t(Index + 1), Animations[Index].Name);
            }
        }

        bWroteAnimationNames = bWroteAnimationNames || bAnimationGroup;
        if (bAnimationGroup && Before != After)
        {
            AppendAnimationNames();
        }
        else
        {
            Append(Groups, *Group);
            NameRecords.insert(NameRecords.end(), OldNames[NameRecordSlot].begin() + ptrdiff_t(NameOffset), OldNames[NameRecordSlot].begin() + ptrdiff_t(NameOffset + RecordBytes));
            Texts.insert(Texts.end(), OldNames[NameTextSlot].begin() + ptrdiff_t(TextOffset), OldNames[NameTextSlot].begin() + ptrdiff_t(TextOffset + GroupTextSize));
        }

        NameOffset += RecordBytes;
        TextOffset += GroupTextSize;
    }

    if (!bWroteAnimationNames)
    {
        AppendAnimationNames();
    }

    Nam->Slots[NameGroupSlot] = std::move(Groups);
    Nam->Slots[NameRecordSlot] = std::move(NameRecords);
    Nam->Slots[NameTextSlot] = std::move(Texts);
    return true;
}
