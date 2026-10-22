#pragma once

#include "Game/Engine.h"
#include "Game/Message.h"
#include "Net/Net.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The port's co-op over the network (Coop.md): the host runs the level and sends what it is; each client moves its own ship and sends that.
// Slot 0 is the host's player, 1 to 3 the clients'.
inline constexpr uint32_t CoopProtocolVersion = 3;
inline constexpr uint8_t HostSlot = 0;

// A ship as its owner moves it, and whether it fires and with which weapon slot.
struct FCoopShip
{
    uint8_t Slot = 0;
    FCollisionPoint Position;
    FCollisionPoint Velocity;
    float Speed = 0.f;
    float SpeedMagnitude = 0.f;
    int32_t Direction = 0;
    int32_t Pose = 1;
    int32_t TargetPose = 1;
    uint32_t ReactorMilliseconds = 0;
    bool bFire = false;
    bool bThrust = false;
    uint8_t WeaponSlot = 1;
};

// A player in the host's snapshot: its ship, its numbers and whether it is down or dead.
struct FCoopPlayer
{
    FCoopShip Ship;
    std::string Name;
    float Energy = 0.f;
    float EnergyMax = 0.f;
    float Shield = 0.f;
    float ShieldMax = 0.f;
    bool bDown = false;
    bool bDead = false;
    // How many pickups the player has collected in the level.
    uint32_t Collected = 0;
    std::vector<float> Ammo;
};

// A body in the host's snapshot, by the host's object index.
struct FCoopBody
{
    uint32_t Object = 0;
    FCollisionPoint Position;
    int32_t Direction = 0;
    int32_t Pose = 1;
    int32_t TopPose = 0;
    int32_t TurretDirection = 0;
    float Energy = 0.f;
    float Shield = 0.f;
    bool bInvisible = false;
    bool bPassable = false;
};

struct FCoopKey
{
    int32_t Id = 0;
    bool bCode = false;
    int32_t PartId = 0;
    int32_t PartCount = 0;
    std::string CodeString;
};

// A level object the host created during the level, which the clients create too under their own index.
struct FCoopCreated
{
    uint32_t Object = 0;
    std::string ClassName;
    FCollisionPoint Position;
};

struct FCoopSnapshot
{
    uint32_t Clock = 0;
    float Money = 0.f;
    float Earned = 0.f;
    std::vector<FCoopKey> Keys;
    std::vector<FCoopPlayer> Players;
    std::vector<FCoopBody> Bodies;
    // One bit per host object: removed, and wrecked.
    std::vector<uint8_t> Removed;
    std::vector<uint8_t> Wrecked;
    // The packs every player has their own of, with the slots that took them.
    std::vector<std::pair<uint32_t, uint8_t>> Takers;
};

// Who is in the game, by slot.
struct FCoopMember
{
    uint8_t Slot = 0;
    std::string Name;
};

class FCoopLink
{
public:
    static std::unique_ptr<FCoopLink> Host(uint16_t Port, std::string_view Name);
    static std::unique_ptr<FCoopLink> Join(std::string_view Address, uint16_t Port, std::string_view Name);

    bool IsHost() const { return bHost; }
    uint8_t GetLocalSlot() const { return LocalSlot; }
    // Whether a client has its slot from the host; a host always has.
    bool IsJoined() const { return bHost || bWelcomed; }
    bool IsLost() const { return bLost; }
    const std::vector<FCoopMember>& GetMembers() const { return Members; }

    // Takes in everything that arrived.
    void Poll();

    // Host: the level everyone plays now, which every client (re)starts, and whoever joins later too.
    void AnnounceLevel(std::string_view LevelPath);
    // Client: a level the host announced since the last call.
    std::optional<std::string> TakeLevel();
    bool HasLevel() const { return PendingLevel.has_value(); }

    // Client: its own ship. Host: the latest each client sent since the last call, by slot.
    void SendShip(const FCoopShip& Ship);
    std::optional<FCoopShip> TakeShip(uint8_t Slot);

    // Host: the world now, and the objects it created since the last snapshot. Client: the latest snapshot, once, and every created object since.
    void SendSnapshot(const FCoopSnapshot& Snapshot);
    void SendCreated(const std::vector<FCoopCreated>& Created);
    std::optional<FCoopSnapshot> TakeSnapshot();
    std::vector<FCoopCreated> TakeCreated();

    // Host: the messages a client's player is to see, which that client shows. Client: the messages sent to it since the last call.
    void SendMessages(uint8_t Slot, std::span<const std::pair<FMessage, EMessagePlacement>> Messages);
    std::vector<std::pair<FMessage, EMessagePlacement>> TakeMessages();

    // Host: members who joined or left since the last call.
    std::vector<uint8_t> TakeJoined();
    std::vector<uint8_t> TakeLeft();

private:
    FCoopLink() = default;
    void Receive(uint32_t Peer, std::span<const uint8_t> Bytes);
    std::optional<uint8_t> SlotOf(uint32_t Peer) const;
    void SendCreatedTo(std::optional<uint32_t> Peer, const std::vector<FCoopCreated>& Created);

    Net::FEndpoint Endpoint;
    // A host answers LAN searches for as long as it hosts.
    Net::FDiscovery Discovery;
    bool bHost = false;
    bool bWelcomed = false;
    bool bLost = false;
    uint8_t LocalSlot = HostSlot;
    std::string LocalName;
    std::string CurrentLevel;
    std::vector<FCoopMember> Members;
    std::vector<std::pair<uint32_t, uint8_t>> PeerSlots;
    std::vector<std::optional<FCoopShip>> Ships = std::vector<std::optional<FCoopShip>>(Net::MaxPeers + 1);
    std::optional<std::string> PendingLevel;
    std::optional<FCoopSnapshot> PendingSnapshot;
    std::vector<FCoopCreated> PendingCreated;
    std::vector<std::pair<FMessage, EMessagePlacement>> PendingMessages;
    // Host: every object created in the current level, for whoever joins during it.
    std::vector<FCoopCreated> AllCreated;
    std::vector<uint8_t> Joined;
    std::vector<uint8_t> Left;
};
