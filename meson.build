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
xkbcommon_dep = dependency('xkbcommon')

inc = include_directories('src')

executable(
  'onewm',
  'src/onewm.c',
  dependencies: [wlroots_dep, wayland_server_dep, xkbcommon_dep],
  include_directories: inc,
  c_args: ['-DWLR_USE_UNSTABLE'],
  install: true,
)
