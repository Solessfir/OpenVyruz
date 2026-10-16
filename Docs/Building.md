# Building

C++23, Premake5, GLFW, Vulkan 1.3 and Slang shaders. Clone with submodules (GLFW), or fetch them afterwards. miniaudio is vendored as its single header in `External/miniaudio`.

```bash
git clone --recursive https://github.com/Solessfir/OpenVyruz.git
git submodule update --init    # if cloned without --recursive
```

The project generators download a pinned Premake on first run. Shaders are Slang (`Source/Shaders/*.slang`), compiled by `slangc` at build time into SPIR-V arrays embedded in the executable. Run the generator again after adding or removing source or shader files, and after switching branches.

Configurations: `Debug`, `Development` (optimized with symbols), `Shipping`.

## Windows

Requirements:

- Visual Studio 2026 or its Build Tools, with the "Desktop development with C++" workload
- Vulkan SDK 1.3 or newer, which provides `slangc`. `VULKAN_SDK` is used when set; otherwise the generator falls back to `%VULKAN_SDK%`

```powershell
.\GenerateProjectFiles.bat vs2026
msbuild Vyruz.slnx /m /t:Vyruz /p:Configuration=Development /p:Platform=x64
```

The executable is written to `Binaries\windows\Development\Vyruz.exe`. You can also open `Vyruz.slnx` in Visual Studio or Rider.

## Linux

Requirements: GCC 14+ or Clang 18+, GNU Make, `slangc`, Vulkan headers and loader, and the Wayland and X11 development packages (`wayland-scanner` included). On Arch:

```bash
sudo pacman -Syu --needed base-devel vulkan-headers vulkan-icd-loader shader-slang wayland libxkbcommon libx11 libxrandr libxinerama libxcursor libxi
```

```bash
./GenerateProjectFiles.sh gmake               # add --cc=clang to build with Clang
make config=development Vyruz -j"$(nproc)"
```

The executable is written to `Binaries/linux/Development/Vyruz`.

### Arch package

`Packaging/Arch/PKGBUILD` builds the `openvyruz-git` package from the latest `main`. It installs `/usr/bin/openvyruz` with its desktop entry and icon. On the first run it asks for the game folder (the one holding `vz-data.rsc`), or pass it once: `openvyruz <path to vz-data.rsc>`.

```bash
cd Packaging/Arch && makepkg -si
```

Without the package, install the desktop entry and icon yourself. The window's app id is `openvyruz`, so the compositor shows the icon once they are in place:

```bash
install -Dm644 Resources/Linux/openvyruz.desktop ~/.local/share/applications/openvyruz.desktop
install -Dm644 Resources/Linux/openvyruz.png ~/.local/share/icons/hicolor/256x256/apps/openvyruz.png
```

### From Windows through WSL

`wsl -d archlinux -- bash Scripts/WslCheck.sh [gcc|clang]` builds a copy of the tree inside WSL and runs the archive checks ([Tools.md](Tools.md#checks)).

## Continuous integration

GitHub Actions (`.github/workflows/ci.yml`) builds every push and pull request with MSVC on Windows and with GCC and Clang on Ubuntu, and runs cppcheck. Pushing a tag such as `v1.0` also builds `Shipping` for both systems and publishes `OpenVyruz-<tag>-windows-x64.zip` and `OpenVyruz-<tag>-linux-x64.tar.gz` as a GitHub release. The archives hold the executable only; the game data stays the player's own.
