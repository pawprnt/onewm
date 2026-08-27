project('onewm', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
  ],
)

wlroots_dep = dependency('wlroots-0.20')
wayland_server_dep = dependency('wayland-server')
wayland_client_dep = dependency('wayland-client')
xkbcommon_dep = dependency('xkbcommon')
cairo_dep = dependency('cairo')
pango_dep = dependency('pangocairo')
fontconfig_dep = dependency('fontconfig')
zlib_dep = dependency('zlib')

wayland_scanner = find_program('wayland-scanner')
layer_shell_xml = files('protocols/wlr-layer-shell-unstable-v1.xml')
xdg_shell_xml = files('/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml')
gen_header = generator(wayland_scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['client-header', '@INPUT@', '@OUTPUT@'])
gen_code = generator(wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'])
proto_src = gen_code.process(layer_shell_xml, xdg_shell_xml)
proto_hdr = gen_header.process(layer_shell_xml, xdg_shell_xml)
layer_shell_server_hdr = custom_target('layer-shell-server-header',
  input: layer_shell_xml,
  output: 'wlr-layer-shell-unstable-v1-protocol.h',
  command: [wayland_scanner, 'server-header', '@INPUT@', '@OUTPUT@'])

inc = include_directories('include')

# --- Embedded assets: bake data/ + themes/themes_metadata.json into the binary
# as a single objcopy'd blob so the WM is self-contained (no external data dir
# required at runtime). asset_bootstrap() extracts it to a cache dir at startup.
python = find_program('python3')
asset_data = run_command('find',
  meson.project_source_root() / 'data',
  meson.project_source_root() / 'themes',
  '-type', 'f', check: true).stdout().strip().split('\n')
asset_data_files = files(asset_data)

asset_bin = custom_target('embed_assets_bin',
  output: 'assets.bin',
  depend_files: asset_data_files,
  command: [python, files('scripts/build/embed_assets.py'),
            meson.project_source_root() / 'data',
            meson.project_source_root() / 'themes',
            '@OUTPUT@'],
  build_by_default: true)

asset_obj = custom_target('embed_assets_obj',
  output: 'assets.o',
  input: asset_bin,
  command: ['objcopy', '-I', 'binary', '-O', 'elf64-x86-64', '-B', 'i386:x86-64',
            '--rename-section', '.data=.rodata,alloc,load,readonly,contents',
            '@INPUT@', '@OUTPUT@'],
  build_by_default: true)

# Single busybox-style binary. `startup` (no arg) runs the compositor; subcommands
# `boot|panel|desktop|filemanager|wallpapers|themes` run the matching client.
executable(
  'onewm',
  [
    'src/shared/dispatch.c',
    'src/shared/config.c',
    'src/shared/assets.c',
    asset_obj,
    'src/compositor/onewm.c',
    'src/clients/boot.c',
    'src/clients/panel.c',
    'src/clients/desktop.c',
    'src/clients/selectors.c',
    'src/clients/filemanager.c',
    'src/clients/theme_apply.c',
  ],
  layer_shell_server_hdr,
  proto_src,
  proto_hdr,
  dependencies: [
    wlroots_dep, wayland_server_dep, wayland_client_dep,
    xkbcommon_dep, cairo_dep, pango_dep, fontconfig_dep, zlib_dep,
  ],
  include_directories: inc,
  c_args: ['-DWLR_USE_UNSTABLE'],
  # client + server protocol objects both define identical interface symbols.
  link_args: ['-Wl,--allow-multiple-definition'],
  install: true,
)

install_subdir(
  'data',
  install_dir: get_option('datadir') / 'onewm',
)

install_data(
  'config/onewm.lua',
  install_dir: get_option('datadir') / 'onewm',
)
