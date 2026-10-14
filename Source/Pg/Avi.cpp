#include "Pg/Avi.h"

#include "Pg/Binary.h"

namespace Avi
{
namespace
{
struct FChunkHeader
{
    uint32_t Id;
    uint32_t Size;
};
static_assert(sizeof(FChunkHeader) == 8);

// avih, the MainAVIHeader.
struct FMainHeader
{
    uint32_t MicroSecondsPerFrame;
    uint32_t MaxBytesPerSecond;
    uint32_t PaddingGranularity;
    uint32_t Flags;
    uint32_t TotalFrames;
    uint32_t InitialFrames;
    uint32_t Streams;
    uint32_t SuggestedBufferSize;
    uint32_t Width;
    uint32_t Height;
    uint32_t Reserved[4];
};
static_assert(sizeof(FMainHeader) == 56);

// The start of strh, the AVISTREAMHEADER.
struct FStreamHeader
{
    uint32_t Type;
    uint32_t Handler;
};
static_assert(sizeof(FStreamHeader) == 8);

// The start of a video stream's strf, the BITMAPINFOHEADER.
struct FBitmapInfo
{
    uint32_t Size;
    int32_t Width;
    int32_t Height;
    uint16_t Planes;
    uint16_t BitCount;
    uint32_t Compression;
};
static_assert(sizeof(FBitmapInfo) == 20);

constexpr uint32_t Riff = FourCc("RIFF");
constexpr uint32_t List = FourCc("LIST");
constexpr uint32_t AviForm = FourCc("AVI ");
constexpr uint32_t HeaderList = FourCc("hdrl");
constexpr uint32_t StreamList = FourCc("strl");
constexpr uint32_t MovieList = FourCc("movi");
constexpr uint32_t RecordList = FourCc("rec ");
constexpr uint32_t MainHeader = FourCc("avih");
constexpr uint32_t StreamHeader = FourCc("strh");
constexpr uint32_t StreamFormat = FourCc("strf");
constexpr uint32_t VideoType = FourCc("vids");
constexpr uint32_t SoundType = FourCc("auds");
constexpr uint32_t ListTypeSize = sizeof(uint32_t);
// A chunk id is the stream's two-digit number followed by its kind.
constexpr uint32_t StreamDigitsMask = 0xffff;
constexpr uint32_t KindShift = 16;
constexpr uint32_t CompressedVideo = FourCc("\0\0dc") >> KindShift;
constexpr uint32_t UncompressedVideo = FourCc("\0\0db") >> KindShift;
constexpr uint32_t WaveBytes = FourCc("\0\0wb") >> KindShift;

constexpr uint32_t StreamDigits(const uint32_t Stream)
{
    return uint32_t('0' + Stream / 10) | uint32_t('0' + Stream % 10) << 8;
}

// Calls Visit(Id, Payload) for each chunk of Data; a LIST passes its type as Id and its contents as Payload, with bList set.
template <typename FVisit>
bool ForEachChunk(const std::span<const uint8_t> Data, FVisit&& Visit)
{
    for (size_t Offset = 0; Offset + sizeof(FChunkHeader) <= Data.size();)
    {
        const std::optional<FChunkHeader> Header = ReadStruct<FChunkHeader>(Data, Offset);
        const size_t Start = Offset + sizeof(FChunkHeader);
        if (!Header || Header->Size > Data.size() - Start)
        {
            return false;
        }

        const std::span<const uint8_t> Payload = Data.subspan(Start, Header->Size);
        if (Header->Id == List)
        {
            const std::optional<uint32_t> Type = ReadStruct<uint32_t>(Payload);
            if (!Type || !Visit(*Type, Payload.subspan(ListTypeSize), true))
            {
                return false;
            }
        }
        else if (!Visit(Header->Id, Payload, false))
        {
            return false;
        }

        // Chunks are padded to an even size.
        Offset = Start + Header->Size + (Header->Size & 1);
    }

    return true;
}
}

std::optional<FMovie> Read(const std::span<const uint8_t> File)
{
    const std::optional<FChunkHeader> Header = ReadStruct<FChunkHeader>(File);
    const std::optional<uint32_t> Form = ReadStruct<uint32_t>(File, sizeof(FChunkHeader));
    if (!Header || Header->Id != Riff || !Form || *Form != AviForm || Header->Size < ListTypeSize || Header->Size > File.size() - sizeof(FChunkHeader))
    {
        return std::nullopt;
    }

    FMovie Movie;
    std::optional<uint32_t> VideoStream;
    std::optional<uint32_t> SoundStream;
    uint32_t StreamCount = 0;
    const auto ReadStreamList = [&](const std::span<const uint8_t> Stream)
    {
        uint32_t Type = 0;
        return ForEachChunk(Stream, [&](const uint32_t Id, const std::span<const uint8_t> Payload, bool)
        {
            if (Id == StreamHeader)
            {
                const std::optional<FStreamHeader> Info = ReadStruct<FStreamHeader>(Payload);
                Type = Info ? Info->Type : 0;
                return Info.has_value();
            }

            if (Id == StreamFormat && Type == VideoType && !VideoStream)
            {
                const std::optional<FBitmapInfo> Info = ReadStruct<FBitmapInfo>(Payload);
                VideoStream = StreamCount;
                Movie.VideoCompression = Info ? Info->Compression : 0;
                return Info.has_value();
            }

            if (Id == StreamFormat && Type == SoundType && !SoundStream)
            {
                const std::optional<uint16_t> Format = ReadStruct<uint16_t>(Payload);
                SoundStream = StreamCount;
                Movie.SoundFormat = Format.value_or(0);
                return Format.has_value();
            }

            return true;
        });
    };

    std::optional<std::span<const uint8_t>> Chunks;
    const bool bRead = ForEachChunk(File.subspan(sizeof(FChunkHeader) + ListTypeSize, Header->Size - ListTypeSize),
        [&](const uint32_t Id, const std::span<const uint8_t> Payload, const bool bList)
        {
            if (bList && Id == HeaderList)
            {
                return ForEachChunk(Payload, [&](const uint32_t InnerId, const std::span<const uint8_t> Inner, const bool bInnerList)
                {
                    if (InnerId == MainHeader && !bInnerList)
                    {
                        const std::optional<FMainHeader> Main = ReadStruct<FMainHeader>(Inner);
                        Movie.FrameMicroseconds = Main ? Main->MicroSecondsPerFrame : 0;
                        Movie.Width = Main ? Main->Width : 0;
                        Movie.Height = Main ? Main->Height : 0;
                        return Main.has_value();
                    }

                    if (InnerId == StreamList && bInnerList)
                    {
                        const bool bStream = ReadStreamList(Inner);
                        ++StreamCount;
                        return bStream;
                    }

                    return true;
                });
            }

            if (bList && Id == MovieList)
            {
                Chunks = Payload;
            }

            return true;
        });
    if (!bRead || !Chunks || !VideoStream || Movie.FrameMicroseconds == 0)
    {
        return std::nullopt;
    }

    const auto AddChunk = [&](const uint32_t Id, const std::span<const uint8_t> Payload, const bool bList) -> bool
    {
        const uint32_t Kind = Id >> KindShift;
        if (bList)
        {
            return true;
        }

        if ((Id & StreamDigitsMask) == StreamDigits(*VideoStream) && (Kind == CompressedVideo || Kind == UncompressedVideo))
        {
            Movie.Frames.push_back(Payload);
        }
        else if (SoundStream && (Id & StreamDigitsMask) == StreamDigits(*SoundStream) && Kind == WaveBytes)
        {
            Movie.Sound.insert(Movie.Sound.end(), Payload.begin(), Payload.end());
        }

        return true;
    };
    // Interleaved files may group chunks in rec lists.
    const bool bChunks = ForEachChunk(*Chunks, [&](const uint32_t Id, const std::span<const uint8_t> Payload, const bool bList)
    {
        return bList && Id == RecordList ? ForEachChunk(Payload, AddChunk) : AddChunk(Id, Payload, bList);
    });
    if (!bChunks)
    {
        return std::nullopt;
    }

    return Movie;
}
}
