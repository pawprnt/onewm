# configuration

onewm is configured through environment variables and a few files under `~/.config/onewm/`.

## environment variables

- `ONEWM_THEME` — theme id to load (falls back to the `theme` file below)
- `ONEWM_DATA_DIR` — where onewm looks for `themes_metadata.json` and assets
- `ONEWM_NO_THEME_APPLY` — set to `1` to skip applying the theme to gtk/kde

## config files

written to `~/.config/onewm/`:

- `theme` — the selected theme id
- `wallpaper` — the current wallpaper path (set by the file manager)

## assets

game assets are covered by the project's assets license. the palette and layout recreation are free to use.
