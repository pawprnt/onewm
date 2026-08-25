# onewm — Design Spec

Recreation of the OneShot: World Machine Edition desktop ("The World Machine") as a
real Linux session. This document is the acceptance criteria for every visual and
behavioral decision. Reference material paths at the bottom.

## 1. Theme system

Source of truth: the game's `gamedata/twm/themes_metadata.json` (schema preserved in
`themes/themes_metadata.json`).

- Every theme = exactly 3 colors: `primary`, `primaryVariant`, `background`.
- `background` is always pure black `#000000`.
- All chrome assets (borders, buttons, icons, text) are drawn WHITE/GRAYSCALE and
  tinted at render time by multiplying with the theme color. No color lives in
  textures.
- `primaryVariant` is the darker shade used for pressed/inactive/secondary states.
- 11 stock themes: purple, blue, teal(cyan), green, yellow, red, pink, orange,
  white, rainbow.
- `rainbow` = hue cycles over time in code; stored colors are the start value.
- Default theme: purple — `primary #9664FF` (150,100,255), `primaryVariant #6442A5`
  (100,66,165).

## 2. Palette (default purple)

| Role | Hex |
|---|---|
| Background (everything) | `#000000` |
| Primary accent (borders, titlebars, text, boot text) | `#9664FF` |
| Primary variant (dim/pressed) | `#6442A5` |
| Foreground on light surfaces | `#FFFFFF` |

## 3. Typography

| Use | Font |
|---|---|
| UI labels, window titles | Volter (Goldfish) Regular |
| Terminal / BIOS / monospace | Terminus Medium |

Both ship with the game (`temp/extracted/fonts/`); OneBoot also vendors copies in
`assets/`.

## 4. Window chrome (server-side decorations)

- Square corners everywhere. No rounding, no shadows.
- Border: 1px primary-colored outline; the WME "circuit" pattern
  (`the_world_machine/fullscreen_border*.png`, grayscale) is used for fullscreen /
  emphasized frames.
- Titlebar: left-aligned title in Volter, primary color text on black; height ~24px.
- Window buttons: `_` (minimize), `▢` (maximize), `✕` (close) as white pixel glyphs
  tinted primary — asset: `the_world_machine/window_buttons.png` (also two arrow
  glyphs used for swap/pin actions).
- Cursor: `the_world_machine/cursor.png` (+ bulb_anim variants).

## 5. Boot sequence (milestone 1 cutscene)

Script source of truth: `temp/oneshot-wme/gamedata/loc/en/twm/bios{,2,3}.txt`.

Markup dialect:
- `@WAIT <s>` — pause before printing next char (used for animated `...`).
- `@SCANSOUNDS` — hook: scan real sound files, print names line by line.
- `@SOUNDS` — substituted with count found.

Layout (from reference video 0:26–0:41, 1080p):
- Top-left: TWM Labs logo (`the_world_machine/logo.png`).
- Top-right: version string `v1.24.12.22.8`.
- Text block: left-aligned, Terminus, primary color on black, starts ~1/3 down,
  one blank line between sections.

Sequence:
1. `TWMBIOS(C)X021 TWM ENTERPRISE, INC.`
2. `XIDON 2290-B ACPI BIOS Revision 1043` / `CPU: YTEC(R) Y-4430M CPU @ 1.650GHz`
3. `Checking LPRAM...` → `2048KB OK`
4. `Detecting SCSI Devices...` → 3 SCSI lines
5. `Auto-detecting Mass Storage...` → `512MB MTD500100593-3B1`
6. `Scanning sounds...` → `Scan complete. Found N sounds.` → loading names
7. `All sounds loaded.` → `Loading system textures:` → `All system textures loaded.`
8. `Finishing Boot Sequence...`
9. Title card: OneBoot `logo_full.png` ("The World Machine").

For OneBoot the dynamic hooks mirror REAL system events (systemd units reached,
mkinitcpio progress) instead of game assets.

## 6. Modes

Same binary, different sessions:

- `desktop` — full session: workspaces, panel, launcher, apps.
- `boot` — kiosk: boot cutscene → installer TUI focus, minimal chrome.

## 7. Reference material

| What | Path |
|---|---|
| Game install (scripts, loc, XNB) | `/nyaa/coding/temp/oneshot-wme/` |
| Decoded assets (PNG/TTF) | `/nyaa/coding/temp/extracted/` |
| BIOS scripts | `temp/oneshot-wme/gamedata/loc/en/twm/bios*.txt` |
| Theme metadata | `temp/oneshot-wme/gamedata/twm/themes_metadata.json` |
| Boot animation recording | `/home/fox/Videos/Recordings/recording_20260824_15-49-39.mp4` (0:26–0:41 = POST sequence) |
| Community reference (skin pack, measurements only) | https://github.com/pdn6606/TWM-xfce (no license — do not ship) |
