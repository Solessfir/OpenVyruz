# Co-op

The port's co-op campaign, which the original does not have: up to four players through the single-player levels. Its multiplayer build ([Multiplayer.md](Multiplayer.md)) is a reference only. A level with one player plays by the original's rules, untouched; everything below applies from the second player on. The [roadmap](Roadmap.md) has what is still to come.

## In the level

Every player is a level object of the player's class with its own ship, inventory, weapons, surprises and death, all stepped by the same code as a lone player (`FLevelPlayer` in `Source/Main.cpp`). The level's bodies, actors, doors, generators, turrets and shots step once a step and meet every player (`FPlayerTargets` in `Source/Game/Engine.h`):

- **Enemies:** a seeker or tracker hunts the nearest player its sensor finds and keeps it while it can; a turret locks onto one player at a time; an actor's or a drifting body's knock hits the player nearest it. A downed player is left alone.
- **Shots and blasts:** another object's shot can strike any player, a player's shot strikes no player, and a player's blast, from a shot or from its death, hurts that player and the enemies but spares the others: there is no friendly fire. Hits, misses, kills and the fast-kill window belong to the player who fired.
- **Toughness:** the hunters' energy, shield and shield maximum are multiplied by `1 + 0.6 * (players - 1)` (`CoopHunterScale`), as the second player arrives and for every hunter born later.

## The team

- **Money, keys and codes** are the team's, held by the first player's inventory (`FPlayerTarget::Team`, `PurseOf`): pickups, rewards, bonuses, a lift's fare and the map all draw on the one purse, and a key or code any player picks up opens its doors for everyone.
- **Packs** of energy, shield or ammo are every player's own: a pack stays in the level until each player has taken it, hidden only for those who have, and any money in it goes to the purse once. Money alone, keys, codes and surprises go to whoever takes them first.

## Down and back

At zero energy a player is down instead of exploding: its ship coasts to a stop and pulses red, takes no command, fires nothing, collects nothing and is ignored by the enemies. A standing teammate within 100 pixels for two seconds brings it back at 30% energy. Unrevived after 30 seconds, it comes back by itself at half energy where the team last saved, or where the level started. With every player down at once the team is lost: each ship dies as a lone player's does and the game ends.

## Lifts

The first player into a lift starts a 10-second countdown; the level changes when it runs out, or at once when every standing player is inside the lift.

## Over the network

`--host` listens on UDP port 47210 and plays as usual; `--join <address>` connects to it (`Source/Net/Net.cpp` over ENet, `Source/Game/Coop.cpp` for the messages). Up to three players join, and anyone may join or leave during a level: a newcomer's ship appears beside the host's, and the hunters toughen or soften with the count.

The host's game is the game. Its level, enemies, shots, pickups, inventories, purse, downs and level changes are the truth, which it sends about twenty times a second: every player, every body, the objects removed or wrecked, the packs' takers, the team's money and keys, and, reliably, every object it created, which a client creates too under its own index. Each client moves its own ship and sends it with its fire and weapon some sixty times a second, so its own steering has no lag; the host takes that ship as it comes. Between snapshots a client steps its world as a lone game would, but as a puppet (`SetPuppetWorld`): it deals no damage, gives no birth, drops nothing, decides no down or death, collects nothing, saves nothing and takes no lift of its own; its shots and blasts are for show, and a body dies when the host says so. A client follows the host to every level it starts, including a retry, and waits again when the host's game ends.

A network game never pauses for a window out of focus. A host should start a new game: a saved one, or a session's retry from the main menu, is not shared yet.

## Trying it

`--coopDummy` ([Tools.md](Tools.md#development-aids)) adds a second ship that follows the player, which is enough to see the rules without a second machine. Two copies of the game on one machine, one with `--host` and one with `--join 127.0.0.1`, try the network.
