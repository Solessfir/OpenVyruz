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

// The original's channel volume runs from 0 to 0xffff and its pan from -0x7fff (left) to 0x7fff (right).
inline constexpr int MaxVolume = 0xffff;
inline constexpr int MaxPan = 0x7fff;

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

    // Opens the default output device; without one every call is ignored.
    bool Open();

    // SoundPlay (0045f7e0): the first idle channel of Mask, else the one playing the lowest priority not above Priority (the one closest to its end on a tie).
    // Volume and pan replace the channel's own when given.
    // Returns the 1-based channel, or 0 when none could take it.
    uint32_t Play(std::shared_ptr<const FSample> Sample, bool bLoop, uint32_t Priority, uint32_t Mask, std::optional<int> Volume = std::nullopt,
        std::optional<int> Pan = std::nullopt);
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

private:
    struct FChannel
    {
        std::shared_ptr<const FSample> Sample;
        double Position = 0.;
        bool bLoop = false;
        uint32_t Priority = 0;
        int Volume = MaxVolume;
        int Pan = 0;
        uint32_t Serial = 0;
    };
    struct FDevice;

    static void Mix(FMixer& Mixer, float* Output, uint32_t FrameCount);
    // Time left in a playing channel, the tie breaker between equal priorities (channel +0x24, which SoundPoll (0045f6b0) keeps).
    static double SecondsLeft(const FChannel& Channel);

    mutable std::mutex Mutex;
    std::array<FChannel, ChannelCount> Channels;
    uint32_t OutputRate = 0;
    uint32_t NextSerial = 1;
    std::unique_ptr<FDevice> Device;
};
}
