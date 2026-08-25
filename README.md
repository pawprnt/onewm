# OneWm

A wlroots-based Wayland compositor and boot experience that recreates the
**Oneshot: World Machine Edition** desktop as a real Linux session.

> Note: the binary and package name is `onewm` (all lowercase) for Wayland /
> compositor and package-manager compatibility. "OneWm" is the project and
> brand name.

## Components

- `onewm` — the wlroots Wayland compositor.
- `onewm-boot` — the World Machine boot cutscene client. It renders the BIOS /
  POST screen (the small TWM logo, version string, and scrolling console text)
  as a full-screen layer-shell surface, with the startup sound effect.

## Building

```bash
git clone https://github.com/pawprnt/onewm.git
cd onewm
meson setup build
meson compile -C build
```

## Installing

```bash
sudo meson install -C build
```

This installs `onewm`, `onewm-boot`, and the data files to
`/usr/share/onewm`.

## Usage

Run `onewm` to start the compositor. The boot cutscene can also be launched
independently against an existing Wayland session (e.g. for previewing / theming):

```bash
onewm-boot            # run the cutscene on the current Wayland session
scripts/test.sh boot  # convenience wrapper (also has a `combo` mode)
```

Required assets (logo, sound, fonts) live under `/usr/share/onewm`. See
`ASSETS_LICENSE.md` for ownership and attribution.

## License

Source code is licensed under the GNU GPL-3.0-or-later — see `LICENSE`.
Game assets bundled in `data/` are **not** covered by that license; see
`ASSETS_LICENSE.md`.
