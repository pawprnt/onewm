#!/bin/sh
# onewm build script:
#   1. detect the system package manager
#   2. install + upgrade build dependencies to their newest version
#   3. meson setup build   (skipped if already configured)
#   4. ninja -C build
#
# System packages are installed with sudo. The wlroots pkg-config name is
# "wlroots-0.20"; on CachyOS/Arch that is provided by the "wlroots0.20" package.
# Adjust the wlroots package name for other distros if needed.
set -e

# ---- 0. parse arguments ----
NO_INSTALL=0
for a in "$@"; do
	case "$a" in
		--no-install) NO_INSTALL=1 ;;
		*) echo "Unknown argument: $a" >&2; exit 1 ;;
	esac
done

# ---- 1. detect package manager ----
PM=""
if   command -v pacman      >/dev/null 2>&1; then PM=pacman
elif command -v apt-get       >/dev/null 2>&1; then PM=apt
elif command -v dnf           >/dev/null 2>&1; then PM=dnf
elif command -v zypper        >/dev/null 2>&1; then PM=zypper
elif command -v xbps-install  >/dev/null 2>&1; then PM=xbps
elif command -v apk           >/dev/null 2>&1; then PM=apk
else
	echo "Unsupported package manager / distro." >&2
	exit 1
fi
echo "Detected package manager: $PM"

# ---- 2. install + upgrade build dependencies ----
if [ "$NO_INSTALL" -eq 0 ]; then
# meson.build dependency()/find_program() needs:
#   wlroots-0.20, wayland-server, wayland-client, xkbcommon, cairo,
#   pangocairo, fontconfig, zlib, wayland-scanner, wayland-protocols,
#   meson, ninja, gcc, python3, objcopy (binutils), pkgconf
case "$PM" in
	pacman)
		DEPS="wlroots0.20 wayland libxkbcommon cairo pango fontconfig zlib \
		      wayland-protocols meson ninja gcc python binutils pkgconf"
		# -S upgrades only the listed packages to newest (no full system upgrade)
		sudo pacman -Syu --needed --noconfirm $DEPS
		;;
	apt)
		DEPS="libwlroots-dev libwayland-dev libxkbcommon-dev libcairo2-dev \
		      libpango1.0-dev libfontconfig1-dev zlib1g-dev wayland-protocols \
		      meson ninja-build gcc pkg-config python3 binutils"
		sudo apt-get update
		sudo apt-get install -y $DEPS
		;;
	dnf)
		DEPS="wlroots-devel wayland-devel libxkbcommon-devel cairo-devel \
		      pango-devel fontconfig-devel zlib-devel wayland-protocols-devel \
		      meson ninja-build gcc python3 binutils pkgconf"
		sudo dnf install -y $DEPS
		;;
	zypper)
		DEPS="wlroots-devel wayland-devel libxkbcommon-devel cairo-devel \
		      pango-devel fontconfig-devel zlib-devel wayland-protocols-devel \
		      meson ninja gcc python3 binutils pkgconf"
		sudo zypper install -y $DEPS
		;;
	xbps)
		DEPS="wlroots-devel wayland-devel libxkbcommon-devel cairo-devel \
		      pango-devel fontconfig-devel zlib-devel wayland-protocols \
		      meson ninja gcc python3 binutils pkgconf"
		sudo xbps-install -S -u $DEPS
		;;
	apk)
		DEPS="wlroots-dev wayland-dev libxkbcommon-dev cairo-dev pango-dev \
		      fontconfig-dev zlib-dev wayland-protocols meson ninja gcc \
		      python3 binutils pkgconf"
		sudo apk add $DEPS
		;;
esac
else
	echo "Skipping dependency install (--no-install)."
fi

# ---- 3. meson setup (only if not already configured) ----
if [ ! -f build/build.ninja ]; then
	echo "meson setup build"
	meson setup build
fi

# ---- 4. ninja build ----
ninja -C build
echo "Build complete."
