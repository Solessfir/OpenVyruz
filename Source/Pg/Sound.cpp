#include "Pg/Sound.h"

#include "Pg/Binary.h"
#include "Pg/Gaf.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// Decoding stays for the intro's MP3 sound alone.
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

namespace
{
constexpr uint32_t MonoChannels = 1;
constexpr uint32_t StereoChannels = 2;
constexpr uint32_t ByteBits = 8;
constexpr uint32_t UnsignedBits = 8;
constexpr uint32_t SignedBits = 16;

// Unsigned 8-bit or signed little-endian 16-bit PCM, interleaved, to floats.
std::optional<Sound::FSample> ConvertPcm(const std::span<const uint8_t> Pcm, const uint32_t Rate, const uint32_t Bits, const uint32_t ChannelCount,
    const size_t FrameCount)
{
    if ((Bits != UnsignedBits && Bits != SignedBits) || (ChannelCount != MonoChannels && ChannelCount != StereoChannels) || Rate == 0)
    {
        return std::nullopt;
    }

    const size_t SampleCount = FrameCount * ChannelCount;
    const size_t SampleBytes = Bits / ByteBits;
    if (SampleCount > Pcm.size() / SampleBytes)
    {
        return std::nullopt;
    }

    constexpr float UnsignedMid = 128.f;
    constexpr float SignedScale = 32768.f;
    Sound::FSample Sample{ .Rate = Rate, .ChannelCount = ChannelCount };
    Sample.Data.resize(SampleCount);
    for (size_t Index = 0; Index < SampleCount; ++Index)
    {
        if (Bits == UnsignedBits)
        {
            Sample.Data[Index] = (float(Pcm[Index]) - UnsignedMid) / UnsignedMid;
        }
        else
        {
            int16_t Value = 0;
            std::memcpy(&Value, Pcm.data() + Index * SampleBytes, sizeof(Value));
            Sample.Data[Index] = float(Value) / SignedScale;
        }
    }

    return Sample;
}

struct FRiffHeader
{
    char Riff[4];
    uint32_t Size;
    char Wave[4];
};
static_assert(sizeof(FRiffHeader) == 12);

struct FChunkHeader
{
    char Id[4];
    uint32_t Size;
};
static_assert(sizeof(FChunkHeader) == 8);

struct FWaveFormat
{
    uint16_t Format;
    uint16_t ChannelCount;
    uint32_t Rate;
    uint32_t ByteRate;
    uint16_t BlockAlign;
    uint16_t Bits;
};
static_assert(sizeof(FWaveFormat) == 16);

// An SMP_ sample record: the tag, the padded PCM size, format words and the frame count, then the PCM.
struct FSoundRecord
{
    char Tag[4];
    uint32_t PaddedBytes;
    uint32_t Format;
    uint32_t MetadataSize;
    uint32_t RepeatedFormat;
    uint32_t Rate;
    uint32_t Bits;
    uint32_t ChannelCount;
    uint32_t FrameCount;
};
static_assert(sizeof(FSoundRecord) == 36);

// The device layer (SoundDeviceOpen (0047e9c0)) turns a volume step n = volume >> 8 into 873 * ln((n + 1) / 256) hundredths of a decibel, truncated, with step 0 silent (-10000) and step 255 at full level.
// Pan attenuates the far side by the step of its distance from the far end.
constexpr int VolumeSteps = 256;
constexpr int VolumeShift = 8;
constexpr int PanShift = 7;
constexpr int SilentHundredths = -10000;
constexpr double HundredthsPerLn = 873.;
constexpr double HundredthsPerDecade = 2000.;

int StepHundredths(const int Step)
{
    if (Step <= 0)
    {
        return SilentHundredths;
    }

    return Step >= VolumeSteps - 1 ? 0 : int(HundredthsPerLn * std::log(double(Step + 1) / double(VolumeSteps)));
}

float HundredthsGain(const int Hundredths)
{
    return Hundredths <= SilentHundredths ? 0.f : float(std::pow(10., double(Hundredths) / HundredthsPerDecade));
}

// SoundDeviceUpdate (0047ee10): DirectSound pan in hundredths of a decibel, positive towards the right, attenuating the other side.
int PanHundredths(const int Pan)
{
    const int Step = VolumeSteps - 1 - (std::abs(Pan) >> PanShift);
    return Pan >= 0 ? -StepHundredths(Step) : StepHundredths(Step);
}
}

std::optional<Sound::FSample> Sound::DecodeWav(const std::span<const uint8_t> File)
{
    const std::optional<FRiffHeader> Riff = ReadStruct<FRiffHeader>(File);
    if (!Riff || std::memcmp(Riff->Riff, "RIFF", 4) != 0 || std::memcmp(Riff->Wave, "WAVE", 4) != 0)
    {
        return std::nullopt;
    }

    constexpr uint16_t PcmFormat = 1;
    std::optional<FWaveFormat> Format;
    size_t Offset = sizeof(FRiffHeader);
    while (const std::optional<FChunkHeader> Chunk = ReadStruct<FChunkHeader>(File, Offset))
    {
        const size_t Body = Offset + sizeof(FChunkHeader);
        if (Chunk->Size > File.size() - Body)
        {
            return std::nullopt;
        }

        const std::span<const uint8_t> Data = File.subspan(Body, Chunk->Size);
        if (std::memcmp(Chunk->Id, "fmt ", 4) == 0)
        {
            Format = ReadStruct<FWaveFormat>(Data);
        }
        else if (std::memcmp(Chunk->Id, "data", 4) == 0)
        {
            if (!Format || Format->Format != PcmFormat || Format->Bits == 0)
            {
                return std::nullopt;
            }

            const size_t FrameBytes = size_t(Format->Bits / ByteBits) * Format->ChannelCount;
            return FrameBytes == 0 ? std::nullopt : ConvertPcm(Data, Format->Rate, Format->Bits, Format->ChannelCount, Data.size() / FrameBytes);
        }

        // RIFF chunks are word aligned.
        Offset = Body + Chunk->Size + (Chunk->Size & 1);
    }

    return std::nullopt;
}

std::optional<std::vector<Sound::FSample>> Sound::DecodeGafSamples(const std::span<const uint8_t> File)
{
    constexpr size_t SampleSlot = 1;
    const std::optional<Gaf::FSlots> Slots = Gaf::ReadEntrySlots(File, Gaf::Tag("SMP_"));
    if (!Slots)
    {
        return std::nullopt;
    }

    const std::vector<uint8_t>& Records = (*Slots)[SampleSlot];
    std::vector<FSample> Samples;
    size_t Offset = 0;
    while (Offset < Records.size())
    {
        const std::optional<FSoundRecord> Record = ReadStruct<FSoundRecord>(Records, Offset);
        const size_t Body = Offset + sizeof(FSoundRecord);
        if (!Record || std::memcmp(Record->Tag, "SND_", 4) != 0 || Record->PaddedBytes > Records.size() - Body)
        {
            return std::nullopt;
        }

        std::optional<FSample> Sample = ConvertPcm(std::span(Records).subspan(Body, Record->PaddedBytes), Record->Rate, Record->Bits, Record->ChannelCount,
            Record->FrameCount);
        if (!Sample)
        {
            return std::nullopt;
        }

        Samples.push_back(std::move(*Sample));
        Offset = Body + Record->PaddedBytes;
    }

    return Samples;
}

std::optional<Sound::FSample> Sound::DecodeMp3(const std::span<const uint8_t> File)
{
    const ma_decoder_config Config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder Decoder;
    if (ma_decoder_init_memory(File.data(), File.size(), &Config, &Decoder) != MA_SUCCESS)
    {
        return std::nullopt;
    }

    FSample Sample{ .Rate = Decoder.outputSampleRate, .ChannelCount = Decoder.outputChannels };
    constexpr ma_uint64 ChunkFrames = 4096;
    for (ma_uint64 Read = ChunkFrames; Read == ChunkFrames && Sample.ChannelCount != 0;)
    {
        const size_t Start = Sample.Data.size();
        Sample.Data.resize(Start + ChunkFrames * Sample.ChannelCount);
        if (ma_decoder_read_pcm_frames(&Decoder, Sample.Data.data() + Start, ChunkFrames, &Read) != MA_SUCCESS)
        {
            Read = 0;
        }

        Sample.Data.resize(Start + Read * Sample.ChannelCount);
    }

    ma_decoder_uninit(&Decoder);
    if (Sample.Data.empty())
    {
        return std::nullopt;
    }

    return Sample;
}

struct Sound::FMixer::FDevice
{
    ma_device Device{};
    bool bStarted = false;
};

Sound::FMixer::FMixer() = default;

Sound::FMixer::~FMixer()
{
    CloseDevice();
}

void Sound::FMixer::CloseDevice()
{
    if (Device && Device->bStarted)
    {
        ma_device_uninit(&Device->Device);
    }

    Device.reset();
}

bool Sound::FMixer::Open(const FOutputFormat& InFormat)
{
    // The mix runs at the primary buffer's rate and channel count, and miniaudio converts it to the system's; an 8-bit buffer is reproduced by truncating the mix.
    CloseDevice();
    {
        const std::scoped_lock Lock(Mutex);
        Format = InFormat;
    }

    Device = std::make_unique<FDevice>();
    ma_device_config Config = ma_device_config_init(ma_device_type_playback);
    Config.playback.format = ma_format_f32;
    Config.playback.channels = Format.bMono ? MonoChannels : StereoChannels;
    Config.sampleRate = Format.Rate;
    Config.pUserData = this;
    Config.dataCallback = [](ma_device* const Output, void* const Frames, const void*, const ma_uint32 FrameCount)
    {
        Mix(*static_cast<FMixer*>(Output->pUserData), static_cast<float*>(Frames), FrameCount);
    };
    if (ma_device_init(nullptr, &Config, &Device->Device) != MA_SUCCESS)
    {
        Device.reset();
        return false;
    }

    OutputRate = Device->Device.sampleRate;
    if (ma_device_start(&Device->Device) != MA_SUCCESS)
    {
        ma_device_uninit(&Device->Device);
        Device.reset();
        return false;
    }

    Device->bStarted = true;
    return true;
}

double Sound::FMixer::SecondsLeft(const FChannel& Channel)
{
    const FSample& Sample = *Channel.Sample;
    return (double(Sample.FrameCount()) - Channel.Position) / double(Sample.Rate);
}

uint32_t Sound::FMixer::Play(std::shared_ptr<const FSample> Sample, const bool bLoop, const uint32_t Priority, const uint32_t Mask,
    const std::optional<int> Volume, const std::optional<int> Pan)
{
    if (!Device || !Sample || Sample->FrameCount() == 0)
    {
        return 0;
    }

    const std::scoped_lock Lock(Mutex);
    std::optional<uint32_t> Chosen;
    for (uint32_t Index = 0; Index < ChannelCount && !Chosen; ++Index)
    {
        if ((Mask & (1u << Index)) != 0 && !Channels[Index].Sample)
        {
            Chosen = Index;
        }
    }

    if (!Chosen)
    {
        // Every channel of the mask is busy: take the lowest priority not above the new one.
        std::optional<uint32_t> Best;
        for (uint32_t Candidate = 0; Candidate < ChannelCount; ++Candidate)
        {
            const FChannel& Channel = Channels[Candidate];
            if ((Mask & (1u << Candidate)) == 0 || Channel.Priority > Priority)
            {
                continue;
            }

            if (!Best || Channel.Priority < Channels[*Best].Priority ||
                (Channel.Priority == Channels[*Best].Priority && SecondsLeft(Channel) < SecondsLeft(Channels[*Best])))
            {
                Best = Candidate;
            }
        }

        if (!Best)
        {
            return 0;
        }

        Chosen = Best;
    }

    FChannel& Channel = Channels[*Chosen];
    Channel.Sample = std::move(Sample);
    Channel.Position = 0.;
    Channel.bLoop = bLoop;
    Channel.Priority = Priority;
    Channel.Volume = Volume ? std::clamp(*Volume, 0, MaxVolume) : Channel.Volume;
    Channel.Pan = Pan ? std::clamp(*Pan, -MaxPan, MaxPan) : Channel.Pan;
    Channel.Serial = NextSerial++;
    return *Chosen + 1;
}

int Sound::FMixer::GetVolume(const uint32_t Channel) const
{
    const std::scoped_lock Lock(Mutex);
    return Channel >= 1 && Channel <= ChannelCount ? Channels[Channel - 1].Volume : 0;
}

uint32_t Sound::FMixer::GetSerial(const uint32_t Channel) const
{
    const std::scoped_lock Lock(Mutex);
    return Channel >= 1 && Channel <= ChannelCount && Channels[Channel - 1].Sample ? Channels[Channel - 1].Serial : 0;
}

bool Sound::FMixer::IsPlaying(const uint32_t Channel) const
{
    const std::scoped_lock Lock(Mutex);
    return Channel >= 1 && Channel <= ChannelCount && Channels[Channel - 1].Sample != nullptr;
}

void Sound::FMixer::Stop(const uint32_t Mask)
{
    const std::scoped_lock Lock(Mutex);
    for (uint32_t Index = 0; Index < ChannelCount; ++Index)
    {
        if ((Mask & (1u << Index)) != 0)
        {
            Channels[Index].Sample.reset();
        }
    }
}

void Sound::FMixer::SetVolume(const uint32_t Channel, const int Volume)
{
    const std::scoped_lock Lock(Mutex);
    if (Channel >= 1 && Channel <= ChannelCount)
    {
        Channels[Channel - 1].Volume = std::clamp(Volume, 0, MaxVolume);
    }
}

void Sound::FMixer::SetPan(const uint32_t Channel, const int Pan)
{
    const std::scoped_lock Lock(Mutex);
    if (Channel >= 1 && Channel <= ChannelCount)
    {
        Channels[Channel - 1].Pan = std::clamp(Pan, -MaxPan, MaxPan);
    }
}

void Sound::FMixer::SetPaused(const bool bInPaused)
{
    const std::scoped_lock Lock(Mutex);
    bPaused = bInPaused;
}

void Sound::FMixer::Mix(FMixer& Mixer, float* const Output, const uint32_t FrameCount)
{
    const std::scoped_lock Lock(Mixer.Mutex);
    const uint32_t OutputChannels = Mixer.Format.bMono ? MonoChannels : StereoChannels;
    std::fill_n(Output, size_t(FrameCount) * OutputChannels, 0.f);
    if (Mixer.bPaused)
    {
        return;
    }

    for (FChannel& Channel : Mixer.Channels)
    {
        if (!Channel.Sample)
        {
            continue;
        }

        const FSample& Sample = *Channel.Sample;
        const float Gain = HundredthsGain(StepHundredths(Channel.Volume >> VolumeShift));
        const int Pan = PanHundredths(Channel.Pan);
        // A mono primary buffer has no sides to pan between.
        const float LeftGain = Gain * (Pan > 0 && !Mixer.Format.bMono ? HundredthsGain(-Pan) : 1.f);
        const float RightGain = Gain * (Pan < 0 && !Mixer.Format.bMono ? HundredthsGain(Pan) : 1.f);
        const double Step = double(Sample.Rate) / double(Mixer.OutputRate);
        const size_t SampleFrames = Sample.FrameCount();
        for (uint32_t Frame = 0; Frame < FrameCount; ++Frame)
        {
            if (Channel.Position >= double(SampleFrames))
            {
                if (!Channel.bLoop)
                {
                    Channel.Sample.reset();
                    break;
                }

                Channel.Position = std::fmod(Channel.Position, double(SampleFrames));
            }

            // Linear interpolation between neighbouring frames, the next one wrapping only for a loop.
            const size_t First = size_t(Channel.Position);
            const size_t Next = First + 1 < SampleFrames ? First + 1 : Channel.bLoop ? 0 : First;
            const float Blend = float(Channel.Position - double(First));
            const auto At = [&](const size_t Index, const uint32_t Side)
            {
                return Sample.Data[Index * Sample.ChannelCount + std::min(Side, Sample.ChannelCount - 1)];
            };
            const float Left = At(First, 0) + (At(Next, 0) - At(First, 0)) * Blend;
            const float Right = At(First, 1) + (At(Next, 1) - At(First, 1)) * Blend;
            if (OutputChannels == MonoChannels)
            {
                constexpr float DownmixGain = 0.5f;
                Output[Frame] += (Left * LeftGain + Right * RightGain) * DownmixGain;
            }
            else
            {
                Output[Frame * StereoChannels] += Left * LeftGain;
                Output[Frame * StereoChannels + 1] += Right * RightGain;
            }

            Channel.Position += Step;
        }
    }

    if (Mixer.Format.bEightBits)
    {
        // Unsigned 8-bit PCM keeps the top byte of each 16-bit value.
        constexpr float EightBitSteps = 128.f;
        for (float& Value : std::span(Output, size_t(FrameCount) * OutputChannels))
        {
            Value = std::floor(std::clamp(Value, -1.f, 1.f - 1.f / EightBitSteps) * EightBitSteps) / EightBitSteps;
        }
    }
}
