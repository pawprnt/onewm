# Maintainer: foxinwinter <you@example.com>
pkgname=onewm
pkgver=0.1.0
pkgrel=1
pkgdesc="wlroots Wayland compositor and World Machine boot cutscene (Oneshot WME recreation)"
arch=('x86_64' 'aarch64')
url="https://github.com/pawprnt/onewm"
license=('GPL3')
# wlroots-0.20 is required (pkg-config name wlroots-0.20); provide it via the
# official wlroots package or an AUR build such as wlroots0.20.
depends=('wayland' 'wayland-protocols' 'cairo' 'pango' 'fontconfig' 'libxkbcommon' 'wlroots0.20')
makedepends=('meson' 'ninja' 'git')
source=("git+https://github.com/pawprnt/onewm.git#tag=v${pkgver}")
sha256sums=('SKIP')

prepare() {
  cd "onewm"
  # Game assets are copyrighted (Future Cat) and must not be redistributed via
  # AUR. The cutscene degrades gracefully without them. See ASSETS_LICENSE.md.
  rm -f data/boot/logo.png \
        data/boot/logo_full.png \
        data/sfx/twm_startup.wav \
        data/fonts/volter.ttf \
        data/fonts/Volter.ttf
}

build() {
  arch-meson "onewm" build
  meson compile -C build
}

package() {
  meson install -C build --destdir "$pkgdir"
  install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
  install -Dm644 ASSETS_LICENSE.md "$pkgdir/usr/share/licenses/$pkgname/ASSETS_LICENSE.md"
}
