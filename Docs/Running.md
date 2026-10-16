# Running

## The game data

The game needs the original `vz-data.rsc`, which is not included. Pass the archive file or the folder holding it:

```powershell
.\Binaries\windows\Development\Vyruz.exe <path to vz-data.rsc>
```

```bash
./Binaries/linux/Development/Vyruz <path to vz-data.rsc>
```

The port remembers the folder in `game-folder.txt` in its user data folder (`%APPDATA%\OpenVyruz` or `$XDG_DATA_HOME/OpenVyruz`). A later run without one uses `vz-data.rsc` beside the executable if there is one, else the remembered folder, which is how the installed Arch package finds it. When neither holds the archive, it asks for the folder with the system's folder picker (zenity or kdialog on Linux).

The game plays the intro when `Intro.avi` is beside the archive ([Intro.md](Intro.md)), a key skipping it, then starts at the main menu ([Menus.md](Menus.md)).

## Command line

`-h` or `--help` lists every argument. Options take one dash or two in any case, an unknown one is an error, and a tool such as `--extract` comes first and runs alone ([Tools.md](Tools.md)).

| Option | Effect |
|---|---|
| `<game folder>` | The folder with `vz-data.rsc`, or the archive itself, remembered for later runs |
| `--level <level>` | Start straight in a level, skipping the menu: a campaign number from 1 to 24, a level id such as `2.3`, or a path such as `cave\t2c1` |
| `--start <x>,<y>` | Put the player at that world position as the level starts; skips the menu |
| `--skipIntro` | Go straight to the main menu, without the story page and the intro |
| `--windowed` | Start in a window instead of fullscreen |

Cheats, which the original does not take this way, combine freely and never touch the demo:

| Option | Effect |
|---|---|
| `--godMode` | Energy and shield stay full |
| `--infiniteAmmo` | Every ammo the player holds stays full |
| `--allWeapons` | Every weapon is armed with full ammo at each level start |
| `--infiniteMoney` | Cash stays at 999999 |

```powershell
.\Binaries\windows\Development\Vyruz.exe <path to vz-data.rsc> --level 7 --godMode --allWeapons
```

## Display

The game runs fullscreen at the desktop resolution; Alt+Enter or F11 switches between fullscreen and a window. A level keeps the original's 480 rows and widens its view to the screen's shape, so a wide screen shows more of the level instead of stretching it, with the panel at the screen's corners. The menus, the other screens and the demo keep the original 4:3 picture, letterboxed. The scaling keeps every original pixel sharp and the same size at any resolution, blending only the screen pixel across each seam, so the picture does not shimmer as it scrolls and turned sprites have smooth edges. The view follows the ship to a fraction of a pixel, so it glides to a stop instead of stepping a whole pixel at a time. Explosions, shots and other lights glow softly onto their surroundings (bloom), unless something is drawn over them. A hit on the player splits the picture's colours toward its edges for a moment, more for a bigger hit.

Like the original, the game stands still while its window is minimized or unfocused, and while active it keeps the mouse cursor inside the window.

## Controls

The keyboard, mouse and gamepad controls are in the [manual](Manual.md#controls). The Options menus rebind the keys and mouse buttons.

## Settings, saves and scores

The game folder's `vzconf.txt` (else the archive's) holds the settings: the details, the gamma, the sound, the input device with its key bindings, the player's name, the difficulty and the language. The Options menus change them and write them back in the original's own format, so the port and the original share the file ([Menus.md](Menus.md#options-and-extras)). The Russian language prints the Russian build's texts ([Menus.md](Menus.md#language)).

Saving plots and each level start write sessions in the original's own format to the game folder's `session` folder, which Restart Level and Load Game read, and the score table (`vz.sco`) beside it, so the port and the original share their saves and scores ([Menus.md](Menus.md#sessions)).
