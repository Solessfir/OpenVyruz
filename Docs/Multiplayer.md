# Multiplayer

The multiplayer build is a separate executable, `Vyruz-mp.exe` (built Dec 10 2000), with its own archive `vyruz-full-mp.rsc`. It shares the engine with `Vyruz.exe` (the same `pg\lib` and `VzEd` sources, plus `mp-receive.cpp`) and adds a network layer, `PgNET`, over Winsock 2. The port has none of it yet; this page records what the binary shows so far, as the base for the [roadmap](Roadmap.md) item.

## Transport

`PgNET` sends and receives UDP datagrams (`sendto` in `PgNetSendTo`, `00456a90`; `recvfrom` in `PgNetReceive`, `00457130`) over TCP/IP; IPX is named but unimplemented (`GetHostAddr: IPX unimplemented.`). Servers are found by broadcast (`PgNET::GetBroadcastAddr`, `Looking for server ...`), and a `lan-only` setting keeps the search on the LAN. Besides the LAN there is a registration service reached over HTTP on port 80 (`Host: %s:80`, `adduser`, `deluser`, `update`), and a `com-port` setting.

Every packet starts with the 3-byte magic `25 12 66` (`%\x12f`), then the message type, then the sender's player number (1 to 8), then the payload; a packet holds at most 162 bytes (`0xa2`).

## Authority

There is no lockstep. A server (`DAT_0053acb0` = 2) accepts the players and runs the join messages; a client is 1. Each machine simulates its own ship and broadcasts its state, and every other machine moves that player's object as a puppet:

- `MpSendPlayerState` (`00404b60`) runs each frame. It sends the position (type `0x17`) whenever it changed, along with `0x19` and `0x18`, and the weapon (`0x27`).
- Held states go as pairs of start and stop messages (`0x1a`/`0x1b`, `0x1c`/`0x1d`, `0x1f`/`0x20`, `0x21`/`0x22`, `0x23`/`0x24`, `0x25`/`0x26`, `0x28`/`0x29`). A start is sent again every 300 ms while the state lasts, so a lost packet only delays it, and the receiver lets a state lapse without its refresh.
- A ship that dies reports it itself (type `0x2b`, with the killer's player number); the killer's machine counts the frag. With `team-mode` a team kill counts down. Reaching `frag-limit` ends the level and moves everyone on to the next one (type `0x15`, unless `always-same-level`).
- Destructible objects that changed are resent every 400 ms until confirmed (`00405910`).
- With no other traffic a keep-alive (type `7`) goes out every 2 seconds.

`MpReceivePackets` (`00403500`) drains the socket each frame and dispatches by type. Each player has a record of `0x68` bytes (`DAT_0053a908 + n * 0x68`) holding a flag per message type received (`MpPeerMarkReceived`, `004033f0`; `MpPeerClearReceived`, `00403420`) and the time each held state was last refreshed. `MpReceivePlayerPosition` (`00403fc0`) moves a remote ship to the reported position and bounding box and refiles it in the world grid.

## Settings

The multiplayer `vzconf.txt` (`# Vyruz Multiplayer configuration.`) adds `server-name`, `frag-limit`, `team-mode`, `friendly-fire`, `always-same-level`, `allow-nmy` (enemies in the level), `visible-player-names`, `lan-only` and `com-port`, and the actions `action-display-frags`, `action-multiplayer-chat` and `action-multiplayer-team-chat`. Bonus and malus surprises are sent to the others too (`SEND bonus/malus START`).

## Errors

The join can fail with: `Current map is forbidden`, `Server and client maps differ`, `Client and server protocol releases differ.`, `Cannot accept more player`, `Client name already used`, `Registered user already present`, `Invalid server ID`, `No answer from client` and `No answer from server`.

## Still open

The join handshake (types 1 to 6 and `0xc`, `0xd`), the payload layout of each type, the chat, how the levels in `vyruz-full-mp.rsc` differ from the campaign's, how `allow-nmy` enemies are kept in step between machines, and the score screen.
