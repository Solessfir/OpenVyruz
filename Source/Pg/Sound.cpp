#include "Pg/Sound.h"

#include "Pg/Binary.h"
#include "Pg/Gaf.h"

#include <algorithm>
#include <array>
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

int16_t Sound::SampleToPcm16(const float Value)
{
    constexpr float SignedScale = 32768.f;
    return int16_t(std::clamp<long>(std::lround(Value * SignedScale), INT16_MIN, INT16_MAX));
}

bool Sound::WriteGafSample(std::vector<uint8_t>& Records, const size_t Index, const FSample& Sample)
{
    constexpr uint32_t Pcm16Bits = 16;
    constexpr size_t Alignment = 4;
    size_t Offset = 0;
    for (size_t Skipped = 0; Skipped < Index; ++Skipped)
    {
        const std::optional<FSoundRecord> Record = ReadStruct<FSoundRecord>(Records, Offset);
        if (!Record || Record->PaddedBytes > Records.size() - Offset - sizeof(FSoundRecord))
        {
            return false;
        }

        Offset += sizeof(FSoundRecord) + Record->PaddedBytes;
    }

    std::optional<FSoundRecord> Record = ReadStruct<FSoundRecord>(Records, Offset);
    if (!Record || Record->PaddedBytes > Records.size() - Offset - sizeof(FSoundRecord) || Sample.ChannelCount == 0)
    {
        return false;
    }

    const size_t OldEnd = Offset + sizeof(FSoundRecord) + Record->PaddedBytes;
    std::vector<uint8_t> Body;
    for (const float Value : Sample.Data)
    {
        const int16_t Pcm = SampleToPcm16(Value);
        Body.push_back(uint8_t(Pcm));
        Body.push_back(uint8_t(uint16_t(Pcm) >> 8));
    }

    Body.resize((Body.size() + Alignment - 1) & ~(Alignment - 1), 0);
    Record->PaddedBytes = uint32_t(Body.size());
    Record->Rate = Sample.Rate;
    Record->Bits = Pcm16Bits;
    Record->ChannelCount = Sample.ChannelCount;
    Record->FrameCount = uint32_t(Sample.FrameCount());
    const auto* const Header = reinterpret_cast<const uint8_t*>(&*Record);
    Body.insert(Body.begin(), Header, Header + sizeof(FSoundRecord));
    Records.erase(Records.begin() + ptrdiff_t(Offset), Records.begin() + ptrdiff_t(OldEnd));
    Records.insert(Records.begin() + ptrdiff_t(Offset), Body.begin(), Body.end());
    return true;
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

namespace
{
// The mix's gains glide toward their targets with a 5 ms time constant, and a released sound fades with a 3 ms one until it drops below Silence.
constexpr float GlideSeconds = 0.005f;
constexpr float FadeSeconds = 0.003f;
constexpr float Silence = 1e-4f;
// More fading sounds than this drop the oldest, which has faded the most.
constexpr size_t MaxTails = 32;
// The distance filter: a one-pole low-pass from ClearCutoff Hz with no muffle down to MuffledCutoff Hz at full muffle, on a logarithmic sweep.
constexpr float ClearCutoff = 8000.f;
constexpr float MuffledCutoff = 700.f;
constexpr float TwoPi = 6.28318530718f;

float LowPassCoefficient(const float Muffle, const float Rate)
{
    const float Cutoff = ClearCutoff * std::pow(MuffledCutoff / ClearCutoff, std::clamp(Muffle, 0.f, 1.f));
    return 1.f - std::exp(-TwoPi * Cutoff / Rate);
}

// Freeverb's tuning, in samples at 44100 Hz.
constexpr uint32_t TuningRate = 44100;
constexpr std::array<uint32_t, 8> CombTuning = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr std::array<uint32_t, 4> AllPassTuning = { 556, 441, 341, 225 };
constexpr uint32_t StereoSpread = 23;
constexpr float InputGain = 0.015f;
constexpr float RoomScale = 0.28f;
constexpr float RoomOffset = 0.7f;
constexpr float DampingScale = 0.4f;
constexpr float AllPassFeedback = 0.5f;
// Below this a comb's memory is flushed to zero, so the tail never decays into slow denormals.
constexpr float Denormal = 1e-15f;
}

// Freeverb (Jezar at Dreampoint, public domain): eight parallel combs and four all-passes in series on each side, the right side's delays a little longer for width.
struct Sound::FMixer::FReverb
{
    struct FComb
    {
        std::vector<float> Buffer;
        size_t Index = 0;
        float Store = 0.f;
    };
    struct FAllPass
    {
        std::vector<float> Buffer;
        size_t Index = 0;
    };

    FReverb(const uint32_t Rate, const FReverbPreset& Preset)
        : Feedback(std::clamp(Preset.RoomSize, 0.f, 1.f) * RoomScale + RoomOffset)
        , Damping(std::clamp(Preset.Damping, 0.f, 1.f) * DampingScale)
        , Wet(std::max(Preset.Wet, 0.f))
    {
        const auto Length = [Rate](const uint32_t Tuning) { return std::max<size_t>(1, size_t(uint64_t(Tuning) * Rate / TuningRate)); };
        for (size_t Side = 0; Side < Combs.size(); ++Side)
        {
            const uint32_t Spread = Side == 0 ? 0 : StereoSpread;
            for (size_t Index = 0; Index < CombTuning.size(); ++Index)
            {
                Combs[Side][Index].Buffer.assign(Length(CombTuning[Index] + Spread), 0.f);
            }

            for (size_t Index = 0; Index < AllPassTuning.size(); ++Index)
            {
                AllPasses[Side][Index].Buffer.assign(Length(AllPassTuning[Index] + Spread), 0.f);
            }
        }
    }

    // The reverb of one stereo frame, already scaled by the wet share.
    std::pair<float, float> Process(const float Left, const float Right)
    {
        const float Input = (Left + Right) * InputGain;
        std::array<float, 2> Out{};
        for (size_t Side = 0; Side < Out.size(); ++Side)
        {
            for (FComb& Comb : Combs[Side])
            {
                const float Delayed = Comb.Buffer[Comb.Index];
                Comb.Store = Delayed * (1.f - Damping) + Comb.Store * Damping;
                Comb.Store = std::abs(Comb.Store) < Denormal ? 0.f : Comb.Store;
                Comb.Buffer[Comb.Index] = Input + Comb.Store * Feedback;
                Comb.Index = Comb.Index + 1 == Comb.Buffer.size() ? 0 : Comb.Index + 1;
                Out[Side] += Delayed;
            }

            for (FAllPass& AllPass : AllPasses[Side])
            {
                const float Delayed = AllPass.Buffer[AllPass.Index];
                AllPass.Buffer[AllPass.Index] = Out[Side] + Delayed * AllPassFeedback;
                AllPass.Index = AllPass.Index + 1 == AllPass.Buffer.size() ? 0 : AllPass.Index + 1;
                Out[Side] = Delayed - Out[Side];
            }
        }

        return { Out[0] * Wet, Out[1] * Wet };
    }

    std::array<std::array<FComb, CombTuning.size()>, 2> Combs;
    std::array<std::array<FAllPass, AllPassTuning.size()>, 2> AllPasses;
    float Feedback = 0.f;
    float Damping = 0.f;
    float Wet = 0.f;
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

    {
        const std::scoped_lock Lock(Mutex);
        OutputRate = Device->Device.sampleRate;
        RebuildReverb();
    }

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
    const std::optional<int> Volume, const std::optional<int> Pan, const float Muffle)
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

    Release(*Chosen);
    FChannel& Channel = Channels[*Chosen];
    Channel.Sample = std::move(Sample);
    Channel.Muffle = std::clamp(Muffle, 0.f, 1.f);
    Channel.State = {};
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
            Release(Index);
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

void Sound::FMixer::SetMuffle(const uint32_t Channel, const float Muffle)
{
    const std::scoped_lock Lock(Mutex);
    if (Channel >= 1 && Channel <= ChannelCount)
    {
        Channels[Channel - 1].Muffle = std::clamp(Muffle, 0.f, 1.f);
    }
}

void Sound::FMixer::SetReverb(const std::optional<FReverbPreset>& Preset, const uint32_t SendMask)
{
    const std::scoped_lock Lock(Mutex);
    ReverbPreset = Preset;
    ReverbMask = SendMask;
    RebuildReverb();
}

void Sound::FMixer::RebuildReverb()
{
    Reverb = ReverbPreset && OutputRate != 0 ? std::make_unique<FReverb>(OutputRate, *ReverbPreset) : nullptr;
}

void Sound::FMixer::Release(const uint32_t Index)
{
    const FChannel& Channel = Channels[Index];
    if (!Channel.Sample || !Channel.State.bStarted)
    {
        return;
    }

    if (Tails.size() >= MaxTails)
    {
        Tails.erase(Tails.begin());
    }

    Tails.push_back({
        .Sample = Channel.Sample,
        .Position = Channel.Position,
        .bLoop = Channel.bLoop,
        .Muffle = Channel.Muffle,
        .bReverb = (ReverbMask & (1u << Index)) != 0,
        .State = Channel.State,
    });
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

    const float Rate = float(Mixer.OutputRate);
    const float Glide = 1.f - std::exp(-1.f / (GlideSeconds * Rate));
    const float Fade = std::exp(-1.f / (FadeSeconds * Rate));
    float* Send = nullptr;
    if (Mixer.Reverb)
    {
        Mixer.ReverbSend.assign(size_t(FrameCount) * StereoChannels, 0.f);
        Send = Mixer.ReverbSend.data();
    }

    // Mixes a sound into the output from where it stands; false once it has ended, or, fading, gone silent.
    const auto MixVoice = [&](const FSample& Sample, double& Position, const bool bLoop, const float LeftTarget, const float RightTarget, const float Muffle,
                              FVoiceState& State, float* const SendTo, const bool bFading)
    {
        if (!State.bStarted)
        {
            State.LeftGain = LeftTarget;
            State.RightGain = RightTarget;
            State.bStarted = true;
        }

        const bool bFilter = Muffle > 0.f;
        const float LowPass = bFilter ? LowPassCoefficient(Muffle, Rate) : 1.f;
        const double Step = double(Sample.Rate) / double(Mixer.OutputRate);
        const size_t SampleFrames = Sample.FrameCount();
        for (uint32_t Frame = 0; Frame < FrameCount; ++Frame)
        {
            if (Position >= double(SampleFrames))
            {
                if (!bLoop)
                {
                    return false;
                }

                Position = std::fmod(Position, double(SampleFrames));
            }

            // Linear interpolation between neighbouring frames, the next one wrapping only for a loop.
            const size_t First = size_t(Position);
            const size_t Next = First + 1 < SampleFrames ? First + 1 : bLoop ? 0 : First;
            const float Blend = float(Position - double(First));
            const auto At = [&](const size_t Index, const uint32_t Side)
            {
                return Sample.Data[Index * Sample.ChannelCount + std::min(Side, Sample.ChannelCount - 1)];
            };
            float Left = At(First, 0) + (At(Next, 0) - At(First, 0)) * Blend;
            float Right = At(First, 1) + (At(Next, 1) - At(First, 1)) * Blend;
            if (bFilter)
            {
                State.LeftLow += (Left - State.LeftLow) * LowPass;
                State.RightLow += (Right - State.RightLow) * LowPass;
                Left = State.LeftLow;
                Right = State.RightLow;
            }

            if (bFading)
            {
                State.LeftGain *= Fade;
                State.RightGain *= Fade;
            }
            else
            {
                State.LeftGain += (LeftTarget - State.LeftGain) * Glide;
                State.RightGain += (RightTarget - State.RightGain) * Glide;
            }

            const float LeftOut = Left * State.LeftGain;
            const float RightOut = Right * State.RightGain;
            if (OutputChannels == MonoChannels)
            {
                constexpr float DownmixGain = 0.5f;
                Output[Frame] += (LeftOut + RightOut) * DownmixGain;
            }
            else
            {
                Output[Frame * StereoChannels] += LeftOut;
                Output[Frame * StereoChannels + 1] += RightOut;
            }

            if (SendTo)
            {
                SendTo[Frame * StereoChannels] += LeftOut;
                SendTo[Frame * StereoChannels + 1] += RightOut;
            }

            Position += Step;
        }

        return !bFading || State.LeftGain >= Silence || State.RightGain >= Silence;
    };

    for (uint32_t Index = 0; Index < ChannelCount; ++Index)
    {
        FChannel& Channel = Mixer.Channels[Index];
        if (!Channel.Sample)
        {
            continue;
        }

        const float Gain = HundredthsGain(StepHundredths(Channel.Volume >> VolumeShift));
        const int Pan = PanHundredths(Channel.Pan);
        // A mono primary buffer has no sides to pan between.
        const float LeftGain = Gain * (Pan > 0 && !Mixer.Format.bMono ? HundredthsGain(-Pan) : 1.f);
        const float RightGain = Gain * (Pan < 0 && !Mixer.Format.bMono ? HundredthsGain(Pan) : 1.f);
        float* const SendTo = (Mixer.ReverbMask & (1u << Index)) != 0 ? Send : nullptr;
        if (!MixVoice(*Channel.Sample, Channel.Position, Channel.bLoop, LeftGain, RightGain, Channel.Muffle, Channel.State, SendTo, false))
        {
            Channel.Sample.reset();
        }
    }

    std::erase_if(Mixer.Tails, [&](FTail& Tail)
    {
        return !MixVoice(*Tail.Sample, Tail.Position, Tail.bLoop, 0.f, 0.f, Tail.Muffle, Tail.State, Tail.bReverb ? Send : nullptr, true);
    });

    if (Send)
    {
        for (uint32_t Frame = 0; Frame < FrameCount; ++Frame)
        {
            const auto [Left, Right] = Mixer.Reverb->Process(Send[Frame * StereoChannels], Send[Frame * StereoChannels + 1]);
            if (OutputChannels == MonoChannels)
            {
                constexpr float DownmixGain = 0.5f;
                Output[Frame] += (Left + Right) * DownmixGain;
            }
            else
            {
                Output[Frame * StereoChannels] += Left;
                Output[Frame * StereoChannels + 1] += Right;
            }
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
