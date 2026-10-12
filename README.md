# OpenVyruz

Native reimplementation of *Vyruz: Destruction of the Untel Empire* (2001, Metropolis Software / C2V, "pg" engine) for Windows 11 and Linux (Wayland/X11). It reads the original game data; no game files are included.

C++23, Premake5, GLFW, Vulkan 1.3, Slang shaders.

## Building

Clone with submodules (GLFW), or fetch them afterwards:

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

From Windows, `wsl -d archlinux -- bash Scripts/WslCheck.sh [gcc|clang]` builds a copy of the tree inside WSL and runs the archive checks.

## Running

The game needs the original `vz-data.rsc`, which is not included. It looks beside the executable by default. Pass an archive file or the directory containing it to use another location.

```powershell
.\Binaries\windows\Development\Vyruz.exe <path to vz-data.rsc>
```

```bash
./Binaries/linux/Development/Vyruz <path to vz-data.rsc>
```

`--verify-gaf` decodes every `.gaf` in `vz-data.rsc`, including animation frames, hotspots, mirror flags and names, and exits non-zero on any failure. `--verify-levels` loads all 25 levels (`.3DX`, `.3DI`, `.3DO` with resolved `.3DC` class properties, `.3DG`), checks their object and shadow GAFs and animation indices, and decodes their animated map GAFs. `--verify-game` runs the frame-loop, terrain-collision, route-search and player-movement self-checks and replays `CAVE\DEMO.3DR` through the ported player movement, failing if fewer than 80% of steps match the recording exactly or fewer than 90% within half a pixel. It also loads every level's player weapons and checks player and turret firing for a few seconds. `--auto-fire` holds the trigger down in the window.

Without a verify flag the window shows a level's map and placed objects, and the player drives with the original movement and terrain collision: Up/Down thrust forward and backward, Left/Right turn, A/D strafe, Space or Left Ctrl fire. The camera follows the player and stops at the level edges as in the original. Object bodies and grounded shadows use their initial animation pose, original hotspots, mirrored crops, altitude and depth ordering. `--level <zone>\<name>` picks the level (default `underground\t1u1`); a level without a player scrolls with the arrow keys instead. Static scenery is solid and pushable, and shooting it destroys it; turrets turn toward the player and fire back. Not implemented yet: object behaviours, animated map objects, and object occlusion by the baked map.

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
| `External\` | Third-party submodules (GLFW) |

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
