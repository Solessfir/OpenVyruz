# Roadmap

Version 1, the single-player campaign with the intro and cutscenes, is done. Planned beyond it:

- **Co-op campaign over ENet**, for 2 to 4 players; no deathmatch. Playable: the rules, the network, the Cooperative menu with its lobby and the LAN search are in and described in [Coop.md](Coop.md). The original's own multiplayer ([Multiplayer.md](Multiplayer.md)) is a reference only, not a model to match.
  - **Netcode:** the host runs the real simulation (enemies, shots, pickups, the random generator); clients send their input, predict their own ship and take the host's world snapshots. Lockstep is out: it needs bit-identical floats across the Windows and Linux builds and breaks with drop-in. A solo game keeps the original's rules untouched.
  - **Hosting:** the main menu gets a Co-op item with Host and Join. Host picks a new or saved session, listens on one UDP port and opens a lobby that lists who has joined; the host starts the game from it. Join lists the LAN games a broadcast finds and takes an address for the rest. A friend can also join a game already running, arriving at the host's ship, and leave without ending it. Internet play needs that port forwarded, or a VPN such as Tailscale or ZeroTier; a relay or NAT punching can come later. `--host` and `--join <address>` on the command line skip the menus for testing.
  - **Texts:** the new items and messages have no text in the exe, so they get English and Russian entries of the port's own beside the exe's pairs in `Source/Game/Language.cpp`, marked as the port's; a text without a Russian entry shows in English, as `Localize` already does.
  - **Enemies:** health scales with the players, about `1 + 0.6 * (players - 1)`; damage stays, or rises at most 10 to 15% a player. Fire rate or scan range may rise a little so the pressure holds when the targets split.
  - **Pickups:** ammo, energy and shield packs are per player, so nobody races a friend for them; keys and codes unlock for everyone; money is shared.
  - **Death:** at zero energy a player is downed instead of exploding and a teammate standing close for about 2 seconds revives them. Unrevived after about 30 seconds, they respawn at the team's last saving plot. With everyone down at once the team reloads the last save.
  - **Lifts:** a lift waits until every standing player is in it.
  - **Saves:** the host's session is the game; saving plots save it for the team, and joining players play on the host's progress.
  - **Friendly fire:** none.
  - **To fix**, from a code review of the co-op work:
    - A waiting co-op lift charges its fare to the team purse, and uses up a limited lift, on every step a rider stays in it.
    - The hunters' co-op toughening is saved into sessions and applied again on load, so a retry or a co-op save makes them tougher each time.
    - A client's ship is made afresh on every level: its energy, shield, ammo and weapons are not carried over as the host's are.
    - The inventory and help screens, and the lobby's saved-game list, stop the network and freeze the other players, who can time out.
    - A surprise a client picks up is lost: the host's copy of that player never runs its surprises.
    - Snapshots over one packet are sent reliably (ENet fragments them without `ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT`), so they queue up behind lost packets.
    - A client respawned after staying down comes back where it fell: only the host's copy moves.
    - A client takes Level and Snapshot messages before the host has welcomed it, as after a refused Hello with another protocol version, and then plays as the host's slot; a refused client is not told why.
    - A snapshot of the previous level can arrive after the next level's start and remove objects there by index.
    - Closing the network link blocks for about 3 seconds on Windows.
    - The host's switch away from an empty weapon and the client's own slot can flip back and forth, which skips the weapon's delay.
    - Positions, poses and directions from the network are not range-checked.
    - Actors knock only the nearest ship, downed or not, and a turret locked onto a downed ship keeps firing at it.
    - The host hears its own voice lines for the other players' kills, hits and pickups.
