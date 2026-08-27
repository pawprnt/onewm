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

executable(
  'onewm',
  'src/compositor/onewm.c',
  layer_shell_server_hdr,
  dependencies: [wlroots_dep, wayland_server_dep, xkbcommon_dep],
  include_directories: inc,
  c_args: ['-DWLR_USE_UNSTABLE'],
  install: true,
)

executable(
  'onewm-boot',
  'src/clients/boot.c',
  proto_src,
  proto_hdr,
  dependencies: [wayland_client_dep, cairo_dep, pango_dep, fontconfig_dep],
  include_directories: inc,
  install: true,
)

executable(
  'onewm-panel',
  'src/clients/panel.c',
  proto_src,
  proto_hdr,
  dependencies: [wayland_client_dep, cairo_dep, pango_dep, fontconfig_dep],
  include_directories: inc,
  install: true,
)

executable(
  'onewm-desktop',
  'src/clients/desktop.c',
  proto_src,
  proto_hdr,
  dependencies: [wayland_client_dep, cairo_dep, pango_dep, fontconfig_dep, zlib_dep],
  include_directories: inc,
  install: true,
)

executable(
  'onewm-wallpapers',
  'src/clients/selectors.c',
  proto_src,
  proto_hdr,
  dependencies: [wayland_client_dep, cairo_dep, pango_dep, fontconfig_dep, zlib_dep],
  include_directories: inc,
  install: true,
)

executable(
  'onewm-themes',
  'src/clients/selectors.c',
  proto_src,
  proto_hdr,
  dependencies: [wayland_client_dep, cairo_dep, pango_dep, fontconfig_dep, zlib_dep],
  include_directories: inc,
  install: true,
)

executable(
  'onewm-filemanager',
  'src/clients/filemanager.c',
  proto_src,
  proto_hdr,
  dependencies: [wayland_client_dep, cairo_dep, pango_dep, fontconfig_dep],
  include_directories: inc,
  install: true,
)

install_subdir(
  'data',
  install_dir: get_option('datadir') / 'onewm',
)
