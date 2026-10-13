# HUD

Traced from `Vyruz.exe` 1.31.01 in Ghidra.

## Panel

`PanelLoad` (`00434d40`) (called from `VzRun`) loads `Data\Panel\panel.gaf` and `v-panel.gaf`, falling back to `..\Common\Data\Panel\`; only Common has them. It starts one animation instance for each of 26 slots, where slot N plays animation N of its GAF. Slots 3 (`screen`), 7 to 10 (the border bars) and 22 (`black_square`) come from `v-panel.gaf`, the rest from `panel.gaf`. The panel mode (session `+0x2578`) starts at 5, the fullest.

`PanelUpdate` (`00435000`) runs twice:

- With its draw flag clear after every game step, following the object update pass. It advances the animated slots: `score2`, `NRJ1` while the shield is low, the weapon lights, and the infos light while a turret has locked on.
- With the flag set once per frame, after the objects. It draws the panel.

Every part is placed from a screen corner (session `+0xf84` and `+0xf88`), so the layout follows the view size. In mode 5 it draws, in this order:

| Part | Anchor | Content |
|---|---|---|
| Score | 0, 0 | `score2` (frame 1, then its current frame), `scorebas`; money as `%08d $` in font 1 at 36, 30 |
| Bars | width / 2, 0 | `NRJ1` frame 1, plus its current frame while shield / max is at most 0.01; `shield` and `armor` clipped to `trunc(width * value / max)` as the inclusive right edge of the bitmap (pgbmp `FUN_0044ed60`) |
| Weapons | width - 1, 0 | `Weapons1` frame 1; `Weapons3`, or `Weapons4` while the player's acceleration (`+0x1a4`) differs from its class value; `Weapons2`, flickering with its own random stream while firing; the selected weapon's icon (session `+0x22b8`) at -60, 70; ammo as `%08d` in font 1 at -107, 9 |
| Speed | 0, height - 1 | `speed2`, `speedtop`, `speedside`; the radar dots; speed as `%03d` in font 2 at 105, -4 |
| Screen | width - 1, height - 1 | `screen` in draw mode 1 (5 with option flag `0x10`); in font 3, the player's X and Y as `%04d` at -107 and -72, -70, `KILLED %d` at -60, `ENEMY %d` at -50, and the time as `%02d:%02d:%02d` at -40, all from x -107; `infos` frame 1 (its current frame while locked), `infosbas` |

- **Speed** is the player's speed magnitude clamped to ±999, multiplied by 100 and taken modulo 1000.
- **Kills** are object `+0x26c`. `AreaDamage` and `AmmoHit` count an object there when the player's shot finishes it off.
- **Enemies** are session `+0x2094`. `EngineActivateObjects` counts every seeker or tracker that hunts the player, and every generator of one; each one's death lowers the count.
- **Option flags:** with flag `0x20` four bars frame the screen. With flag 8, "WARNING : YOU ARE LOCKED" and "DANGER : SHIELD IS LOW" appear as floating messages at 100, 90 and 100, 100.
- **Big radar:** while it is on (session `+0x1ff0`), it costs 30 money in each pass, and runs out below 30.

## Radar

`PanelDrawRadar` (`00436080`) plots single pixels into the radar beside the speedometer. It takes objects within 15 columns and 30 rows of the player's cell and within 960 pixels. It leaves out ammo, FX, gates, things, passable objects and items without a key. Each dot is at 72 + dx / 32, height - 59 + dy / 32, in palette colour 0xfa for the player, 0xfc for a lift, 0xfb for a key or code, and 0xf9 otherwise. The legend reads "RED: ENEMY, YELLOW: KEY/CODE, BLUE: LIFT".

## Fonts

`FNT_` entries hold bitmap fonts whose glyphs are the GAF's GOB bitmaps. See [DataFormats.md](DataFormats.md#fnt_---fonts). `FUN_0044c3c0` selects font N (1-based), `FUN_0044db70` sets the cursor and `FUN_0044b0e0` prints a formatted string.

- A tilde prints as a percent sign. A space advances by the font's advance.
- Each glyph's bitmap is placed so that its trimmed top-left lands at the cursor plus the glyph's offset. Its overlay bitmap follows.
- A proportional font (flag 1) advances by the bitmap width plus the character spacing; others by the fixed advance.
- Flag 4 draws a shadow pass first, in the shadow draw mode and colour, shifted by the shadow offset. None of the panel's three fonts uses it.

## Port

`Source/Pg/Font` loads and lays out fonts. `Source/Game/Panel` keeps the slots and returns the sprites to draw. The window draws them after the objects at the 640x480 corners.

Not ported yet:

- The weapon icon.
- The weapon light flicker and the acceleration light.
- The locked and low-shield messages, and floating messages in general (`FUN_004315f0`).
- The big radar and its money drain.
- The border bars.
- The other panel modes, and the key that switches them.
- The radar palette: the dots use the legend's colours, and green for the player.
