#include "Game/Coop.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
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
    Messages,
    Pause,
    // Keeps its number and form in every protocol version, since it answers a client of another one.
    Refused = 0x80,
};

// Why the host would not have a client.
enum class ERefusal : uint8_t
{
    OtherVersion = 1,
    Full,
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
    Writer.Put(uint8_t((Ship.bFire ? 1 : 0) | (Ship.bThrust ? 2 : 0) | (Ship.bInvisible ? 4 : 0)));
    Writer.Put(Ship.WeaponSlot);
    Writer.Put(Ship.Respawns);
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
    Ship.bInvisible = (Flags & 4) != 0;
    Ship.WeaponSlot = Reader.Get<uint8_t>();
    Ship.Respawns = Reader.Get<uint32_t>();
    return Ship;
}

void PutLevel(Net::FWriter& Writer, const uint32_t Serial, const std::string& Path, const std::optional<FCoopSession>& Session)
{
    Writer.Put(EMessage::Level);
    Writer.Put(Serial);
    Writer.PutString(Path);
    Writer.Put(uint8_t(Session ? 1 : 0));
    if (Session)
    {
        Writer.PutString(Session->LastText, Net::MaxFileBytes);
        Writer.PutString(Session->Objects, Net::MaxFileBytes);
    }
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
        // A client of another version, whose Hello may go on differently, or one past the last slot is told why it cannot join.
        const auto Refuse = [&](const ERefusal Reason)
        {
            Net::FWriter Refusal;
            Refusal.Put(EMessage::Refused);
            Refusal.Put(Reason);
            Endpoint.Send(Peer, Refusal.Data(), Net::EChannel::Reliable);
        };
        const uint32_t Version = Reader.Get<uint32_t>();
        if (!bHost || !Reader.IsGood() || SlotOf(Peer))
        {
            return;
        }

        if (Version != CoopProtocolVersion)
        {
            Refuse(ERefusal::OtherVersion);
            return;
        }

        const std::string Name = Reader.GetString();
        if (!Reader.IsGood())
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
            Refuse(ERefusal::Full);
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
            PutLevel(Level, LevelSerial, CurrentLevel, LevelSession);
            Endpoint.Send(Peer, Level.Data(), Net::EChannel::Reliable);
            SendCreatedTo(Peer, AllCreated);
        }

        break;
    }
    case EMessage::Welcome:
        LocalSlot = Reader.Get<uint8_t>();
        bWelcomed = !bHost && Reader.IsGood() && LocalSlot != HostSlot && LocalSlot <= Net::MaxPeers;
        std::printf("Co-op: joined as player %u\n", unsigned(LocalSlot));
        break;
    case EMessage::Refused:
    {
        const ERefusal Reason = Reader.Get<ERefusal>();
        if (!bHost && !bWelcomed)
        {
            Refusal = Reason == ERefusal::Full ? "The game is full." : "The host plays another version.";
            bLost = true;
            std::printf("Co-op: refused, %s\n", Refusal.c_str());
            std::fflush(stdout);
        }

        break;
    }
    case EMessage::Members:
    {
        std::vector<FCoopMember> List(GetCount(Reader));
        for (FCoopMember& Member : List)
        {
            Member.Slot = Reader.Get<uint8_t>();
            Member.Name = Reader.GetString();
        }

        if (!bHost && bWelcomed && Reader.IsGood())
        {
            Members = std::move(List);
        }

        break;
    }
    case EMessage::Level:
    {
        const uint32_t Serial = Reader.Get<uint32_t>();
        std::string Path = Reader.GetString();
        std::optional<FCoopSession> Session;
        if (Reader.Get<uint8_t>() != 0)
        {
            Session.emplace();
            Session->LastText = Reader.GetString(Net::MaxFileBytes);
            Session->Objects = Reader.GetString(Net::MaxFileBytes);
        }

        if (!bHost && bWelcomed && Reader.IsGood())
        {
            LevelSession = std::move(Session);
            PendingLevel = std::move(Path);
            PendingSerial = Serial;
            PendingCreated.clear();
            PendingSnapshot.reset();
            PendingMessages.clear();
        }

        break;
    }
    case EMessage::Ship:
    {
        const uint32_t Serial = Reader.Get<uint32_t>();
        FCoopShip Ship = ReadShip(Reader);
        const std::optional<uint8_t> Slot = SlotOf(Peer);
        // A client moves only its own ship, and only in the level it plays now.
        if (bHost && Reader.IsGood() && Slot && Serial == LevelSerial)
        {
            Ship.Slot = *Slot;
            Ships[*Slot] = Ship;
        }

        break;
    }
    case EMessage::Snapshot:
    {
        const uint32_t Serial = Reader.Get<uint32_t>();
        FCoopSnapshot Snapshot;
        Snapshot.Clock = Reader.Get<uint32_t>();
        Snapshot.bPaused = Reader.Get<uint8_t>() != 0;
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
            Player.Collected = Reader.Get<uint32_t>();
            Player.Surprises = Reader.Get<uint32_t>();
            Player.Surprise = Reader.Get<uint8_t>();
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

        // A snapshot comes unreliably and may overtake the level's change: only one of the level the game plays now applies.
        if (!bHost && bWelcomed && Reader.IsGood() && Serial == LevelSerial && !PendingLevel)
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

        if (!bHost && bWelcomed && Reader.IsGood())
        {
            PendingCreated.insert(PendingCreated.end(), Created.begin(), Created.end());
        }

        break;
    }
    case EMessage::Pause:
        PauseRequests += bHost && SlotOf(Peer) ? 1 : 0;
        break;
    case EMessage::Messages:
    {
        std::vector<std::pair<FMessage, EMessagePlacement>> Messages(GetCount(Reader));
        for (auto& [Message, Placement] : Messages)
        {
            Message.Text = Reader.GetString();
            Message.X = Reader.Get<float>();
            Message.Y = Reader.Get<float>();
            Message.VelocityX = Reader.Get<float>();
            Message.VelocityY = Reader.Get<float>();
            Message.LifeMilliseconds = Reader.Get<uint32_t>();
            const uint8_t Flags = Reader.Get<uint8_t>();
            Message.bWorld = (Flags & 1) != 0;
            Message.bTypewriter = (Flags & 2) != 0;
            Message.bFade = (Flags & 4) != 0;
            Message.Gaf = EMessageGaf(std::min<uint8_t>(Reader.Get<uint8_t>(), uint8_t(EMessageGaf::Surprise)));
            Message.Font = Reader.Get<uint32_t>();
            Message.Color.Alpha = Reader.Get<uint32_t>();
            Message.Color.Diffuse = Reader.Get<uint32_t>();
            Message.Color.Specular = Reader.Get<uint32_t>();
            Message.BlinkPeriod = Reader.Get<int32_t>();
            Placement = EMessagePlacement(std::min<uint8_t>(Reader.Get<uint8_t>(), uint8_t(EMessagePlacement::KeepExisting)));
        }

        if (!bHost && bWelcomed && Reader.IsGood())
        {
            PendingMessages.insert(PendingMessages.end(), std::make_move_iterator(Messages.begin()), std::make_move_iterator(Messages.end()));
        }

        break;
    }
    }
}

void FCoopLink::AnnounceLevel(const std::string_view LevelPath, std::optional<FCoopSession> Session)
{
    CurrentLevel = std::string(LevelPath);
    LevelSession = std::move(Session);
    AllCreated.clear();
    ++LevelSerial;
    Net::FWriter Writer;
    PutLevel(Writer, LevelSerial, CurrentLevel, LevelSession);
    Endpoint.SendToAll(Writer.Data(), Net::EChannel::Reliable);
}

std::optional<std::string> FCoopLink::TakeLevel()
{
    LevelSerial = PendingLevel ? PendingSerial : LevelSerial;
    return std::exchange(PendingLevel, std::nullopt);
}

void FCoopLink::SendShip(const FCoopShip& Ship)
{
    Net::FWriter Writer;
    Writer.Put(EMessage::Ship);
    Writer.Put(LevelSerial);
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
    Writer.Put(LevelSerial);
    Writer.Put(Snapshot.Clock);
    Writer.Put(uint8_t(Snapshot.bPaused ? 1 : 0));
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
        Writer.Put(Player.Collected);
        Writer.Put(Player.Surprises);
        Writer.Put(Player.Surprise);
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

void FCoopLink::SendMessages(const uint8_t Slot, const std::span<const std::pair<FMessage, EMessagePlacement>> Messages)
{
    const auto Peer = std::ranges::find(PeerSlots, Slot, &std::pair<uint32_t, uint8_t>::second);
    if (Peer == PeerSlots.end())
    {
        return;
    }

    // An animation message, a surprise's icon, does not travel: the client's own run of the surprise shows it.
    const auto IsText = [](const std::pair<FMessage, EMessagePlacement>& Entry) { return !Entry.first.Animation; };
    const uint32_t Count = uint32_t(std::ranges::count_if(Messages, IsText));
    if (Count == 0)
    {
        return;
    }

    Net::FWriter Writer;
    Writer.Put(EMessage::Messages);
    Writer.Put(Count);
    for (const auto& [Message, Placement] : Messages)
    {
        if (Message.Animation)
        {
            continue;
        }

        Writer.PutString(Message.Text);
        Writer.Put(Message.X);
        Writer.Put(Message.Y);
        Writer.Put(Message.VelocityX);
        Writer.Put(Message.VelocityY);
        Writer.Put(Message.LifeMilliseconds);
        Writer.Put(uint8_t((Message.bWorld ? 1 : 0) | (Message.bTypewriter ? 2 : 0) | (Message.bFade ? 4 : 0)));
        Writer.Put(uint8_t(Message.Gaf));
        Writer.Put(Message.Font);
        Writer.Put(Message.Color.Alpha);
        Writer.Put(Message.Color.Diffuse);
        Writer.Put(Message.Color.Specular);
        Writer.Put(Message.BlinkPeriod);
        Writer.Put(uint8_t(Placement));
    }

    Endpoint.Send(Peer->first, Writer.Data(), Net::EChannel::Reliable);
}

void FCoopLink::RequestPause()
{
    Net::FWriter Writer;
    Writer.Put(EMessage::Pause);
    Endpoint.SendToAll(Writer.Data(), Net::EChannel::Reliable);
}

std::vector<std::pair<FMessage, EMessagePlacement>> FCoopLink::TakeMessages()
{
    return std::exchange(PendingMessages, {});
}

std::optional<FCoopSnapshot> FCoopLink::TakeSnapshot()
{
    return std::exchange(PendingSnapshot, std::nullopt);
}

std::vector<FCoopCreated> FCoopLink::TakeCreated()
{
    // Objects made after a level change the game has yet to take belong to the next level.
    return PendingLevel ? std::vector<FCoopCreated>() : std::exchange(PendingCreated, {});
}

std::vector<uint8_t> FCoopLink::TakeJoined()
{
    return std::exchange(Joined, {});
}

std::vector<uint8_t> FCoopLink::TakeLeft()
{
    return std::exchange(Left, {});
}
