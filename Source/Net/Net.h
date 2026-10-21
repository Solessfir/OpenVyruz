#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The co-op's transport: one ENet host, either listening for players or connected to one, with a reliable and an unreliable channel.
namespace Net
{
inline constexpr uint16_t DefaultPort = 47210;
inline constexpr size_t MaxPeers = 3;

enum class EChannel : uint8_t
{
    Reliable = 0,
    Unreliable = 1,
};

enum class EEventKind : uint8_t
{
    Connected,
    Disconnected,
    Received,
};

struct FEvent
{
    EEventKind Kind = EEventKind::Received;
    // The peer's id, stable while it stays connected; a client's one peer is the host.
    uint32_t Peer = 0;
    std::vector<uint8_t> Bytes;
};

class FEndpoint
{
public:
    FEndpoint();
    ~FEndpoint();
    FEndpoint(const FEndpoint&) = delete;
    FEndpoint& operator=(const FEndpoint&) = delete;

    // Listens on a UDP port for up to MaxPeers players.
    bool Host(uint16_t Port);
    // Starts connecting to a host; the Connected event says when it is done.
    bool Connect(std::string_view Address, uint16_t Port);
    bool IsHost() const;
    // Every event since the last call, without waiting.
    std::vector<FEvent> Poll();
    void Send(uint32_t Peer, std::span<const uint8_t> Bytes, EChannel Channel);
    void SendToAll(std::span<const uint8_t> Bytes, EChannel Channel);
    // Sends what is queued and closes every connection.
    void Close();

private:
    struct FState;
    std::unique_ptr<FState> State;
};

// A message body in little-endian order, as both builds of the port read it.
class FWriter
{
public:
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void Put(const T Value)
    {
        const size_t At = Bytes.size();
        Bytes.resize(At + sizeof(T));
        std::memcpy(Bytes.data() + At, &Value, sizeof(T));
    }

    void PutString(std::string_view Text);
    const std::vector<uint8_t>& Data() const { return Bytes; }

private:
    std::vector<uint8_t> Bytes;
};

class FReader
{
public:
    explicit FReader(std::span<const uint8_t> Bytes) : Bytes(Bytes) {}

    // A read past the end gives a zero value and leaves the reader failed.
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    T Get()
    {
        T Value{};
        if (At + sizeof(T) > Bytes.size())
        {
            bFailed = true;
            return Value;
        }

        std::memcpy(&Value, Bytes.data() + At, sizeof(T));
        At += sizeof(T);
        return Value;
    }

    std::string GetString();
    bool IsGood() const { return !bFailed; }
    bool AtEnd() const { return At == Bytes.size(); }

private:
    std::span<const uint8_t> Bytes;
    size_t At = 0;
    bool bFailed = false;
};

// Finding games on the LAN: a host answers broadcast questions on DiscoveryPort with its name, and a searcher collects the answers.
inline constexpr uint16_t DiscoveryPort = DefaultPort + 1;

struct FFoundGame
{
    std::string Address;
    std::string Name;
};

class FDiscovery
{
public:
    FDiscovery() = default;
    ~FDiscovery();
    FDiscovery(const FDiscovery&) = delete;
    FDiscovery& operator=(const FDiscovery&) = delete;

    // A host: answers with Name.
    bool Answer(std::string_view Name);
    // A searcher: asks the LAN once a second.
    bool Search();
    // Answers the questions, or asks and collects the answers.
    void Poll(uint32_t NowMilliseconds);
    const std::vector<FFoundGame>& GetFound() const { return Found; }

private:
    void Close();

    std::intptr_t Socket = -1;
    bool bAnswering = false;
    bool bAcquired = false;
    std::string Name;
    uint32_t AskedAt = 0;
    bool bAsked = false;
    std::vector<FFoundGame> Found;
};

// "address" or "address:port", with the default port.
std::optional<std::pair<std::string, uint16_t>> ParseAddress(std::string_view Text);
}
