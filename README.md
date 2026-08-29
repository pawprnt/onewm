# OneWm

A wlroots-based Wayland compositor and boot experience that recreates the
**Oneshot: World Machine Edition** desktop as a real Linux session, rendered
1:1 from the game's own drawing code.

> Note: the binary and package name is `onewm` (all lowercase) for Wayland /
> compositor and package-manager compatibility. "OneWm" is the project and
> brand name.

## Single binary

Everything ships as **one executable** — including all assets (logo, sound,
fonts, wallpapers, themes). `onewm` with no arguments starts the compositor
and automatically spawns the desktop and taskbar. Subcommands run the
individual layer-shell clients (used internally and for previewing):

| Command             | Purpose                                                     |
|---------------------|-------------------------------------------------------------|
| `onewm`             | Compositor + auto-spawned desktop & taskbar (the full WM).  |
| `onewm boot`        | World Machine boot / POST cutscene (full-screen).            |
| `onewm panel`       | Taskbar (window list, clock, click-to-focus).               |
| `onewm desktop`     | Desktop icons + wallpaper.                                  |
| `onewm filemanager` | The World Machine "file browser" window.                    |
| `onewm filemanager --wallpapers` | Wallpaper manager — reuses the file browser; pick a wallpaper to apply it live. |
| `onewm filemanager --themes`    | Theme manager — reuses the file browser; pick a theme to apply it live. |
| `onewm wallpapers`  | (legacy) standalone wallpaper selector.                     |
| `onewm themes`      | (legacy) standalone theme selector.                         |

The in-game **Wallpapers** and **Themes** desktop icons launch
`onewm filemanager --wallpapers` / `--themes`. The file manager doubles as the
wallpaper and theme picker: choosing an entry writes the selection to
`~/.config/onewm/wallpaper` (or `theme`) and signals the desktop to re-apply it
live. The standalone `onewm wallpapers` / `onewm themes` selectors remain as an
alternative.

Set `ONEWM_NO_AUTOSPAWN=1` to start only the compositor.

Assets are embedded into the binary at build time (see
`scripts/build/embed_assets.py`). At startup `asset_bootstrap()` extracts them
to a cache dir under `$XDG_RUNTIME_DIR/onewm-assets-$UID` (or `/tmp`) when
`ONEWM_DATA_DIR` is not already set. You can still point at external assets
with `ONEWM_DATA_DIR`, and add your own wallpapers via `general.wallpaper_dir`
(see below). Set `ONEWM_NO_EMBED=1` to disable extraction and require an
external data dir.

## Building

```bash
./build.sh                 # install build deps (pacman/apt/dnf/zypper/xbps/apk) + meson + ninja
# or manually:
meson setup build
meson compile -C build
sudo meson install -C build
```

`build.sh` detects your package manager, installs/upgrades the build
dependencies (wlroots-0.20, wayland, xkbcommon, cairo, pango, fontconfig,
zlib, meson, ninja, gcc, python3, binutils), then configures and compiles.
Installing puts `onewm` and the data files on the system. The asset embedder
(`scripts/build/embed_assets.py`) is invoked automatically by the build.

## Configuration (`onewm.lua`)

Config uses a Lua-ish, Hyprland-style syntax (comments with `--` or `#`).
The example at `config/onewm.lua` is installed to `/usr/share/onewm/onewm.lua`
and is also read from `$XDG_CONFIG_HOME/onewm/onewm.lua` or
`$ONEWM_CONFIG`.

```lua
general = {
  theme        = "purple",  -- purple|blue|teal|green|yellow|red|pink|orange|white|rainbow
  wallpaper    = "catwalks",
  icon_dir     = "/path/to/icons",
  panel_height = 30,
  transparency = 100,       -- 20..100 (% opaque) for unfocused windows
  data_dir     = "/usr/share/onewm",
  wallpaper_dir = "/path/to/my/wallpapers",  -- optional external wallpapers
}

keybind("SUPER+ENTER", "exec alacritty")
keybind("SUPER+Q",     "kill")
keybind("SUPER+E",     "exec onewm filemanager")
keybind("SUPER+ESCAPE","exit")

windowrule("opacity", "0.95")
```

- **Keybind controls** — `SUPER`/`WIN`/`LOGO`, `SHIFT`, `CTRL`, `ALT` modifiers;
  actions: `exec <shell>`, `kill` (close focused), `exit` (quit WM).
- **Window rules** — currently `opacity` (session-wide, mirrors
  `general.transparency`); reserved hooks for future per-window rules.
- **Transparency** — unfocused toplevels are dimmed to the configured opacity.
- **External wallpapers** — `general.wallpaper_dir` is exposed to clients as
  `ONEWM_WALLPAPER_DIR`; any PNGs there are listed alongside the embedded ones.

## Toolkit theming (GTK / KDE / Qt)

`onewm` derives native toolkit color schemes from the active OneWM theme and
applies them automatically at startup (and again whenever you switch themes in
the Theme Selector). This makes GTK3/4, KDE and Qt applications follow the
World Machine palette instead of a default theme.

```bash
onewm theme-apply            # re-apply the current theme to GTK/KDE/Qt now
onewm theme-apply red        # apply a specific theme by id
```

Files written (under `$HOME`):

- `~/.config/gtk-3.0/gtk.css` and `~/.config/gtk-4.0/gtk.css`
- `~/.local/share/color-schemes/onewm-<theme>.colors` + `~/.config/kdeglobals`
- `~/.config/Kvantum/onewm-<theme>.kvconfig` (best-effort for Qt/Kvantum)

All colors come from `themes/themes_metadata.json` — the same source the WM
chrome uses. See `docs/THEMING.md`.

> **Safety:** `onewm theme-apply` writes a `<file>.bak` of any existing config
> (e.g. `~/.config/kdeglobals`) *before* overwriting it, so your previous
> settings are always preserved.

Theme colors, panel chrome, file-manager grid, selector thumbnails and desktop
icon selection boxes all follow the constants extracted from the game's
decompiled source (see `docs/design-spec.md`).

## Scripts

| Script                          | Purpose                                                  |
|---------------------------------|----------------------------------------------------------|
| `build.sh`                      | Install build deps + meson/ninja (root-level helper).    |
| `scripts/build/embed_assets.py` | Pack `data/` + themes into the binary at build time.     |
| `scripts/test-launch.sh`        | Build (optional) and launch onewm **nested** in the current session. |

`scripts/test-launch.sh` auto-detects whether the host is a **Wayland**
(`WLR_BACKENDS=wayland`) or **X11** (`WLR_BACKENDS=x11`) session and selects
the matching wlroots backend:

```bash
scripts/test-launch.sh            # build + launch nested (skips GTK/KDE theming)
scripts/test-launch.sh --no-build # launch the existing build
scripts/test-launch.sh --theme    # also apply toolkit theming
```

Common flags: `--no-replace` (don't kill a previous nested instance),
`--workspace N` (jump to workspace N first on Hyprland), `--log [FILE]`.

## Usage

```bash
onewm                      # start the full WM on a tty / DRM backend
scripts/test-launch.sh     # quick nested preview in your current session
```

Game assets bundled in `data/` are **not** covered by the GPL; see
`ASSETS_LICENSE.md` for ownership and attribution.

## License

Source code is licensed under the GNU GPL-3.0-or-later — see `LICENSE`.
Game assets bundled in `data/` are **not** covered by that license; see
`ASSETS_LICENSE.md`.
