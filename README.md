# OpenVyruz

Native reimplementation of *Vyruz: Destruction of the Untel Empire* (2001, Metropolis Software / C2V, "pg" engine) for Windows 11 and Linux (Wayland/X11). It reads the original game data; no game files are included.

C++23, Premake5, GLFW, Vulkan 1.3, Slang shaders.

## Building

Clone with submodules (GLFW), or fetch them afterwards. miniaudio is vendored as its single header in `External/miniaudio`.

```bash
git clone --recursive https://github.com/Solessfir/OpenVyruz.git
git submodule update --init    # if cloned without --recursive
```

The project generators download a pinned Premake on first run. Shaders are Slang (`Source/Shaders/*.slang`), compiled by `slangc` at build time into SPIR-V arrays embedded in the executable.

Configurations: `Debug`, `Development` (optimized with symbols), `Shipping`.

### Windows

Requirements:

- Visual Studio 2026 or its Build Tools, with the "Desktop development with C++" workload
- Vulkan SDK 1.3 or newer, which provides `slangc`. `VULKAN_SDK` is used when set; otherwise the generator falls back to `%VULKAN_SDK%`

```powershell
.\GenerateProjectFiles.bat vs2026
msbuild Vyruz.slnx /m /t:Vyruz /p:Configuration=Development /p:Platform=x64
```

The executable is written to `Binaries\windows\Development\Vyruz.exe`. You can also open `Vyruz.slnx` in Visual Studio or Rider.

### Linux

Requirements: GCC 14+ or Clang 18+, GNU Make, `slangc`, Vulkan headers and loader, and the Wayland and X11 development packages (`wayland-scanner` included). On Arch:

```bash
sudo pacman -Syu --needed base-devel vulkan-headers vulkan-icd-loader shader-slang wayland libxkbcommon libx11 libxrandr libxinerama libxcursor libxi
```

```bash
./GenerateProjectFiles.sh gmake               # add --cc=clang to build with Clang
make config=development Vyruz -j"$(nproc)"
```

The executable is written to `Binaries/linux/Development/Vyruz`.

On Arch, `Packaging/Arch/PKGBUILD` builds the `openvyruz-git` package from the latest `main`: it installs `/usr/bin/openvyruz` with its desktop entry and icon. On the first run it asks for the game folder (the one holding `vz-data.rsc`), or pass it once: `openvyruz <path to vz-data.rsc>`.

```bash
cd Packaging/Arch && makepkg -si
```

Otherwise, the window's app id is `openvyruz`, so the compositor shows the icon once the desktop entry and icon are installed:

```bash
install -Dm644 Resources/Linux/openvyruz.desktop ~/.local/share/applications/openvyruz.desktop
install -Dm644 Resources/Linux/openvyruz.png ~/.local/share/icons/hicolor/256x256/apps/openvyruz.png
```

From Windows, `wsl -d archlinux -- bash Scripts/WslCheck.sh [gcc|clang]` builds a copy of the tree inside WSL and runs the archive checks.

## Running

The game needs the original `vz-data.rsc`, which is not included. Pass an archive file or the directory containing it; the port remembers it in `game-folder.txt` in its user data folder (`%APPDATA%\OpenVyruz` or `$XDG_DATA_HOME/OpenVyruz`), and a later run without one uses `vz-data.rsc` beside the executable if there is one, else the remembered folder (which is how the installed Arch package finds it). When neither holds the archive, it asks for the folder with the system's folder picker (zenity or kdialog on Linux).

```powershell
.\Binaries\windows\Development\Vyruz.exe <path to vz-data.rsc>
```

```bash
./Binaries/linux/Development/Vyruz <path to vz-data.rsc>
```

The window draws the original panel ([Docs/Hud.md](Docs/Hud.md)): score, shield and energy bars, ammo, speed, radar, position, kills, enemies and time, and the floating pickup, key, gate, lift, reward and warning messages. Surprise items give their bonuses and maluses ([Docs/Surprises.md](Docs/Surprises.md)), invisibility among them.

`--verify-gaf` decodes every `.gaf` in `vz-data.rsc`, including animation frames, hotspots, mirror flags, names and embedded sound samples, the `.tga` story and ending images and the `.wav` sounds, and exits non-zero on any failure. `--verify-levels` loads all 25 levels (`.3DX`, `.3DI`, `.3DO` with resolved `.3DC` class properties, `.3DP` patrol paths, `.3DG`), checks their object and shadow GAFs and animation indices, decodes their animated map GAFs, and parses every object's `Behaviour`. `--verify-game` runs the frame-loop, terrain-collision, route-search and player-movement self-checks and replays `CAVE\DEMO.3DR` through the ported player movement, failing if fewer than 80% of steps match the recording exactly or fewer than 90% within half a pixel. It also loads every level's player weapons and checks player and turret firing for a few seconds, that destroyed objects drop their contents, that every level's first turret leaves a wreck before it is gone, and that nine in ten generators give birth within 20 seconds. Each level also puts the player in view of an armed cop, which must route to it and fire. Every level's cops then run for 30 seconds: at least half of the patrollers with a `.3DP` path must reach their next patrol point, and at least half of the random movers that found open ground must leave their cell. Every level's sensing gates must close with the player away and open for the player, and the player must ride every usable lift to a level that loads. `--verify-intro` decodes every frame of `Intro.avi` beside the archive, checking each picture against its band checksums, and its MP3 sound ([Docs/Intro.md](Docs/Intro.md)). `--auto-fire` holds the trigger down in the window, `--start X,Y` spawns the player at a world position, and `--dump-gaf PATH` lists an archive GAF's animations with their frame counts, flags and durations. In the window, N starts the next of the 25 levels (the list goes to the console at startup) and wraps to the first after the last, without a carried player, for checking every level.

Without a verify flag the window shows a level's map and placed objects, and the player drives with the original movement and terrain collision. The game folder's `vzconf.txt` (else the archive's) picks the input device as the original does: with `use-mouse = 1`, the installed default, the ship turns toward the green sight at the mouse, the left button fires, the right button (or Up) thrusts, Down backs off and Left/Right strafe; otherwise the keyboard uses the original's default keys: Up/Down thrust forward and backward, Left/Right turn, Left Shift with Left/Right strafes (A/D too), Space fires (Left Ctrl too), Backspace takes the next weapon and 1 to 9 a weapon slot, Tab cycles the panel modes (Left Shift steps back), M opens the map for 21000 cash, Page Down toggles low speed, P pauses, Esc opens the game menu (Resume Game, Retry Level, End Game). Its detail keywords (`shadow`, `reactor-fx`, `hud-green-info`, `translucent-panel`, `tv-borders`, `low-details`) switch the same effects as the original's Video Details, at the highest preset for any it leaves out, `difficulty-level` (0 Easy to 3 Insane) picks the difficulty from `COMMON\DIFFICULTY.3DC`, `gamma` sets the base gamma under the Gamma FX flashes, and `music`, `sound-fx`, `voices` and their volumes set the sound ([Docs/Audio.md](Docs/Audio.md)): the music loops, the menu sounds, the player's motor, weapon, pickup, money, alarm and death sounds, the enemies' weapon sounds, the sounds of explosions, gates and lifts, and the voice lines play. Like the original, the game stands still while its window is minimized or unfocused. The game plays the intro when `Intro.avi` is beside the archive ([Docs/Intro.md](Docs/Intro.md)), a key skipping it, then starts at the main menu ([Docs/Menus.md](Docs/Menus.md)), which `--level` and `--start` skip. Saving plots and each level start write sessions to `%APPDATA%\OpenVyruz\session` (Windows) or `$XDG_DATA_HOME/OpenVyruz/session` (Linux), which Restart Level and Load Game read, and the score table (`vz.sco`) beside them. The camera follows the player and stops at the level edges as in the original. Object bodies and grounded shadows use their initial animation pose, original hotspots, mirrored crops, altitude and depth ordering, including occlusion by terrain. `--level <zone>\<name>` picks the level (default `underground\t1u1`); a level without a player scrolls with the arrow keys instead. Static scenery is solid and pushable, and shooting it destroys it; turrets turn toward the player and fire back, and patrolling cops, jeeps, tanks, walkers, spiders, helis, burners and rollers follow their `.3DP` paths, seeking ones chase the player and shoot, random cops wander, and guardians return to their post. Not implemented yet: the other object behaviours.

## Layout

| Path | Contents |
|---|---|
| `Source\Pg\` | Reimplementation of the original `pg\lib\` engine library (archive, GAF, LZSS) |
| `Source\Game\` | Reimplementation of the original `vzed\` game code (level loader, frame clock, terrain collision, route search, player movement and combat) |
| `Source\Render\` | Vulkan renderer replacing the original Direct3D 6 path |
| `Source\Shaders\` | Slang shaders |
| `Docs\DataFormats.md` | Reverse-engineered data formats |
| `Docs\MainLoop.md` | Startup, level loading and frame loop of the original exe |
| `Docs\PlayerMovement.md` | Player input, turning, displacement and bounce, and the demo replay check |
| `Docs\Combat.md` | Random generator, weapons, shots, area damage and destruction |
| `Docs\PathSearch.md` | Incremental obstacle-corner route search and the fixed-seeker caller |
| `Docs\Audio.md` | The channel mixer, music, menu, motor and animation sounds |
| `Resources\` | The app icon: the Windows `.ico` and `.rc` linked into the exe, and the Linux desktop entry and icon |
| `Packaging\Arch\` | The Arch `PKGBUILD` |
| `External\` | Third-party code: GLFW as a submodule, miniaudio vendored |

## Original Module Map

The original source file names survive in `__FILE__` strings, and MSVC links each object file contiguously, so function addresses map to source files. In Ghidra, functions between two anchors of the same file are placed in a namespace named after that file.

| Range | Module |
|---|---|
| `00401000-00413740` | unknown game code (no `__FILE__` anchors, 281 functions) |
| `00413800-0044967f` | `vzed\` game: `3do`, `anim`, `surprise`, `decor`, `misc`, `engine-damage`, `engine`, `loop`, `i3de`, with unanchored game files in between (largest gap `0041cf40-00433930`, 288 functions, between `surprise` and `decor`) |
| `00449700-00481350` | `pg\lib\` engine: `pg\` core (io, mem, file, font, bmp, video, input, data, dialog, snd, audio, text, gob, cursor, gaf, cmap), `dx\` and `hw\` DirectX backends |
| `004813b0-0050879b` | after `hwtimer`: MSVC CRT and static libraries, but also unanchored pg code such as `LzssDecode` (`0049ace0`) |

## License

Licensed under the [MIT License](LICENSE).
