# GTK / KDE Theming Gameplan (deferred this iteration)

The WM chrome (taskbar, file manager, selectors, desktop icons) is already
themed from the active OneWM theme via `themes/themes_metadata.json`. This
document outlines how to extend that single source of truth to **native
toolkit applications** (GTK3/4 and Qt/KDE) so third-party windows match the
World Machine palette instead of falling back to Adwaita/Breeze.

Status: **implemented**. `onewm theme-apply` derives GTK3/4, KDE and Qt
color schemes from `themes/themes_metadata.json` and writes them under
`$HOME` (see README for the file list). It runs automatically at compositor
startup and is re-invoked by the Theme Selector on every theme change. The
notes below describe the design and the remaining optional refinements.

## Source of truth

`themes/themes_metadata.json` already exposes, per theme:

```
primary          (r,g,b)   – chrome accent
primaryVariant   (r,g,b)   – dimmer accent (borders, dividers)
background       (r,g,b)   – window / workspace background
rainbow          bool      – hue-cycling accent
```

We add `text`/`textVariant` if a theme needs explicit foreground colors; the
game renders text in `primary` on `background`, so those are the defaults.

## Mechanism

A new subcommand `onewm theme-apply` (or a hook the compositor calls on theme
change, e.g. via `ONEWM_THEME` env / a `theme` file watch) does:

1. Load the active theme's colors (reuse `load_theme`/JSON parse already in
   the clients).
2. Emit GTK and KDE/Qt config into the user's config dirs.
3. Signal running apps (or rely on toolkit live-reload) to pick them up.

### GTK3 / GTK4

Write `~/.config/gtk-3.0/gtk.css` and `~/.config/gtk-4.0/gtk.css`:

```css
@define-color wm_primary rgb(150,100,255);
@define-color wm_bg      rgb(0,0,0);
@define-color wm_variant rgb(100,66,165);

window, headerbar, .background { background-color: @wm_bg; color: @wm_primary; }
headerbar { background-color: @wm_variant; }
```

And set the session's `GTK_THEME` (or ship a thin `onewm-gtk` theme that only
overrides colors) so the palette applies without forcing a full theme swap.

### KDE / Qt (Qt5/Qt6, Kvantum)

- Write a KDE color scheme to
  `~/.local/share/color-schemes/onewm-{theme}.colors`:

  ```ini
  [General]
  Name=OneWM {theme}
  [Colors:Window]
  BackgroundNormal=0,0,0
  ForegroundNormal=150,100,255
  DecorationFocus=150,100,255
  [Colors:Button]
  BackgroundNormal=100,66,165
  ForegroundNormal=150,100,255
  ```

- For Qt apps without KDE running, also export `QT_COLOR_SCHEME`/set
  `QSG_` or use Kvantum: write a
  `~/.config/Kvantum/onewm-{theme}.kvconfig` mapped from the same colors.
- Export `QT_QPA_PLATFORMTHEME=qt5ct` (or `kdeglobals` symlink) so the scheme
  is honored.

### Rainbow theme

When `rainbow=true`, the compositor animates `rainbow_hue`. For toolkits we
either (a) periodically regenerate the scheme files on a timer, or (b) snap
the toolkit accent to the current hue at theme-apply time only. Option (b) is
the pragmatic first cut; (a) is a later enhancement.

## Open questions

- Whether to ship a full GTK/Kvantum *theme* (controls, not just colors) for a
  closer 1:1 match, vs. color-override only.
- Live-reload strategy for already-running apps (DBus `org.kde.GtkConfig` /
  `color-scheme` portal vs. restart).
- Mapping game `primary` (a mid purple) to both accent *and* foreground can
  hurt contrast on light themes (yellow/white); may need a derived
  foreground (e.g. lighten/darken) per theme.
