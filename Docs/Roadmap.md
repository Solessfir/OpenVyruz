# Roadmap

Version 1, the single-player campaign with the intro and cutscenes, is done. Planned beyond it:

- **LAN multiplayer over ENet.** The original's own multiplayer, `Vyruz-mp.exe`, is being mapped in [Multiplayer.md](Multiplayer.md): up to 8 players, frags and teams, each machine owning its own ship and broadcasting its state. The port can follow that model, or run in lockstep the way the demo replay does (peers exchange each step's input commands and its length, every machine runs the same simulation, and a periodic state hash catches desyncs), which would keep enemies in step too. ENet carries the reliable and unreliable channels, and a UDP broadcast finds hosts on the LAN. Talking to the original's own clients is not a goal.
