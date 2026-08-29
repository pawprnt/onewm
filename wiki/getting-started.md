# getting started

onewm is a recreation of the oneshot: world machine edition desktop as a real wayland compositor, written in c on wlroots 0.20.

it draws the white/grayscale "world machine" chrome and tints everything with a runtime theme, just like the game.

## building

    meson setup build
    ninja -C build

## running

under an existing wayland session (nested):

    WLR_BACKENDS=wayland ./build/onewm

headless smoke test (no display needed):

    scripts/test.sh headless

## next steps

- read [themes](themes.md) to change the look
- read [configuration](config.md) for env vars and config files
