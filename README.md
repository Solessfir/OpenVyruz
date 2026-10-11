# OpenVyruz

Native reimplementation of *Vyruz: Destruction of the Untel Empire* (2001, Metropolis Software / C2V, "pg" engine) for Windows 11 and Linux (Wayland/X11). It reads the original game data; no game files are included.

C++23, Premake5, GLFW, Vulkan 1.3, Slang shaders.

## Building

```powershell
git submodule update --init
.\GenerateProjectFiles.bat vs2026    # downloads Premake on first run, writes Vyruz.slnx
msbuild Vyruz.slnx /m /t:Vyruz /p:Configuration=Development /p:Platform=x64 && .\Binaries\windows\Development\Vyruz.exe
```

On Linux use `./GenerateProjectFiles.sh gmake` (needs `wayland-scanner`, `slangc`, Wayland/X11 dev packages and the Vulkan loader). On Windows `VULKAN_SDK` is used when set, otherwise the SDK copy under `%VULKAN_SDK%`. Shaders are Slang (`Source\Shaders\*.slang`), compiled by `slangc` at build time into SPIR-V arrays embedded in the exe.

## Running

The exe needs the original `vz-data.rsc`. Pass the directory containing it as an argument; the default is `../Vyruz/Game/Vyruz`, the original-data repository checked out next to this one.

`--verify-gaf` decodes every `.gaf` in `vz-data.rsc` and exits non-zero on any failure.

## Layout

| Path | Contents |
|---|---|
| `Source\Pg\` | Reimplementation of the original `pg\lib\` engine library (archive, GAF, LZSS) |
| `Source\Render\` | Vulkan renderer replacing the original Direct3D 6 path |
| `Source\Shaders\` | Slang shaders |
| `Docs\DataFormats.md` | Reverse-engineered data formats |
| `External\` | Third-party submodules (GLFW) |

## Original Module Map

The original source file names survive in `__FILE__` strings, and MSVC links each object file contiguously, so function addresses map to source files. In Ghidra, functions between two anchors of the same file are placed in a namespace named after that file.

| Range | Module |
|---|---|
| `00401000-00413740` | unknown game code (no `__FILE__` anchors, 281 functions) |
| `00413800-0044967f` | `vzed\` game: `3do`, `anim`, `surprise`, `decor`, `misc`, `engine-damage`, `engine`, `loop`, `i3de`, with unanchored game files in between (largest gap `0041cf40-00433930`, 288 functions, between `surprise` and `decor`) |
| `00449700-00481350` | `pg\lib\` engine: `pg\` core (io, mem, file, font, bmp, video, input, data, dialog, snd, audio, text, gob, cursor, gaf, cmap), `dx\` and `hw\` DirectX backends |
| `004813b0-0050879b` | after `hwtimer`: MSVC CRT and static libraries, but also unanchored pg code such as `LzssDecode` (`0049ace0`) |
