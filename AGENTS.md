# OpenVyruz Agent Guide

## Scope

Applies to the whole repository. The current user request takes precedence over this file. Read [README.md](README.md) and [Docs/DataFormats.md](Docs/DataFormats.md) before architectural changes.

## Project direction

OpenVyruz is a functional (non-matching) reimplementation of `Vyruz.exe` 1.31.01. The goal is a native Windows 11 and Linux (Wayland first) game that loads the original `vz-data.rsc` and can be completed 100%. Version 1 covers the single-player campaign plus the intro and cutscenes; multiplayer is out of scope.

- Gameplay must match the original: same frame stepping (a variable millisecond step capped at the display period, 10 ms for demos; see [Docs/MainLoop.md](Docs/MainLoop.md)), RNG, integer and floating-point math, and evaluation order. Only the platform layer (window, input, audio, file I/O, rendering) is redesigned.
- The original rendered with hardware Direct3D 6 at 640x480x16. The port uses a full Vulkan 1.3 renderer (dynamic rendering, synchronization2) that reproduces the original render states, drawn in the original 640x480 coordinate space and letterboxed.
- No original game files, extracted assets, or decompiler output dumps are ever committed. The data lives in the separate original-data repository (`the original-data repository`, `Game\Vyruz\`).

## Reverse-engineering workflow

- `Vyruz.exe` is analyzed in Ghidra through GhidraMCP (`.mcp.json`, server `vyruz-ghidra`): Claude Code starts `ghidra_mcp --headless` over stdio, which opens `Vyruz.exe` in the project `the Ghidra project` and saves changes to it on exit. Close that program in the Ghidra GUI first, since the project cannot be opened twice.
- The binary is MSVC C++ written in C style (almost no methods, no RTTI). Recover structure from `__FILE__` anchors; see the module map in README.
- Read the decompiled code to understand behavior, then write clean, idiomatic port code. Never paste raw decompiler output into the repository.
- When a function's behavior is understood, rename it in Ghidra (and its key globals), and confirm with `get_program_info` at the start of a session that the server has `Vyruz.exe` open; the headless server saves the project when the session ends. Reference the original address in a short comment on the port code that reimplements it, e.g. `RscFileRead at 004559f0`.
- The binary is the ground truth. The Python scripts and notes in the original-data repository are hypotheses: verify every format detail against the exe or raw bytes before relying on it. Correct `Docs/DataFormats.md` when either turns out wrong. The same applies to the older Godot-port notes in `the original-data repository's Docs\` (`Reverse Engineering Reference.html`).
- There is no ASLR, so live process memory addresses equal Ghidra addresses; reading a running original game is a valid way to establish ground truth.
- Name port modules after the original source files (`pg\lib\pg\pgfile.cpp` -> `Source/Pg/...`, `vzed\engine.cpp` -> `Source/Game/...`).

## C++ rules

- C++23 on MSVC, Clang, and GCC. Prefer `constexpr` for pure operations; `consteval` only when the contract requires compile-time evaluation.
- Unreal-style naming: `F` for value types, `E` for enums, `b` for booleans, PascalCase for functions, variables, and files.
- Prefer RAII, `std::span`, `std::string_view`, ranges, and `std::optional`/`std::expected` for expected failures. No exceptions for control flow; treat allocation failure as fatal.
- All original data is untrusted input: bounds-check every offset and size read from a file before use.
- No magic numbers. Declare on-disk layouts as plain structs with a `static_assert` on their size and read them with `ReadStruct` (`Source/Pg/Binary.h`) instead of hand-written byte offsets. Give every other constant (sizes, masks, slot indices) a named `constexpr`, derived from related constants where possible.
- Use designated initializers for descriptors and aggregates with more than one field. End multi-line braced lists with a trailing comma.
- Always brace `if`/`else`/`for`/`while` bodies. Blank line after a closing brace of a control block unless the next line closes the scope.
- Float literals without trailing zeros: `1.f`, `0.5f`.
- Mark member functions `const` when they leave observable state unchanged. Declare member functions before data members, public then private.
- Keep headers self-contained and third-party types out of public APIs where practical.

Comments explain why: original-game quirks, format constraints, engine behavior, or lifetime rules. Do not narrate what the code does. Keep comments short and never split a sentence just to fit a column.

## Dependencies

- Add dependencies only when the current work needs them. Source dependencies are pinned Git submodules under `External/` (GLFW is upstream `glfw/glfw`; switch to a fork only when a concrete need appears).
- Premake is downloaded by `GenerateProjectFiles` into ignored `External/Premake/` with a pinned version and SHA-256. Never commit tool binaries.
- The Vulkan SDK comes from `VULKAN_SDK`, falling back to `%VULKAN_SDK%`.

## Verification

- Build: `msbuild Vyruz.slnx /m /t:Vyruz /p:Configuration=Development /p:Platform=x64`. Regenerate project files after adding or removing source files.
- After C++ changes, also compile with Clang from the MSVC install (`ClangCL` toolset, `VC\Tools\Llvm` in the VS 18 BuildTools). This compile-only check keeps its objects apart from the MSVC build:
  `msbuild Intermediate\ProjectFiles\Vyruz.vcxproj /t:ClCompile /p:BuildProjectReferences=false /p:MSBuildWarningsAsMessages=MSB8028 /p:Configuration=Development /p:Platform=x64 /p:PlatformToolset=ClangCL /p:IntDir="$PWD\Intermediate\ClangCheck\\"`
- After C++, shader, or build changes, also build on Linux in WSL (Arch, distro `archlinux`) with both GCC and Clang: `wsl -d archlinux -- bash Scripts/WslCheck.sh gcc` and `... clang`. The script copies the tree to `~/OpenVyruz-wsl/<compiler>`, generates gmake files, builds, and runs `--verify-gaf` and `--verify-levels`. It needs `vulkan-headers`, `shader-slang`, `wayland`, and the X11 development packages from pacman; WSL's Arch is rolling, so update with `pacman -Syu`, never a partial `-Sy`. The Linux binary runs on WSLg Wayland with lavapipe, but WSLg windows cannot be screenshotted, so Linux visuals stay unverified unless the user checks them.
- Run cppcheck on touched code and fix real findings; `useStlAlgorithm` style notes are advisory:
  `& "C:\Program Files\Cppcheck\cppcheck.exe" --std=c++23 --enable=warning,style,performance,portability --inline-suppr --suppress=missingIncludeSystem --check-level=exhaustive -q -I Source -D_WIN32 -D_M_X64 Source`
- Every loader gets a whole-archive check mode, like `--verify-gaf`, that decodes all relevant entries and exits non-zero on failure. Run it after touching that loader.
- Check rendering changes visually (screenshot of the running window); compilation alone does not verify appearance.
- Do not rebuild for comment or documentation-only changes. Report exactly what was verified and what was not.

## Documentation and writing

- Keep `README.md` current for build, run, and layout changes; put format knowledge in `Docs/DataFormats.md` and other docs under `Docs/` with PascalCase names.
- Never use em dashes or en dashes; use a hyphen or rewrite.
- Preserve exact path casing for Linux.

## Git

- Work directly on `main`; do not create feature branches. Committing and amending unpushed commits is allowed. Never push, force-push, or rewrite pushed history without an explicit request.
- Commit messages are short natural titles, no `feat:`-style prefixes, no body, no `Co-authored-by` trailers.
- Submodule revisions are intentional locks.

## Current state

Implemented: Premake + GLFW + Vulkan window with letterboxed 640x480 space; `.rsc` archive reader (`Source/Pg/ResourceArchive`); GAF/LZSS sprite loader (`Source/Pg/Gaf`, all 178 GAFs decode); sprite-sheet packing and a textured-quad sprite pipeline (`Source/Render/Renderer`); level loader for `.3DX/.3DI/.3DO/.3DC/.3DG` with resolved class properties (`Source/Game/Level`, all 25 levels pass `--verify-levels`); placed objects drawn in their initial pose with hotspots, shadows, blend modes and the original depth order; the frame clock and variable millisecond step (`Source/Game/Loop`); static terrain collision built for every level and its movement query (`Source/Game/Collision`); player input, turning, displacement and terrain bounce (`Source/Game/Engine`, [Docs/PlayerMovement.md](Docs/PlayerMovement.md)), checked by `--verify-game` against the `CAVE\DEMO.3DR` recording; object collisions, pickups, the shared random generator, and the player's weapons, shots, damage, effects, chained explosions and turrets ([Docs/Combat.md](Docs/Combat.md)); patrolling, wandering, guarding, chasing and shooting cops and jeeps ([Docs/PathSearch.md](Docs/PathSearch.md)). The window lets the player drive through a level with a centred camera. The original startup and frame loop are documented in [Docs/MainLoop.md](Docs/MainLoop.md).

Next: the other actor kinds (tanks with their turret top, walkers and spiders with their stepping gait, helis), then lifts, gates and teleports.
