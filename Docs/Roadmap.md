# Roadmap

Version 1, the single-player campaign with the intro and cutscenes, is done. Planned beyond it:

- **LAN multiplayer over ENet.** First find in Ghidra what the original's multiplayer was: its modes, levels, player count and how players joined. Then run it in lockstep, the way the demo replay already does: peers exchange each step's input commands and its length, every machine runs the same simulation, and a periodic state hash catches desyncs. ENet carries the reliable and unreliable channels, and a UDP broadcast finds hosts on the LAN. Talking to the original's own clients is not a goal.
