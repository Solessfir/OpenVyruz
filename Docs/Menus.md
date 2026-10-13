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
| Options | `Video, Audio, Control, User settings, Language.` | `FUN_004229a0` |
| Extras | `Scores, Credits, Intro, Slide Show, Demo.` | `FUN_00427820` |
| Exit (default) | | "Exit Now ?" (Yes, bye. / No !, default Yes), with explosions over the menu (`MenuExplode` (`00421460`)) before leaving |

The cursor starts on Restart Level when there is a default session and on Start New Game otherwise. The difficulty names are Easy, Normal, Hard and Insane (`DifficultyName` (`00403310`)); the default is Normal.

## Game menu

Resume Game (the default), Options, Retry Level (only with a default session; otherwise a gap ends the list) and End Game. Retry Level asks "Retry level ?" as above and restarts from the default session; End Game asks "End this Game ?" (Yes / No, whose default is Yes) and returns to the main menu without the game-over image. A dead player's Escape ends the game at once with the game-over image.

## Sessions

`SessionWrite` (`0042ad70`) writes a session as `session\<player>\s%03d` or `session\<player>\default`: the level's objects as a `.3DO` file (`Level3doWrite` (`004163a0`), each object's changed fields by `Level3doWriteObject` (`00416730`)) and a `last.txt` line (`SessionWriteLast` (`0042b190`)). EngineRunLevel writes the default session as a level starts, unless the level came from a session; `ObjectUpdateLevelExit` writes it too. A `saving-plot` object counts 5.5 seconds with "Saving in %d second%s", takes its cost in cash ("Not enough Cash"), and writes a numbered session ("Saving Completed."); leaving it early says "No Saving." (`SavingPlotUpdate` (`0042a3b0`), `SavingPlotLeave` (`0042a650`)).

## Port

`Source/Game/Menu` draws and steps a menu; `Source/Main.cpp` runs the main menu, the game menu and the questions. `--level` and `--start` skip the main menu at startup. The default session is the level's path and the player brought into it, kept in memory. Not ported yet: saved sessions and save plots, Options and Extras, the idle demo, the exit explosions, the menu sounds and the "Licensed to" line.
