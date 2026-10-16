#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Sound
{
struct FSample;
}

// Original pg-engine GAF container (pgga9902.cpp / pggob.cpp). Format notes live in Docs/DataFormats.md.
namespace Gaf
{
// Decodes the pg LZSS variant (LzssDecode at 0049ace0). Returns nullopt unless exactly OutputSize bytes are produced.
std::optional<std::vector<uint8_t>> LzssDecode(std::span<const uint8_t> Input, size_t OutputSize);

struct FFrame
{
    uint32_t Width = 0;
    uint32_t Height = 0;
    // Offset of the trimmed bitmap inside the untrimmed sprite cell (slot 0 metadata).
    int32_t TrimX = 0;
    int32_t TrimY = 0;
    uint16_t ColorKey = 0;
    // RGB555, tightly packed rows; ColorKey marks transparent pixels.
    std::vector<uint16_t> Pixels;
};

// Decodes every sprite frame of the GOB_ entry in a .GAF file.
std::optional<std::vector<FFrame>> LoadFrames(std::span<const uint8_t> File);

struct FAnimationFrame
{
    // Original GOB indices are 1-based; zero is an invisible frame.
    uint32_t BitmapIndex = 0;
    int32_t OffsetX = 0;
    int32_t OffsetY = 0;
    uint32_t DrawFlags = 0;
    bool bMirrorX = false;
    bool bMirrorY = false;
    // UpdateAnimation at 0045e7e0 copies these to the animation instance; DrawAnimationFrame at 0045eab0 reads them as a transform.
    uint32_t EventFlags = 0;
    uint32_t EventValue = 0;
    // In animation ticks: VzRun fixes the tick at 14 ms through AnimationSetFixedPeriod (0045e4c0).
    int32_t Duration = 0;
};

inline constexpr uint32_t AnimationLoops = 0x2;
inline constexpr uint32_t AnimationPingPong = 0x20;
inline constexpr uint32_t AnimationReverse = 0x40;
// A frame event flag: the frame plays the GAF sample its event value names.
inline constexpr uint32_t FrameSoundEvent = 0x1;

struct FAnimation
{
    std::string Name;
    std::vector<FAnimationFrame> Frames;
    // 1-based frame a looping animation restarts from.
    uint32_t LoopStart = 0;
    // StartAnimation at 0045e770 copies these to the animation instance: 2 loops, 0x20 plays back and forth, 0x40 runs backwards.
    uint32_t Flags = 0;
    // The GAF's SMP_ samples, shared by its animations: a frame with event flag 1 plays the one its event value names, from 1.
    std::shared_ptr<const std::vector<Sound::FSample>> Samples;
};

// ANM_ visual frames and NAM_ names, in original animation-index order. Offsets already account for frame mirroring.
std::optional<std::vector<FAnimation>> LoadAnimations(std::span<const uint8_t> File, size_t BitmapCount);

constexpr uint32_t Tag(const char (&Text)[5])
{
    return uint32_t(uint8_t(Text[0])) | uint32_t(uint8_t(Text[1])) << 8 | uint32_t(uint8_t(Text[2])) << 16 | uint32_t(uint8_t(Text[3])) << 24;
}

inline constexpr size_t SlotCount = 4;
using FSlots = std::array<std::vector<uint8_t>, SlotCount>;

// Decoded slot streams of the first DATA entry with the given tag (GafParseSlots at 0047bf00). A missing entry yields empty slots.
std::optional<FSlots> ReadEntrySlots(std::span<const uint8_t> File, uint32_t EntryTag);
// The size of an entry's slot 0 records, as the HEAD table of contents gives it (GafParseSlot0 at 0047b830 steps by it); older files have shorter records.
std::optional<uint32_t> ReadEntryRecordSize(std::span<const uint8_t> File, uint32_t EntryTag);

// Parses back-to-back BMP_ chunks, as stored in GOB_ slot 1 and BLK_ slot streams.
std::optional<std::vector<FFrame>> ParseBitmaps(std::span<const uint8_t> Bitmaps);

// A HEAD table-of-contents entry. GafParseSlot0 (0047b830) steps through RecordCount records of RecordSize, the slot 2 counts size the entry's allocation,
// and GafReadData (00453c90) seeks the entry's DATA record at DataOffset.
struct FHeadDescriptor
{
    uint32_t RecordCount;
    uint32_t RecordSize;
    uint32_t Reserved0;
    uint32_t Slot2Count;
    uint32_t Slot2RecordSize;
    uint32_t Reserved1;
    uint32_t DataOffset;
    // A MAP_ entry's layer count; other entries' values have no known reader and are kept as written.
    uint32_t Slot3Count;
    uint32_t Slot3Size;
};
static_assert(sizeof(FHeadDescriptor) == 36);

// One entry of a GAF as --compress rewrites it: its descriptor and its decoded slots.
struct FContainerEntry
{
    uint32_t Tag = 0;
    FHeadDescriptor Head{};
    FSlots Slots;
};

// Every entry in HEAD order; nullopt when the file is malformed.
std::optional<std::vector<FContainerEntry>> ReadContainer(std::span<const uint8_t> File);
// A whole GAF of the entries, every slot LZSS coded; the record counts, slot 3 size and offsets follow the slots.
std::vector<uint8_t> WriteContainer(std::span<const FContainerEntry> Entries);
// A stream LzssDecode reads back.
std::vector<uint8_t> LzssEncode(std::span<const uint8_t> Input);
// The frames as a BMP_ stream over the original one: each chunk keeps its header and padding unless its size changed, and frames past its end copy the first chunk's header.
std::optional<std::vector<uint8_t>> WriteBitmaps(std::span<const uint8_t> Original, std::span<const FFrame> Frames);
// The frames as the GOB_ entry's bitmaps, a frame record and rectangle rewritten for every frame whose size or trim changed. The frame count is fixed.
bool WriteFrames(std::vector<FContainerEntry>& Entries, std::span<const FFrame> Frames);
// The animations as the ANM_ entry and the ANM_ names of the NAM_ entry, the inverse of LoadAnimations.
bool WriteAnimations(std::vector<FContainerEntry>& Entries, std::span<const FAnimation> Animations);
}
