# Menus

The menus come from `vzed\menu.cpp`. `VzMenuLoop` (`00447820`) (VzRun's loop) shows the main menu (`MenuMain` (`004271a0`)) and runs levels until the game ends; Escape during a level opens the game menu (`MenuGame` (`004223e0`), through `EngineEscape` (`00445b40`)).

## Art

`MenuLoadArt` (`00420560`) loads `Common\Data\Menu\menu.gaf` and `v-menu.gaf` (from the `tmp` copies VzRun extracts):

- `menu.gaf` animation 4 (`MENU_BG00`) is the full-screen background, drawn first at 0, 0.
- `v-menu.gaf` animation 6 (`VZfire`) is a flash over it: each update pass draws `RandomNext() % 87` and flags the flash on zero; the next draw pass draws it once and clears the flag.
- `menu.gaf` animation 8 (`cursor`, 20 frames) is the mouse cursor, drawn last at the mouse and animated only while shown.
- `v-menu.gaf` fonts: 1 for items, 2 for the item under the cursor, 4 for help texts, 5 (mode 5, alpha 0x80) for the build date `Apr 10 2000 17:52:06` that `MenuDrawBuildDate` (`00422340`) prints at 4, 464.

The flash's draws come from the shared generator. Before a level they do not matter, since `VzLoadLevel` reseeds; the game menu's draws do change the level's sequence.

## Layout

`MenuCreate` (`004209b0`) makes a menu over a rectangle with an item step: the main and game menus use 270, 90 to 640, 480 with items 45 pixels apart. Item N prints at `Left + N * StepX`, `Top + N * StepY` (the font's baseline), and the mouse is over it strictly between that baseline and the previous one, left of the rectangle's right edge. An item without text is a gap. The title prints at 10, 120 in menu font `+0x74`. The help text of the selected item prints in font 4 with word wrap inside 400, 360 to 635, 475, its cursor `LineHeight - LineGap` below the clip's top.

`RunMenu` (`00421050`) runs a menu: the cursor starts on menu `+0x4c`'s item (or where the menu's last run left it), each step runs the update pass of `MenuStep` (`00421780`), and each frame one draw pass.

## Input

PgPoll's key codes index the key name table at `0x517a94`: 1 Escape, 14 Backspace, 32 Space, 33 Return, 200 Up, 208 Down, and 0x103 the left mouse button.

- The item the mouse is over becomes the selected one (state `+0x0`), which stays when the mouse leaves; over a gap it becomes none. Only the item under the mouse prints in the highlighted font.
- Escape and Backspace choose the default item (menu `+0x5c`); Space, Return and the left button the selected one.
- Up and Down find the row under the cursor and take the one above or below; when that is the row the arrow keys last chose, or a gap, they search on in that direction for an item with text, wrapping around. The cursor then jumps to `(Right - Left + StepY) / 4 + Left`, a row's middle, and hides until the mouse moves.
- The idle timeout (menu `+0x54`, 30 seconds for the main menu) returns menu `+0x58`; the main menu's 10 plays `demo.3dr`.

## Main menu

| Item | Help | Choice |
| --- | --- | --- |
| Restart Level, or a gap without a default session | `SessionDescribeDefault` (`00427ad0`): `%s   (%d:%d:%d)\nEnergy : %d\nShield : %d\nDifficulty : %s ...\n` with the level name, the clock and the player's energy and shield | "Retry level ?" (Yes please ! / No thanks., default No thanks.), then the default session |
| Start New Game | | the first level |
| Load Game | `%d saved session%s available.` or `No saved session.` | `SessionLoad` (`0042ba00`) mode 0, the saved sessions |
| Options | `Video, Audio, Control, User settings, Language.` | `MenuOptions` |
| Extras | `Scores, Credits, Intro, Slide Show, Demo.` | `MenuExtras` |
| Exit (default) | | "Exit Now ?" (Yes, bye. / No !, default Yes), with explosions over the menu (`MenuExplode` (`00421460`)) before leaving |

The cursor starts on Restart Level when there is a default session and on Start New Game otherwise. The difficulty names are Easy, Normal, Hard and Insane (`DifficultyName` (`00403310`)); the default is Normal.

## Options and Extras

The Options menu (`MenuOptions`, `004229a0`) lists Video (`Details, Device, Video Mode, Gamma.`), Audio (`Music, Sound FX, Quality, Frequency, Mono/Stereo.`, or `Sound driver (DirectSound) is unavalaible.` without DirectSound), Control (`Input device, Key settings.`), User settings (`User name, Game difficulty level.`) and Language (`English.`), then Back, the default; each opens its own menu (`FUN_00422c10`, `FUN_00423e20`, `FUN_00424f30`, `FUN_00425b40`, `FUN_004259f0`).

Video Details (`MenuVideoDetails`, `004235a0`) offers Max, which sets option flags (session `+0x27e0`) `0xbe` and clears `0x40`, Min, which clears `0xae` and sets `0x40` (Translucent Panel stays as it was), and Custom (`MenuVideoDetailsCustom`, `00423760`), which toggles each one:

| Flag | Custom item | `vzconf.txt` keyword | Effect |
|---|---|---|---|
| `2` | Shadow | `shadow` | Object shadows |
| `4` | Reactor FX | `reactor-fx` | The player's reactor glow |
| `8` | HUD Green Info | `hud-green-info` | The weapon lists, the mines note and the panel's lock and shield warnings |
| `0x10` | Translucent Panel | `translucent-panel` | The panel screen in draw mode 5 rather than 1 |
| `0x20` | TV Borders | `tv-borders` | The borders around the view |
| `0x40` | Ground/Smoke details (shown inverted) | `low-details`, or `high-details` inverted | Set, it hides `Is-detail` classes (class flag `0x100000`), the player's front light and the smoke particles |
| `0x80` | Gamma FX | `gamma-fx` | Gamma flashes |

`ConfigLoad` (`0042f570`) reads each keyword's value with `atoi` and the menus write them back to `vzconf.txt` (`ConfigSave`, `004303f0`). The port reads them the same way and starts from Max for any keyword the file leaves out; the archive's own `VZCONF.TXT` is the Max preset.

The Extras menu (`MenuExtras`, `00427820`) lists Scores, Credits, Intro (`Select to view intro animation.`, `Install Indeo 5.11 or higher to see the intro.` or `Intro animation file is not present.`), Slide Show only while `vz-slide.rsc` is present (`SlidesPresent`, `00404a60`), and Demo (`Auto-playing game demo.`), then a gap without the slide show, and Back, the default. This executable is the Russian build: in English it still prints the Extras title, Scores, Intro and Audio in Russian (Windows-1251 strings at `0051db88`, `00513c8c`, `00513b64`, `00512828`).

- Scores shows the score table (`SaveScores`, mode 0).

## Scores

`SaveScores` (`0043e470`) shows the score table; with its second argument set it first enters a record for the game just ended. `EngineGameOver` is followed by it, and so are the ending pages; ending a game from the game menu is not.

- **The file:** `vz.sco` in the game folder, 0x5a0 bytes: twenty 72-byte records, best first. `ScoreRun` (`00401960`) starts a file of any other size over, empty, and writes the table back when the new record placed.
- **A record:** a 32-bit value per column (0 and 1 unused, then the tower and level of the `.3DO` `LEVEL-ID [ tower.level ]` from session `+0x26f4` and `+0x26f8`, the player's kills `+0x26c`, the money earned `+0x270`, the hit share `hits * 100 / shots`, the shots, and the game clock's hours, minutes and seconds, each modulo 60), and the player's name at `+0x34`, 20 bytes.
- **Money earned** (player `+0x270`) rises with the cash for pickups and rewards but not for spending. **Hits and misses** (session `+0x26e0`, `+0x26e4`) are counted by `AmmoHit` (`0043d7d0`) for every one of the player's shots that bursts: a hit when it struck an object that hunts the player (`ObjectHuntsPlayer` (`004371b0`), which also takes a generator of hunters) or an item, a miss otherwise.
- **Ranking** (`ScoreInsert` (`004015a0`)): the record climbs from the bottom while it is not lower on the sorted columns compared in order (tower, level, kills, money, hit share, shots), so a tie goes above the older record; the rank is 1 to 20, or 0 when it does not place.
- **The table** (`ScoreDrawLoop` (`00401670`), layout at `DAT_0050d070`): `score.gaf`'s first frame at its own place, then in v-menu font 4 the headers at y 80 in `0x80ffa0` ("THE BRAVEST RESISTANCE FIGHTERS" centred 32 higher, specular `0x2000`), and rows from y 110 every 18 pixels, `#rank`, tower, level, kills, money, hit share, shots, `%02d:%02d:%02d` and the name, each aligned in its column's box (`PgFontPrintAligned` (`0044b100`)). Row colours blend from white (row 1) to `0x80a0ff` with specular `0x90` (row 20) by `ColorBlend` (`0044df60`). The new record's row is brightened in place (`ScoreHighlightRow` (`00401410`)). A key or mouse button ends it.
- Credits (`ShowCredits`, `00402990`) draws `credits.gaf` and scrolls `credits-en.txt` (`credits-fr.txt` in the other language) in v-menu font 4 with word wrap inside 380, 0 to 639, 479, from the clip's bottom up by 0.5 pixel per step (the speed argument 500 times 0.001), until the text has passed the top or a key or mouse button goes down.
- Slide Show (`ShowSlides`, `00404aa0`) opens `vz-slide.rsc` and shows `slides\slide-001.tga` to `slide-031.tga`, each centred, for up to 4 seconds (`WaitForInput`); it starts over while any slide showed and ends on a key or mouse button.
- Demo returns 10, as an idle main menu does, and `VzMenuLoop` plays the demo.

## Demo

`VzMenuLoop` sets the level to `cave\demo.3di` and `DemoLoad` (`004292b0`) reads `demo.3dr` (`CAVE\DEMO.3DR`): a count, then 20-byte records of the step's game-clock time, its input bits, and the player's position and direction before the step. EngineRunLevel writes no default session for it. `DemoStartPlayback` reseeds the shared generator after `LevelStartMapAnimations` has drawn and before `EngineActivateObjects`, and starts the game clock at the first record's time; steps are capped at 10 ms.

Before each step's update, unless Escape is down (which ends the demo), `DemoReplayStep` (`004291d0`) gives the record under the cursor's input once the game clock has reached its time and moves the cursor on, staying on the last record; before that the input is 0. `EngineReplayActionKeys` (`0043bb00`) then replays its actions: 0x100 the next weapon, 0x200 the speed toggle, 0x400 the map, 0x800 and 0x1000 the inventory and help flags, 0x2000 pause, 0x4000 the panel cycle (never backwards), 0x10000 << N weapon slot N + 1, and 0x8000 ends the demo. In the player's update `ReplayPlayerInput` (`00438800`) puts the player where the record before the cursor has it (`DemoReplayPosition`), with the move from its previous position in collision units as its velocity, faces it the record's way (pose 2 * direction + 1), and drives with the input, dropping the turn while strafing. The demo then returns to the menu.

## Game menu

Resume Game (the default), Options, Retry Level (only with a default session; otherwise a gap ends the list) and End Game. Retry Level asks "Retry level ?" as above and restarts from the default session; End Game asks "End this Game ?" (Yes / No, whose default is Yes) and returns to the main menu without the game-over image. A dead player's Escape ends the game at once with the game-over image.

## Sessions

`SessionWrite` (`0042ad70`) writes a session into `session\<session-name>\default` or `session\<session-name>\s%03d` in the game folder (`session-name` comes from `vzconf.txt`):

- `<zone>-<level>.3do`, the level's objects (`Level3doWrite`, `004163a0`). It visits the grid from its last row and column back to the first, each cell's newest object first, the same order `EngineActivateObjects` uses; a level loaded from it therefore lists each cell's objects the other way round. `Level3doWriteObject` (`00416730`) writes `Class-name`, `Position` and only the fields that differ from a fresh object of the class: `Energy`, `Shield`, `Dir`, `Behaviour`, `Life-time`, `Inter-frame`, the turn timeouts, the object flags, and the attribute blocks through `FUN_00417210` (`TYPE Gate` and `TYPE Lift` with their state, `TYPE Patrol` with its current `Point-ID`, `TYPE Generator` with `Max-births` but not the births so far, `TYPE Key`, `TYPE Item` timeouts). Carried objects (the player's weapons, ammo and keys) are written before their carrier. Flags 0xb, which every caller passes, leave out the `No-saving` classes (effects and shots) and add `DIFFICULTY-LEVEL`.
- `last.txt`, one record: `#%d %d`, the session folder, the zone, the `.3di` file, the level name, the clock in milliseconds, the player's energy and shield as `%.3f`, and seven numbers.
- A `.tga` thumbnail (`FUN_0042b360`), which the session menu shows at 8, 150 for the session under the cursor.

A numbered session takes the highest number in the folder plus one, from `s000`, and the sessions seven or more numbers below it are deleted. EngineRunLevel writes the default session as a level starts, unless the level came from a session; `ObjectUpdateLevelExit` writes it too.

A `Saving-Plot-Area` object (`Saving-zone`, invisible, 64 pixels) and a `Saving-Plot` object (`Saving-plot`, `Reward = -100000`, the cost) lie at the same position. While the living player touches the area, `EngineUpdateObjects` finds the plot in the area's cell at exactly the same position and calls `SavingPlotUpdate` (`0042a3b0`), timed on PgClockNow:

- The first contact starts the count and clears the done flag.
- Before 5.5 seconds it shows `Saving in %d second%s` with `(6000 - elapsed) / 1000` seconds, the `s` only above one.
- Then, once: without the cash, `Not enough Cash %s !\n You need %d $\n  to save this game ...` with the player's name, for 5 seconds; otherwise the cost comes off the cash, `SessionWrite` writes a numbered session, and `Saving Completed.` shows for 3 seconds.

The texts print at the plot minus 150, 50 in world space in `v-bonus.gaf` font 6, each replacing the last. Once the player touches no area, `SavingPlotLeave` (`0042a650`) ends the count; before the save or refusal it removes the message at the plot's x - 150, y (not y - 50) and shows `No Saving.` for a second. `SessionLoad` (`0042ba00`) in mode 0 sets the counting flag without restarting the count, so the plot a saved session starts on does not save again until the player leaves it.

`SessionChoose` (`00427c30`) lists up to seven sessions over 265, 70 with items 35 pixels apart, title `Saved sessions`, fonts 4 and 3, each as `%s   [%d h %d mn %d sec] ` with the help `Energy : %d\nShield : %d\nDifficulty : %s ...\n`, then a gap when there are fewer than seven, and `Back`, the default. The cursor starts at the screen's middle.

## Port

`Source/Game/Menu` draws and steps a menu; `Source/Main.cpp` runs the main menu, the game menu, the questions, the session menu, Options, Extras, the credits and the slide show. The menus print English throughout. `--level` and `--start` skip the main menu at startup.

`Source/Game/Session` keeps the port's own session files, since the port does not model carried items as objects: `default.txt` and `s000.txt` onwards in `%APPDATA%\OpenVyruz\session\<session-name>` on Windows and `$XDG_DATA_HOME/OpenVyruz/session/<session-name>` (or `~/.local/share/...`) on Linux, where `<session-name>` comes from the archive's `VZCONF.TXT`, else `Player`. A file holds the level, the clock, energy and shield, the player (pose, inventory with ammo and keys, weapon slots) and, for a session saved during play, every object in the writer's order with its `.3DO` index or class, position, altitude, direction, wreck, energy, shield and door state. Loading replaces the level's objects with these, as loading the original's `.3DO` would; removed objects are left out except the ones carried keys came from. `--verify-game` checks that every level comes back from a session and that a saving plot saves once, for its cost, after 5.5 seconds.

Not ported yet: the option menus behind Options (the port follows the detail flags from `vzconf.txt` but has no Gamma FX, and keeps the default keys, Normal difficulty and English), the score row's own brightening (the port draws its text twice, additively), the intro, the demo's pause, speed toggle, inventory and help actions, the exit explosions, the menu sounds, the "Licensed to" line, the session thumbnails, patrol points and other attribute blocks in sessions, and reading and writing the original's session folders (a todo: then both games would share one save folder).
