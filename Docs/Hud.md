# HUD

Traced from `Vyruz.exe` 1.31.01 in Ghidra.

## Panel

`PanelLoad` (`00434d40`) (called from `VzRun`) loads `Data\Panel\panel.gaf` and `v-panel.gaf`, falling back to `..\Common\Data\Panel\`; only Common has them. It starts one animation instance for each of 26 slots, where slot N plays animation N of its GAF. Slots 3 (`screen`), 7 to 10 (the border bars) and 22 (`black_square`) come from `v-panel.gaf`, the rest from `panel.gaf`. The panel mode (session `+0x2578`) starts at 5, the fullest.

`PanelUpdate` (`00435000`) runs twice:

- With its draw flag clear after every game step, following the object update pass. It advances the animated slots of the parts the mode shows: `score2`, `NRJ1` while the shield is low, `Weapons3` or `Weapons4`, the weapon lights, the weapon icon, and the infos light while the player is locked.
- With the flag set once per frame, after the messages. It draws the panel; `PanelDrawBigRadar` follows with the map.

Mode 0 shows nothing. From mode 1 on it draws, in this order, each part from the mode in the second column:

| Part | Modes | Anchor | Content |
|---|---|---|---|
| Borders | 1 to 5 | 0, 0 | With option flag `0x20` (`tv-borders`): `hbar2` stretched to the view's width, `vbar2` and `vbar1` (at width - 1) stretched to its height, and `hbar1` (at height - 1) to its width; the stretch is the view size over the bitmap's |
| Score | 3 to 5 | 0, 0 | `score2` (frame 1, then its current frame), `scorebas`; money as `%08d $` in font 1 at 36, 30 |
| Bars | 2 to 5 | width / 2, 0 | `NRJ1` frame 1, plus its current frame while shield / max is at most 0.01; `shield` and `armor` clipped to `trunc(width * value / max)` as the inclusive right edge of the bitmap (pgbmp `FUN_0044ed60`) |
| Weapons | 3 to 5 | width - 1, 0 | `Weapons1` frame 1; `Weapons3`, or `Weapons4` while the player's `Speed-max` (`+0x1a4`) differs from its class value; the lights `Weapons2`; the selected weapon's icon (session `+0x22b8`) at -60, 70; ammo as `%08d` in font 1 at -107, 9 |
| Speed | 4 and 5 | 0, height - 1 | `speed2`, `speedtop`, `speedside`; the radar dots; speed as `%03d` in font 2 at 105, -4 |
| Screen | 5 | width - 1, height - 1 | `screen` in draw mode 1 (5, half translucent, with option flag `0x10`, `translucent-panel`); unless a surprise is in effect (object `+0xd4`), in font 3, the player's X and Y as `%04d` at -107 and -72, -70, `KILLED %d` at -60, `ENEMY %d` at -50, and the time as `%02d:%02d:%02d` at -40, all from x -107; `infos` frame 1 (its current frame while locked), `infosbas` |
| Text | 1 | 70, 20 | `SHIELD : %d / 100      ENERGY : %d / 100      AMMO : %d      CASH : %d $` in panel font 3 (session `+0x20a4` and `+0x20a8`), the percentages as whole numbers times 100 over the whole maximum |

After the modes, in any mode, a surprise in effect prints each carried surprise's time left as `%d:%d` (minutes, then seconds of the minute) in panel font 3 at width - 100, height - 30.

- **Modes:** `PanelLoad` starts at mode 5 with a cycling step of -1 (session `+0x2580`). `action-cycle-panels` (Tab, `PanelCycleModes` at `0043ba30`) adds the step and reflects at 0 and 5 (-1 becomes 1, 6 becomes 4); with Left Shift held it subtracts the step instead. Reaching 0 or 5 without Shift turns the step round, so Tab alone walks 5, 4, 3, 2, 1, 0, 1, ... and back. EngineRunLevel keeps the mode (`+0x257c`) when a level ends and starts the next one with it.
- **Weapon lights:** while the bound ammo stock (object `+0x248`) holds rounds, each pass pushes the Misc random stream (session `+0x259c`), draws `RandomBelow(53)` and pops it; the lights advance or draw only when that is 1. With the weapon empty they play steadily. `EngineCreateRandomStreams` (`0043c7a0`) makes five streams at each level start, Surprise, Creation, Panel, Vibration and Misc, each seeded `0xfeabcefe`, `0xabcdefda` like the shared generator.
- **Locked:** `EngineUpdateObjects` clears session `+0x20` bit `0x200000` before its objects update, and `SeekerUpdate` sets it whenever a seeker on its route segment engages the player (a turret's engage branch, a moving seeker's segment step).

- **Speed** is the player's speed magnitude clamped to ±999, multiplied by 100 and taken modulo 1000.
- **Kills** are object `+0x26c`. `AreaDamage` and `AmmoHit` count an object there when the player's shot finishes it off.
- **Enemies** are session `+0x2094`. `EngineActivateObjects` counts every seeker or tracker that hunts the player, and every generator of one; each birth of a hunter raises it and each death lowers it.
- **Warnings:** with option flag 8, "WARNING : YOU ARE LOCKED" (modes 1 to 5) and "DANGER : SHIELD IS LOW" (modes 2 to 5, with the bars) appear as typed floating messages in panel font 3 at 100, 90 and 100, 100, or renew to 500 ms the one already there.

## Map

`action-map` (M, `MapToggleWithPanel` at `0043b570`) calls `MapToggle` (`00436460`): an open map closes; a closed one opens only with at least 21000 cash, else `MapNeedsCash` (`00435fe0`) says `%s,\n   you need MORE Cash\n     to see the Map !` with the player's name at width / 2 - 200, height / 2 for 2 seconds in v-bonus font 6. On success the panel switches to mode 1 and keeps the previous mode (`+0x257c`) for when the map closes. Escape and `action-pause` close an open map instead of their own action, and the level's end closes it without giving the mode back.

Opening renders the level into a screen-sized bitmap (session `+0x1ff0`): `GobRenderMap` (`0046b120`) stretches every block of every `MAP_` layer, first to last, into its cell's share of the bitmap, which starts black; pixels at 0x800 are transparent. `PanelDrawBigRadar` (`004362a0`) runs after the panel's draw pass: it copies the bitmap over the view, then draws, scaled by the view size over the map's, the player's icon (animation 21, `hud`), each lift's (23, `lift`; 24, `asc`, in French) and each saving plot's (25, `saveuk`; 26, `savfr`) from session lists `+0x25b0` and `+0x25b4`, each from its last entry.

Port deviation: the port's map lies over the game instead of covering it. It shows only the cells that hold blocks, keeps the level's shape, fits the view inside a 40-pixel margin and is centred. Its empty pixels are transparent, and the game behind it is blurred with a dual Kawase filter (four halvings) and darkened to `0x707078`. Where the device cannot blit the swapchain image, the game is only darkened.

While the map is open, every pass of `PanelUpdate` in modes 1 to 5 takes 30 cash; below 30 it closes the map, says it needs cash, and leaves the panel in mode 1.

## Radar

`PanelDrawRadar` (`00436080`) plots single pixels into the radar beside the speedometer. It takes objects within 15 columns and 30 rows of the player's cell and within 960 pixels. It leaves out ammo, FX, gates, things, passable objects and items without a key. Each dot is at 72 + dx / 32, height - 59 + dy / 32, in palette colour 0xfa for the player, 0xfc for a lift, 0xfb for a key or code, and 0xf9 otherwise. The legend reads "RED: ENEMY, YELLOW: KEY/CODE, BLUE: LIFT". `BitmapPlotIndexed` (`00467a10`) converts the index through the colour map that `ColorMapInit` (`0046eb60`) fills: entries 0 to 7 and 0xf8 to 0xff from `DAT_0051bfd8` and `DAT_0051bff8`, which make 0xf9 to 0xfc pure red, green, yellow and blue.

## Fonts

`FNT_` entries hold bitmap fonts whose glyphs are the GAF's GOB bitmaps. See [DataFormats.md](DataFormats.md#fnt_---fonts). `PgFontSelect` (`0044c3c0`) selects font N (1-based) of a GAF and takes its draw mode and colour, `PgFontSetCursor` (`0044db70`) sets the cursor and `PgFontPrintf` (`0044b0e0`) prints a formatted string through `PgFontPrint` (`0044bd70`).

- A tilde prints as a percent sign. A space advances by the font's advance.
- With word wrap on (the story and ending pages), `PgFontPrint` measures each word first: when one of its bitmaps would pass the clip's right edge, measured from the cursor minus one with each glyph's X offset, the word starts a new line at the clip's left edge.
- The text page of the story and the ending (`ShowTextPage`, `004027f0`, callback `00402640`) starts the cursor at the font's baseline (font record `+0x14`) below the page's top, so v-bonus font 4's first line sits 17 pixels lower than the page's top suggests. It closes on a key or mouse button, or after its timeout, counted down by the frame time (`FUN_00450d50`): 30 seconds for the story, none for the ending.
- A line break goes back to the starting column and down by the line height, plus the line gap for a proportional font. Word wrap at the clip edge would start the line at the clip's left instead, but `FUN_0043be00` turns it off when a level starts.
- `PgFontSetColor` (`0044cc30`) combines a colour into the font's with `ColorCombine` (`0044f310`): each diffuse channel becomes `c - (256 - m) * c / 256`, the speculars add with saturation and the alphas multiply. A draw mode set before `PgFontSelect` is overwritten by the font's.
- Each glyph's bitmap is placed so that its trimmed top-left lands at the cursor plus the glyph's offset. Its overlay bitmap follows.
- A proportional font (flag 1) advances by the bitmap width plus the character spacing; others by the fixed advance.
- Flag 4 draws a shadow pass first, in the shadow draw mode and colour, shifted by the shadow offset. None of the panel's three fonts uses it.

## Panel help

While `action-pannel-help` (F2, a fixed binding) holds session `+0x20d0` bit `0x800`, which `EngineRunLevel` clears as a level starts, `FUN_00434130` runs last in each frame's draw pass: it names the panel's parts in panel font 3 (draw mode 1), each with a leader line in palette colour 0xfa (the radar's green, `FUN_00434040`) that ends in a filled circle of radius 1 in `0x00ff00` (`FUN_00434100`), which plots a plus. Energy, Shield and Money sit at width / 2 - 72 with their leaders drawn first; Ammunitions, Low Speed, High Speed and Current Weapon at width / 2 + 30, and the Danger Warning, Coordinates, Destroyed and Remaining Enemies and Elapsed Time near the bottom, each leader starting 8 pixels after its text; the radar's legend at 158, height - 170. The port keeps the coordinates as a table (`FPanel::DrawHelp`).

## Weapon icon

`PanelLoadWeaponIcons` (`00434910`) loads one icon per weapon slot from `Common\Data\Object\Item`: `icon-gatling`, `icon-grenade`, `icon-rocket`, `icon-fire`, `icon-saw`, `icon-missile`, `icon-laser`, `icon-pulsar` and `icon-mine`, each playing its first animation at session `+0x22b8 + 72 * (slot - 1)`. The update pass advances only the icon of the slot in hand, and the draw pass draws it at width - 61, 70 over the weapon block's screen.

## Messages

`g_Messages` (`005237d0`) is the list of floating messages. `MessageAddText` (`004312e0`) adds a text with its position, velocity, life, a placement mode, a world flag, a typewriter flag, a font and a colour; `MessageCreate` (`004311a0`) can first remove every message at the same whole-pixel position (mode 1) or give up when there is one (mode 2). A new message takes the slot that `g_MessageFreeSlot` (`0050d734`) last recorded, else it is appended.

`MessagesUpdate` (`004315f0`) runs twice per frame: with its draw flag clear after each step's game update, and with it set after the objects and before the panel. It visits the slots from the last to the first, and each empty slot it sees becomes the free slot. For each message, in both passes:

- A message whose life is not above the step is removed. Only the update pass takes the step off the life.
- The position moves by `velocity / (14 / step)` (session `+0x2744`). World messages are drawn at the position minus the scroll; the others at the position.
- A typewriter message shows its first N characters, and N grows by one each time the 50 ms typing timer runs out.

The texts and where they come from:

| Text | Position | Velocity, life | Font | Colour |
|---|---|---|---|---|
| `Energy +%d`, `Money +%d`, `Shield +%d`, `%s +%d` (ammo name) | item - 32, item + 0, each later line 20 higher; world, typed | 0, -1; 2 s | bonus 6 | `0x80ff20`, `0xffc040`, `0xff4020`, white |
| `Key #%d !`, `Code #%d : part %d !`, `Code #%d complete !` | view centre - 100 | 2 s | bonus 6 | `0x00ff00` |
| `Using Key #%d !`, `Using Code #%d !` (Destroy-key took the player's key) | view centre - 100 | 2 s | bonus 6 | white |
| `Wait, lift will come.`; `Lift needs key #%d !` or `Lift needs full code !` 50 further left | lift - 150, lift - 32; world | 2 s | bonus 6 | white |
| `Gate needs key #%d !`, `Gate needs full code #%d !` | gate - 200, gate - 32; world | 2 s | bonus 6 | white |
| `%d $` (reward), then the object's name at -32, -32 | wreck; world, typed | 0, -1.5 and 0, -1; 2 s | bonus 2, then 6 | `0xf0ff10`, white |
| `You are so professional %s !` and two more lines, when the last hunter dies; the original never pays the "Real Killer Bonus : +100000 $" it prints, the port does | 70, height / 2 - 102 | 10 s | bonus 6 | white |
| `DANGER : SHIELD IS LOW` (draw pass of the panel, option flag 8) | 100, 100; typed, its life renewed to 500 ms while shown | 500 ms | panel 3 | white |

Every one replaces a message at its position. The lift and gate texts come from the player's contact with the closed door in `EngineUpdateObjects`; "Cannot take lift while invisible !" and "Cannot open gates while invisible !" replace them while the player is invisible.

## Port

`PanelDraw` (`00435000`) would offset every part of the panel by the pair at session `+0x21f8` when it is not zero, but nothing writes it, so the panel never shakes; the screen shake moves only the view.

`MessagesUpdate` also draws a type 1, a still bitmap, but nothing creates one: `MessageCreate` is called only by `MessageAddText` (type 3) and `MessageAddAnimation` (`004310b0`, type 2), so the port leaves it out.

`Source/Pg/Font` loads and lays out fonts. `Source/Game/Panel` keeps the slots and returns the sprites to draw. `Source/Game/Message` keeps the floating messages, which the panel prints. The window draws them after the objects at the view's corners, 640x480 in the original and as wide as the screen in the port's wide view; the sprite shader multiplies by the diffuse colour and adds the specular one.

Not ported yet:

- The map's picture is point-sampled rather than stretched block by block, so a cell edge may round a pixel differently. The map itself plays nothing: `MapToggle` and the icon animations (21, 23 and 25) have no sounds.
