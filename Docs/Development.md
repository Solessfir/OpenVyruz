# Development

OpenVyruz is a functional (non-matching) reimplementation of `Vyruz.exe` 1.31.01, written from the decompiled original in Ghidra. Gameplay matches the original: frame stepping, random generator, math and evaluation order. The platform layer (window, input, audio, file I/O, rendering) is redesigned. Deliberate deviations live on `main`; the `classic` branch keeps the original's rules. [AGENTS.md](../AGENTS.md) holds the working rules, the reverse-engineering workflow and the verification steps.

## Layout

| Path | Contents |
|---|---|
| `Source\Pg\` | Reimplementation of the original `pg\lib\` engine library (archive, GAF, LZSS, fonts, sound mixer, video) |
| `Source\Game\` | Reimplementation of the original `vzed\` game code (levels, frame clock, collision, route search, movement, combat, panel, menus, sessions) |
| `Source\Render\` | Vulkan renderer replacing the original Direct3D 6 path |
| `Source\Shaders\` | Slang shaders |
| `Source\Tools\` | `--extract` and `--compress` |
| `Docs\` | These notes |
| `Resources\` | The app icon: the Windows `.ico` and `.rc` linked into the exe, and the Linux desktop entry and icon |
| `Packaging\Arch\` | The Arch `PKGBUILD` |
| `Scripts\` | The WSL build check |
| `External\` | Third-party code: GLFW as a submodule; miniaudio and a subset of SDL_GameControllerDB vendored |

## Original module map

The original source file names survive in `__FILE__` strings, and MSVC links each object file contiguously, so function addresses map to source files. In Ghidra, functions between two anchors of the same file are placed in a namespace named after that file.

| Range | Module |
|---|---|
| `00401000-00413740` | unknown game code (no `__FILE__` anchors, 281 functions) |
| `00413800-0044967f` | `vzed\` game: `3do`, `anim`, `surprise`, `decor`, `misc`, `engine-damage`, `engine`, `loop`, `i3de`, with unanchored game files in between (largest gap `0041cf40-00433930`, 288 functions, between `surprise` and `decor`) |
| `00449700-00481350` | `pg\lib\` engine: `pg\` core (io, mem, file, font, bmp, video, input, data, dialog, snd, audio, text, gob, cursor, gaf, cmap), `dx\` and `hw\` DirectX backends |
| `004813b0-0050879b` | after `hwtimer`: MSVC CRT and static libraries, but also unanchored pg code such as `LzssDecode` (`0049ace0`) |
