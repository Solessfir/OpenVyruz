#include "Game/Coop.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace
{
enum class EMessage : uint8_t
{
    Hello = 1,
    Welcome,
    Members,
    Level,
    Ship,
    Snapshot,
    Created,
};

// Lists in a message are bounded, so a bad count cannot make a reader allocate without end.
constexpr uint32_t MaxListEntries = 1u << 16;

void PutShip(Net::FWriter& Writer, const FCoopShip& Ship)
{
    Writer.Put(Ship.Slot);
    Writer.Put(Ship.Position.X);
    Writer.Put(Ship.Position.Y);
    Writer.Put(Ship.Velocity.X);
    Writer.Put(Ship.Velocity.Y);
    Writer.Put(Ship.Speed);
    Writer.Put(Ship.SpeedMagnitude);
    Writer.Put(int8_t(Ship.Direction));
    Writer.Put(uint8_t(Ship.Pose));
    Writer.Put(uint8_t(Ship.TargetPose));
    Writer.Put(Ship.ReactorMilliseconds);
    Writer.Put(uint8_t((Ship.bFire ? 1 : 0) | (Ship.bThrust ? 2 : 0)));
    Writer.Put(Ship.WeaponSlot);
}

FCoopShip ReadShip(Net::FReader& Reader)
{
    FCoopShip Ship;
    Ship.Slot = Reader.Get<uint8_t>();
    Ship.Position.X = Reader.Get<float>();
    Ship.Position.Y = Reader.Get<float>();
    Ship.Velocity.X = Reader.Get<float>();
    Ship.Velocity.Y = Reader.Get<float>();
    Ship.Speed = Reader.Get<float>();
    Ship.SpeedMagnitude = Reader.Get<float>();
    Ship.Direction = Reader.Get<int8_t>();
    Ship.Pose = Reader.Get<uint8_t>();
    Ship.TargetPose = Reader.Get<uint8_t>();
    Ship.ReactorMilliseconds = Reader.Get<uint32_t>();
    const uint8_t Flags = Reader.Get<uint8_t>();
    Ship.bFire = (Flags & 1) != 0;
    Ship.bThrust = (Flags & 2) != 0;
    Ship.WeaponSlot = Reader.Get<uint8_t>();
    return Ship;
}

uint32_t GetCount(Net::FReader& Reader)
{
    const uint32_t Count = Reader.Get<uint32_t>();
    return Count <= MaxListEntries ? Count : 0;
}

void PutBits(Net::FWriter& Writer, const std::vector<uint8_t>& Bits)
{
    Writer.Put(uint32_t(Bits.size()));
    for (const uint8_t Byte : Bits)
    {
        Writer.Put(Byte);
    }
}

std::vector<uint8_t> GetBits(Net::FReader& Reader)
{
    std::vector<uint8_t> Bits(GetCount(Reader));
    for (uint8_t& Byte : Bits)
    {
        Byte = Reader.Get<uint8_t>();
    }

    return Bits;
}
}

std::unique_ptr<FCoopLink> FCoopLink::Host(const uint16_t Port, const std::string_view Name)
{
    std::unique_ptr<FCoopLink> Link(new FCoopLink());
    if (!Link->Endpoint.Host(Port))
    {
        return nullptr;
    }

    Link->bHost = true;
    Link->LocalName = std::string(Name);
    Link->Discovery.Answer(Name);
    Link->Members.push_back({ .Slot = HostSlot, .Name = Link->LocalName });
    std::printf("Co-op: hosting on UDP port %u\n", unsigned(Port));
    return Link;
}

std::unique_ptr<FCoopLink> FCoopLink::Join(const std::string_view Address, const uint16_t Port, const std::string_view Name)
{
    std::unique_ptr<FCoopLink> Link(new FCoopLink());
    if (!Link->Endpoint.Connect(Address, Port))
    {
        return nullptr;
    }

    Link->LocalName = std::string(Name);
    std::printf("Co-op: joining %.*s:%u\n", int(Address.size()), Address.data(), unsigned(Port));
    return Link;
}

std::optional<uint8_t> FCoopLink::SlotOf(const uint32_t Peer) const
{
    const auto Found = std::ranges::find(PeerSlots, Peer, &std::pair<uint32_t, uint8_t>::first);
    return Found != PeerSlots.end() ? std::optional(Found->second) : std::nullopt;
}

void FCoopLink::Poll()
{
    Discovery.Poll(0);
    for (Net::FEvent& Event : Endpoint.Poll())
    {
        switch (Event.Kind)
        {
        case Net::EEventKind::Connected:
            if (!bHost)
            {
                Net::FWriter Writer;
                Writer.Put(EMessage::Hello);
                Writer.Put(CoopProtocolVersion);
                Writer.PutString(LocalName);
                Endpoint.Send(Event.Peer, Writer.Data(), Net::EChannel::Reliable);
            }

            break;
        case Net::EEventKind::Disconnected:
            if (!bHost)
            {
                bLost = true;
                std::printf("Co-op: the host is gone\n");
            }
            else if (const std::optional<uint8_t> Slot = SlotOf(Event.Peer))
            {
                std::erase_if(PeerSlots, [&](const auto& Entry) { return Entry.first == Event.Peer; });
                std::erase_if(Members, [&](const FCoopMember& Member) { return Member.Slot == *Slot; });
                Ships[*Slot].reset();
                Left.push_back(*Slot);
                std::printf("Co-op: player %u left\n", unsigned(*Slot));
            }

            break;
        case Net::EEventKind::Received:
            Receive(Event.Peer, Event.Bytes);
            break;
        }
    }
}

void FCoopLink::Receive(const uint32_t Peer, const std::span<const uint8_t> Bytes)
{
    Net::FReader Reader(Bytes);
    const EMessage Kind = Reader.Get<EMessage>();
    switch (Kind)
    {
    case EMessage::Hello:
    {
        const uint32_t Version = Reader.Get<uint32_t>();
        const std::string Name = Reader.GetString();
        if (!bHost || !Reader.IsGood() || Version != CoopProtocolVersion || SlotOf(Peer))
        {
            return;
        }

        uint8_t Slot = HostSlot + 1;
        while (std::ranges::contains(Members, Slot, &FCoopMember::Slot))
        {
            ++Slot;
        }

        if (Slot > Net::MaxPeers)
        {
            return;
        }

        PeerSlots.emplace_back(Peer, Slot);
        Members.push_back({ .Slot = Slot, .Name = Name });
        Joined.push_back(Slot);
        std::printf("Co-op: %s joined as player %u\n", Name.c_str(), unsigned(Slot));
        std::fflush(stdout);
        Net::FWriter Welcome;
        Welcome.Put(EMessage::Welcome);
        Welcome.Put(Slot);
        Endpoint.Send(Peer, Welcome.Data(), Net::EChannel::Reliable);
        // Everyone learns who is in, and the newcomer the level and what the host made in it so far.
        Net::FWriter List;
        List.Put(EMessage::Members);
        List.Put(uint32_t(Members.size()));
        for (const FCoopMember& Member : Members)
        {
            List.Put(Member.Slot);
            List.PutString(Member.Name);
        }

        Endpoint.SendToAll(List.Data(), Net::EChannel::Reliable);
        if (!CurrentLevel.empty())
        {
            Net::FWriter Level;
            Level.Put(EMessage::Level);
            Level.PutString(CurrentLevel);
            Endpoint.Send(Peer, Level.Data(), Net::EChannel::Reliable);
            SendCreatedTo(Peer, AllCreated);
        }

        break;
    }
    case EMessage::Welcome:
        LocalSlot = Reader.Get<uint8_t>();
        bWelcomed = Reader.IsGood() && LocalSlot != HostSlot && LocalSlot <= Net::MaxPeers;
        std::printf("Co-op: joined as player %u\n", unsigned(LocalSlot));
        break;
    case EMessage::Members:
    {
        std::vector<FCoopMember> List(GetCount(Reader));
        for (FCoopMember& Member : List)
        {
            Member.Slot = Reader.Get<uint8_t>();
            Member.Name = Reader.GetString();
        }

        if (!bHost && Reader.IsGood())
        {
            Members = std::move(List);
        }

        break;
    }
    case EMessage::Level:
    {
        std::string Path = Reader.GetString();
        if (!bHost && Reader.IsGood())
        {
            PendingLevel = std::move(Path);
            PendingCreated.clear();
            PendingSnapshot.reset();
        }

        break;
    }
    case EMessage::Ship:
    {
        FCoopShip Ship = ReadShip(Reader);
        const std::optional<uint8_t> Slot = SlotOf(Peer);
        // A client moves only its own ship.
        if (bHost && Reader.IsGood() && Slot)
        {
            Ship.Slot = *Slot;
            Ships[*Slot] = Ship;
        }

        break;
    }
    case EMessage::Snapshot:
    {
        FCoopSnapshot Snapshot;
        Snapshot.Clock = Reader.Get<uint32_t>();
        Snapshot.Money = Reader.Get<float>();
        Snapshot.Earned = Reader.Get<float>();
        Snapshot.Keys.resize(GetCount(Reader));
        for (FCoopKey& Key : Snapshot.Keys)
        {
            Key.Id = Reader.Get<int32_t>();
            Key.bCode = Reader.Get<uint8_t>() != 0;
            Key.PartId = Reader.Get<int32_t>();
            Key.PartCount = Reader.Get<int32_t>();
            Key.CodeString = Reader.GetString();
        }

        Snapshot.Players.resize(GetCount(Reader));
        for (FCoopPlayer& Player : Snapshot.Players)
        {
            Player.Ship = ReadShip(Reader);
            Player.Name = Reader.GetString();
            Player.Energy = Reader.Get<float>();
            Player.EnergyMax = Reader.Get<float>();
            Player.Shield = Reader.Get<float>();
            Player.ShieldMax = Reader.Get<float>();
            const uint8_t Flags = Reader.Get<uint8_t>();
            Player.bDown = (Flags & 1) != 0;
            Player.bDead = (Flags & 2) != 0;
            Player.Ammo.resize(GetCount(Reader));
            for (float& Count : Player.Ammo)
            {
                Count = Reader.Get<float>();
            }
        }

        Snapshot.Bodies.resize(GetCount(Reader));
        for (FCoopBody& Body : Snapshot.Bodies)
        {
            Body.Object = Reader.Get<uint32_t>();
            Body.Position.X = Reader.Get<float>();
            Body.Position.Y = Reader.Get<float>();
            Body.Direction = Reader.Get<int8_t>();
            Body.Pose = Reader.Get<uint8_t>();
            Body.TopPose = Reader.Get<uint8_t>();
            Body.TurretDirection = Reader.Get<int8_t>();
            Body.Energy = Reader.Get<float>();
            Body.Shield = Reader.Get<float>();
            const uint8_t Flags = Reader.Get<uint8_t>();
            Body.bInvisible = (Flags & 1) != 0;
            Body.bPassable = (Flags & 2) != 0;
        }

        Snapshot.Removed = GetBits(Reader);
        Snapshot.Wrecked = GetBits(Reader);
        Snapshot.Takers.resize(GetCount(Reader));
        for (auto& [Object, Slots] : Snapshot.Takers)
        {
            Object = Reader.Get<uint32_t>();
            Slots = Reader.Get<uint8_t>();
        }

        if (!bHost && Reader.IsGood())
        {
            PendingSnapshot = std::move(Snapshot);
        }

        break;
    }
    case EMessage::Created:
    {
        std::vector<FCoopCreated> Created(GetCount(Reader));
        for (FCoopCreated& Object : Created)
        {
            Object.Object = Reader.Get<uint32_t>();
            Object.ClassName = Reader.GetString();
            Object.Position.X = Reader.Get<float>();
            Object.Position.Y = Reader.Get<float>();
        }

        if (!bHost && Reader.IsGood())
        {
            PendingCreated.insert(PendingCreated.end(), Created.begin(), Created.end());
        }

        break;
    }
    }
}

void FCoopLink::AnnounceLevel(const std::string_view LevelPath)
{
    CurrentLevel = std::string(LevelPath);
    AllCreated.clear();
    Net::FWriter Writer;
    Writer.Put(EMessage::Level);
    Writer.PutString(CurrentLevel);
    Endpoint.SendToAll(Writer.Data(), Net::EChannel::Reliable);
}

std::optional<std::string> FCoopLink::TakeLevel()
{
    return std::exchange(PendingLevel, std::nullopt);
}

void FCoopLink::SendShip(const FCoopShip& Ship)
{
    Net::FWriter Writer;
    Writer.Put(EMessage::Ship);
    PutShip(Writer, Ship);
    Endpoint.SendToAll(Writer.Data(), Net::EChannel::Unreliable);
}

std::optional<FCoopShip> FCoopLink::TakeShip(const uint8_t Slot)
{
    return Slot < Ships.size() ? std::exchange(Ships[Slot], std::nullopt) : std::nullopt;
}

void FCoopLink::SendSnapshot(const FCoopSnapshot& Snapshot)
{
    Net::FWriter Writer;
    Writer.Put(EMessage::Snapshot);
    Writer.Put(Snapshot.Clock);
    Writer.Put(Snapshot.Money);
    Writer.Put(Snapshot.Earned);
    Writer.Put(uint32_t(Snapshot.Keys.size()));
    for (const FCoopKey& Key : Snapshot.Keys)
    {
        Writer.Put(Key.Id);
        Writer.Put(uint8_t(Key.bCode));
        Writer.Put(Key.PartId);
        Writer.Put(Key.PartCount);
        Writer.PutString(Key.CodeString);
    }

    Writer.Put(uint32_t(Snapshot.Players.size()));
    for (const FCoopPlayer& Player : Snapshot.Players)
    {
        PutShip(Writer, Player.Ship);
        Writer.PutString(Player.Name);
        Writer.Put(Player.Energy);
        Writer.Put(Player.EnergyMax);
        Writer.Put(Player.Shield);
        Writer.Put(Player.ShieldMax);
        Writer.Put(uint8_t((Player.bDown ? 1 : 0) | (Player.bDead ? 2 : 0)));
        Writer.Put(uint32_t(Player.Ammo.size()));
        for (const float Count : Player.Ammo)
        {
            Writer.Put(Count);
        }
    }

    Writer.Put(uint32_t(Snapshot.Bodies.size()));
    for (const FCoopBody& Body : Snapshot.Bodies)
    {
        Writer.Put(Body.Object);
        Writer.Put(Body.Position.X);
        Writer.Put(Body.Position.Y);
        Writer.Put(int8_t(Body.Direction));
        Writer.Put(uint8_t(Body.Pose));
        Writer.Put(uint8_t(Body.TopPose));
        Writer.Put(int8_t(Body.TurretDirection));
        Writer.Put(Body.Energy);
        Writer.Put(Body.Shield);
        Writer.Put(uint8_t((Body.bInvisible ? 1 : 0) | (Body.bPassable ? 2 : 0)));
    }

    PutBits(Writer, Snapshot.Removed);
    PutBits(Writer, Snapshot.Wrecked);
    Writer.Put(uint32_t(Snapshot.Takers.size()));
    for (const auto& [Object, Slots] : Snapshot.Takers)
    {
        Writer.Put(Object);
        Writer.Put(Slots);
    }

    Endpoint.SendToAll(Writer.Data(), Net::EChannel::Unreliable);
}

void FCoopLink::SendCreated(const std::vector<FCoopCreated>& Created)
{
    AllCreated.insert(AllCreated.end(), Created.begin(), Created.end());
    SendCreatedTo(std::nullopt, Created);
}

void FCoopLink::SendCreatedTo(const std::optional<uint32_t> Peer, const std::vector<FCoopCreated>& Created)
{
    if (Created.empty())
    {
        return;
    }

    Net::FWriter Writer;
    Writer.Put(EMessage::Created);
    Writer.Put(uint32_t(Created.size()));
    for (const FCoopCreated& Object : Created)
    {
        Writer.Put(Object.Object);
        Writer.PutString(Object.ClassName);
        Writer.Put(Object.Position.X);
        Writer.Put(Object.Position.Y);
    }

    if (Peer)
    {
        Endpoint.Send(*Peer, Writer.Data(), Net::EChannel::Reliable);
    }
    else
    {
        Endpoint.SendToAll(Writer.Data(), Net::EChannel::Reliable);
    }
}

std::optional<FCoopSnapshot> FCoopLink::TakeSnapshot()
{
    return std::exchange(PendingSnapshot, std::nullopt);
}

std::vector<FCoopCreated> FCoopLink::TakeCreated()
{
    return std::exchange(PendingCreated, {});
}

std::vector<uint8_t> FCoopLink::TakeJoined()
{
    return std::exchange(Joined, {});
}

std::vector<uint8_t> FCoopLink::TakeLeft()
{
    return std::exchange(Left, {});
}
