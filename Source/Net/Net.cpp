#include "Net/Net.h"

#include <enet/enet.h>

#include <algorithm>
#include <charconv>
#include <cstdio>

namespace Net
{
namespace
{
constexpr size_t ChannelCount = 2;
// How long Close waits for the goodbyes to go out.
constexpr uint32_t CloseMilliseconds = 200;
// Text and lists in a message are bounded, so a bad length cannot make a reader allocate without end.
constexpr uint32_t MaxStringBytes = 1024;

int ENetUsers = 0;

bool AcquireENet()
{
    if (ENetUsers == 0 && enet_initialize() != 0)
    {
        std::fprintf(stderr, "Cannot start the network (enet_initialize)\n");
        return false;
    }

    ++ENetUsers;
    return true;
}

void ReleaseENet()
{
    if (--ENetUsers == 0)
    {
        enet_deinitialize();
    }
}
}

struct FEndpoint::FState
{
    ENetHost* Host = nullptr;
    bool bHost = false;
    bool bAcquired = false;
    std::vector<ENetPeer*> Peers;
    uint32_t NextPeer = 1;

    uint32_t IdOf(ENetPeer* const Peer)
    {
        if (Peer->data == nullptr)
        {
            Peer->data = reinterpret_cast<void*>(uintptr_t(NextPeer++));
            Peers.push_back(Peer);
        }

        return uint32_t(reinterpret_cast<uintptr_t>(Peer->data));
    }

    ENetPeer* PeerOf(const uint32_t Id) const
    {
        for (ENetPeer* const Peer : Peers)
        {
            if (uint32_t(reinterpret_cast<uintptr_t>(Peer->data)) == Id)
            {
                return Peer;
            }
        }

        return nullptr;
    }
};

FEndpoint::FEndpoint() : State(std::make_unique<FState>()) {}

FEndpoint::~FEndpoint()
{
    Close();
}

bool FEndpoint::Host(const uint16_t Port)
{
    Close();
    State->bAcquired = AcquireENet();
    if (!State->bAcquired)
    {
        return false;
    }

    ENetAddress Address{};
    Address.host = ENET_HOST_ANY;
    Address.port = Port;
    State->Host = enet_host_create(&Address, MaxPeers, ChannelCount, 0, 0);
    State->bHost = State->Host != nullptr;
    if (!State->Host)
    {
        std::fprintf(stderr, "Cannot listen on UDP port %u\n", unsigned(Port));
    }

    return State->Host != nullptr;
}

bool FEndpoint::Connect(const std::string_view Address, const uint16_t Port)
{
    Close();
    State->bAcquired = AcquireENet();
    if (!State->bAcquired)
    {
        return false;
    }

    State->Host = enet_host_create(nullptr, 1, ChannelCount, 0, 0);
    if (!State->Host)
    {
        return false;
    }

    ENetAddress Target{};
    const std::string Name(Address);
    if (enet_address_set_host(&Target, Name.c_str()) != 0)
    {
        std::fprintf(stderr, "Cannot resolve %s\n", Name.c_str());
        return false;
    }

    Target.port = Port;
    ENetPeer* const Peer = enet_host_connect(State->Host, &Target, ChannelCount, 0);
    if (!Peer)
    {
        return false;
    }

    State->IdOf(Peer);
    return true;
}

bool FEndpoint::IsHost() const
{
    return State->bHost;
}

std::vector<FEvent> FEndpoint::Poll()
{
    std::vector<FEvent> Events;
    if (!State->Host)
    {
        return Events;
    }

    ENetEvent Event{};
    while (enet_host_service(State->Host, &Event, 0) > 0)
    {
        switch (Event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            Events.push_back({ .Kind = EEventKind::Connected, .Peer = State->IdOf(Event.peer) });
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
        {
            const uint32_t Id = State->IdOf(Event.peer);
            std::erase(State->Peers, Event.peer);
            Event.peer->data = nullptr;
            Events.push_back({ .Kind = EEventKind::Disconnected, .Peer = Id });
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
        {
            FEvent Received{ .Kind = EEventKind::Received, .Peer = State->IdOf(Event.peer) };
            Received.Bytes.assign(Event.packet->data, Event.packet->data + Event.packet->dataLength);
            enet_packet_destroy(Event.packet);
            Events.push_back(std::move(Received));
            break;
        }
        default:
            break;
        }
    }

    return Events;
}

void FEndpoint::Send(const uint32_t Peer, const std::span<const uint8_t> Bytes, const EChannel Channel)
{
    ENetPeer* const Target = State->Host ? State->PeerOf(Peer) : nullptr;
    if (!Target)
    {
        return;
    }

    const uint32_t Flags = Channel == EChannel::Reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    if (ENetPacket* const Packet = enet_packet_create(Bytes.data(), Bytes.size(), Flags))
    {
        enet_peer_send(Target, uint8_t(Channel), Packet);
    }
}

void FEndpoint::SendToAll(const std::span<const uint8_t> Bytes, const EChannel Channel)
{
    if (!State->Host)
    {
        return;
    }

    const uint32_t Flags = Channel == EChannel::Reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    if (ENetPacket* const Packet = enet_packet_create(Bytes.data(), Bytes.size(), Flags))
    {
        enet_host_broadcast(State->Host, uint8_t(Channel), Packet);
    }
}

void FEndpoint::Close()
{
    if (State->Host)
    {
        for (ENetPeer* const Peer : State->Peers)
        {
            enet_peer_disconnect(Peer, 0);
        }

        // The goodbyes go out with the next service; the host then forgets the peers.
        ENetEvent Event{};
        for (uint32_t Waited = 0; Waited < CloseMilliseconds && enet_host_service(State->Host, &Event, 1) >= 0; ++Waited)
        {
            if (Event.type == ENET_EVENT_TYPE_RECEIVE)
            {
                enet_packet_destroy(Event.packet);
            }
        }

        enet_host_destroy(State->Host);
        State->Host = nullptr;
    }

    State->Peers.clear();
    State->bHost = false;
    if (State->bAcquired)
    {
        ReleaseENet();
        State->bAcquired = false;
    }
}

void FWriter::PutString(const std::string_view Text)
{
    const uint32_t Size = uint32_t(std::min<size_t>(Text.size(), MaxStringBytes));
    Put(Size);
    Bytes.insert(Bytes.end(), Text.begin(), Text.begin() + Size);
}

std::string FReader::GetString()
{
    const uint32_t Size = Get<uint32_t>();
    if (Size > MaxStringBytes || At + Size > Bytes.size())
    {
        bFailed = true;
        return {};
    }

    std::string Text(reinterpret_cast<const char*>(Bytes.data() + At), Size);
    At += Size;
    return Text;
}

std::optional<std::pair<std::string, uint16_t>> ParseAddress(const std::string_view Text)
{
    if (Text.empty())
    {
        return std::nullopt;
    }

    const size_t Colon = Text.rfind(':');
    if (Colon == std::string_view::npos)
    {
        return std::pair(std::string(Text), DefaultPort);
    }

    uint16_t Port = 0;
    const std::string_view PortText = Text.substr(Colon + 1);
    if (Colon == 0 || std::from_chars(PortText.data(), PortText.data() + PortText.size(), Port).ec != std::errc{} || Port == 0)
    {
        return std::nullopt;
    }

    return std::pair(std::string(Text.substr(0, Colon)), Port);
}
}
