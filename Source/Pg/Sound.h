#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

// The pg sound library: samples (pgsnd.cpp) and the channel mixer (pgaudio.cpp) that the original drove through DirectSound.
namespace Sound
{
// PCM converted to floats, channels interleaved.
struct FSample
{
    uint32_t Rate = 0;
    uint32_t ChannelCount = 0;
    std::vector<float> Data;

    size_t FrameCount() const { return ChannelCount == 0 ? 0 : Data.size() / ChannelCount; }
};

// A RIFF WAVE file with 8-bit unsigned or 16-bit signed PCM, mono or stereo (SoundParseWav (0045da80)).
std::optional<FSample> DecodeWav(std::span<const uint8_t> File);
// The SND_ records of a GAF's SMP_ entry, which animation frame events play by 1-based index. A GAF without one yields none.
std::optional<std::vector<FSample>> DecodeGafSamples(std::span<const uint8_t> File);
// Replaces the 0-based SND_ record of an SMP_ sample stream with the sample as 16-bit PCM, keeping the record's format words.
bool WriteGafSample(std::vector<uint8_t>& Records, size_t Index, const FSample& Sample);
// A float sample as the 16-bit value it came from, exact for 8-bit and 16-bit sources.
int16_t SampleToPcm16(float Value);
// An MPEG audio layer III stream, such as the intro's sound.
std::optional<FSample> DecodeMp3(std::span<const uint8_t> File);

// The original's channel volume runs from 0 to 0xffff and its pan from -0x7fff (left) to 0x7fff (right).
inline constexpr int MaxVolume = 0xffff;
inline constexpr int MaxPan = 0x7fff;

// SoundOpen's output format (0045f300): its flags pick 8 or 16 bits, 11025 or 22050 Hz, and mono or stereo for DirectSound's primary buffer, which mixes every channel.
// VzOpenSound (004477e0) opens 16 bits, 22050 Hz, stereo.
struct FOutputFormat
{
    static constexpr uint32_t LowRate = 11025;
    static constexpr uint32_t HighRate = 22050;

    bool bEightBits = false;
    uint32_t Rate = HighRate;
    bool bMono = false;

    bool operator==(const FOutputFormat&) const = default;
};

// A Freeverb room for the port's optional reverb: its size and damping from 0 to 1, and the share of the reverb in the output.
struct FReverbPreset
{
    float RoomSize = 0.5f;
    float Damping = 0.5f;
    float Wet = 0.2f;
};

// SoundOpen (0045f300) sets up 16 channels; each plays one sample at a time.
class FMixer
{
public:
    static constexpr uint32_t ChannelCount = 16;
    static constexpr uint32_t AllChannels = (1u << ChannelCount) - 1;

    FMixer();
    ~FMixer();
    FMixer(const FMixer&) = delete;
    FMixer& operator=(const FMixer&) = delete;

    // Opens the default output device in the format, or reopens it in a new one with every channel kept; without a device every call is ignored.
    bool Open(const FOutputFormat& InFormat);

    // SoundPlay (0045f7e0): the first idle channel of Mask, else the one playing the lowest priority not above Priority (the one closest to its end on a tie).
    // Volume and pan replace the channel's own when given; Muffle starts the sound behind the distance filter (see SetMuffle).
    // Returns the 1-based channel, or 0 when none could take it.
    uint32_t Play(std::shared_ptr<const FSample> Sample, bool bLoop, uint32_t Priority, uint32_t Mask, std::optional<int> Volume = std::nullopt,
        std::optional<int> Pan = std::nullopt, float Muffle = 0.f);
    // SoundChannelPlaying (0045fb70).
    bool IsPlaying(uint32_t Channel) const;
    // SoundStop (0045fa00): stops every channel in Mask.
    void Stop(uint32_t Mask);
    // SoundGetVolume at 0045fda0.
    int GetVolume(uint32_t Channel) const;
    // Channel +0x3c: a number given to each sound when it starts, so a caller can tell its sound still plays; zero for an idle channel.
    uint32_t GetSerial(uint32_t Channel) const;
    // SoundSetVolume (0045fe00) and SoundSetPan (0045ff00): the setting stays with the channel for later samples.
    void SetVolume(uint32_t Channel, int Volume);
    void SetPan(uint32_t Channel, int Pan);
    // The port's distance filter: 0 leaves the channel clear, 1 closes its low-pass to the farthest setting.
    // Unlike the original, volume and pan glide instead of jumping and a stopped or replaced sound fades out over a few milliseconds instead of being cut.
    void SetMuffle(uint32_t Channel, float Muffle);
    // The port's reverb over the channels of SendMask; none turns it off.
    void SetReverb(const std::optional<FReverbPreset>& Preset, uint32_t SendMask);
    // Holds every channel where it is and outputs silence, while the game stands still.
    void SetPaused(bool bInPaused);

private:
    // What the mix carries from one buffer to the next for a playing sound: its gains as they glide and its filter's memory.
    struct FVoiceState
    {
        float LeftGain = 0.f;
        float RightGain = 0.f;
        bool bStarted = false;
        float LeftLow = 0.f;
        float RightLow = 0.f;
    };
    struct FChannel
    {
        std::shared_ptr<const FSample> Sample;
        double Position = 0.;
        bool bLoop = false;
        uint32_t Priority = 0;
        int Volume = MaxVolume;
        int Pan = 0;
        uint32_t Serial = 0;
        float Muffle = 0.f;
        FVoiceState State;
    };
    // A sound the mix let go of, fading out where it was.
    struct FTail
    {
        std::shared_ptr<const FSample> Sample;
        double Position = 0.;
        bool bLoop = false;
        float Muffle = 0.f;
        bool bReverb = false;
        FVoiceState State;
    };
    struct FDevice;
    struct FReverb;

    static void Mix(FMixer& Mixer, float* Output, uint32_t FrameCount);
    void CloseDevice();
    // Moves a playing channel's sound to the fading tails, so cutting it does not click.
    void Release(uint32_t Index);
    // Builds the reverb for the preset at the output rate.
    void RebuildReverb();
    // Time left in a playing channel, the tie breaker between equal priorities (channel +0x24, which SoundPoll (0045f6b0) keeps).
    static double SecondsLeft(const FChannel& Channel);

    mutable std::mutex Mutex;
    std::array<FChannel, ChannelCount> Channels;
    uint32_t OutputRate = 0;
    FOutputFormat Format;
    uint32_t NextSerial = 1;
    bool bPaused = false;
    std::vector<FTail> Tails;
    std::optional<FReverbPreset> ReverbPreset;
    uint32_t ReverbMask = 0;
    std::unique_ptr<FReverb> Reverb;
    // The reverb's input for the buffer being mixed, stereo interleaved.
    std::vector<float> ReverbSend;
    std::unique_ptr<FDevice> Device;
};
}
