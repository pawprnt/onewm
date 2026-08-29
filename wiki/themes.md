# themes

themes are defined in `themes/themes_metadata.json`, using the same 3-color schema as the game:

- `primary` — the main accent (window borders, titlebar glyphs)
- `primaryVariant` — a secondary tint
- `background` — usually black, like the world machine

every piece of chrome is drawn white/grayscale and tinted with these at render time, so you only ever pick three colors.

## selecting a theme

set the theme at launch with the `ONEWM_THEME` env var (a theme id from the metadata file):

    ONEWM_THEME=default ./build/onewm

or write the id to `~/.config/onewm/theme`:

    echo default > ~/.config/onewm/theme

onewm also applies the theme to gtk3, gtk4, kdeglobals and kvantum so the rest of the desktop matches. set `ONEWM_NO_THEME_APPLY=1` to skip that.

## adding a theme

add an entry to `themes/themes_metadata.json` with your three colors. keep `background` black to stay game-accurate.
