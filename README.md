# OneWm

A wlroots-based Wayland compositor and boot experience that recreates the
**Oneshot: World Machine Edition** desktop as a real Linux session, rendered
1:1 from the game's own drawing code.

> Note: the binary and package name is `onewm` (all lowercase) for Wayland /
> compositor and package-manager compatibility. "OneWm" is the project and
> brand name.

## Single binary

Everything ships as **one executable**. `onewm` with no arguments starts the
compositor and automatically spawns the desktop and taskbar. Subcommands run
the individual layer-shell clients (used internally and for previewing):

| Command                | Purpose                                                     |
|------------------------|-------------------------------------------------------------|
| `onewm`                | Compositor + auto-spawned desktop & taskbar (the full WM). |
| `onewm boot`           | World Machine boot / POST cutscene (full-screen).           |
| `onewm panel`          | Taskbar (window list, clock, click-to-focus).              |
| `onewm desktop`        | Desktop icons + wallpaper.                                  |
| `onewm filemanager`    | The World Machine "file browser" window.                    |
| `onewm wallpapers`     | Wallpaper selector.                                         |
| `onewm themes`         | Theme selector.                                             |

Set `ONEWM_NO_AUTOSPAWN=1` to start only the compositor.

## Building

```bash
meson setup build
meson compile -C build
sudo meson install -C build
```

Installs `onewm` and the data files to `/usr/share/onewm`.

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

Theme colors, panel chrome, file-manager grid, selector thumbnails and desktop
icon selection boxes all follow the constants extracted from the game's
decompiled source (see `docs/design-spec.md`).

## Usage

```bash
onewm                 # start the full WM on a tty / DRM or nested backend
scripts/test.sh combo # headless/nested preview harness
```

Required assets (logo, sound, fonts, wallpapers, themes) live under
`/usr/share/onewm`. See `ASSETS_LICENSE.md` for ownership and attribution.

## License

Source code is licensed under the GNU GPL-3.0-or-later — see `LICENSE`.
Game assets bundled in `data/` are **not** covered by that license; see
`ASSETS_LICENSE.md`.
