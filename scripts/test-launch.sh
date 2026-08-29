#!/bin/sh
# Test-launch onewm nested inside the current graphical session.
# Automatically detects whether the parent session is Wayland or X11 and picks
# the matching wlroots backend (WLR_BACKENDS=wayland | x11).
#
# Usage: scripts/test-launch.sh [--build] [--no-build] [--no-install] [--no-replace] [--theme]
#                                [--workspace N] [--log [FILE]] [--data DIR]
#
#   --build       run ./build.sh (install deps + full configure + compile)
#   --no-build    skip building entirely (assume build/ is current)
#   --no-install  passed to ./build.sh: skip installing build dependencies
#   --no-replace  don't kill a previous nested instance first
#   --theme       actually apply GTK/KDE/Qt theming (default: ONEWM_NO_THEME_APPLY=1)
#   --workspace N jump to workspace N first (Hyprland, Wayland only)
#   --log [FILE] tee output to a log (default /tmp/onewm-test.log)
#   --data DIR   override the embedded-asset data dir
set -e

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LOG=/tmp/onewm-test.log
WS=3
NO_BUILD=0
RUN_BUILD_SH=0
NO_INSTALL=0
NO_REPLACE=0
APPLY_THEME=0
DATA_DIR=""

while [ $# -gt 0 ]; do
	case "$1" in
		--build)      RUN_BUILD_SH=1; NO_BUILD=0 ;;
		--no-build)   NO_BUILD=1 ;;
		--no-install) NO_INSTALL=1 ;;
		--no-replace) NO_REPLACE=1 ;;
		--theme)      APPLY_THEME=1 ;;
		--workspace)  shift; WS="$1" ;;
		--log)        shift; [ $# -gt 0 ] && LOG="$1" ;;
		--data)       shift; DATA_DIR="$1" ;;
		*) echo "unknown arg: $1" >&2; exit 1 ;;
	esac
	shift
done

cd "$REPO_DIR"

# Build.
#   default     -> incremental ninja (auto meson setup on first run)
#   --no-build  -> skip building entirely
#   --build     -> run ./build.sh (install deps + full configure + compile)
if [ "$NO_BUILD" -eq 0 ]; then
	if [ "$RUN_BUILD_SH" -eq 1 ]; then
		echo "Running build.sh"
		BUILD_ARGS=""
		[ "$NO_INSTALL" -eq 1 ] && BUILD_ARGS="--no-install"
		./build.sh $BUILD_ARGS
	else
		if [ ! -f build/build.ninja ]; then
			echo "meson setup build"
			meson setup build
		fi
		ninja -C build
		echo "Build OK"
	fi
fi

# Detect the host session type.
if [ -n "$WAYLAND_DISPLAY" ]; then
	BACKEND=wayland
	echo "Host session: Wayland (WAYLAND_DISPLAY=$WAYLAND_DISPLAY)"
elif [ -n "$DISPLAY" ]; then
	BACKEND=x11
	echo "Host session: X11 (DISPLAY=$DISPLAY)"
else
	echo "No Wayland or X11 session detected; cannot launch nested." >&2
	exit 1
fi

# Stop a previous nested instance so relaunching doesn't stack compositors.
PIDFILE="$REPO_DIR/build/.nested.pid"
if [ "$NO_REPLACE" -eq 0 ] && [ -f "$PIDFILE" ]; then
	OLD=$(cat "$PIDFILE" 2>/dev/null || true)
	if [ -n "$OLD" ] && kill -0 "$OLD" 2>/dev/null; then
		echo "stopping previous onewm ($OLD)"
		kill "$OLD" 2>/dev/null || true
		sleep 0.5
	fi
	rm -f "$PIDFILE"
fi

# On Hyprland, jump to a dedicated workspace first (Wayland only).
if [ "$BACKEND" = wayland ] && [ -n "$HYPRLAND_INSTANCE_SIGNATURE" ] && command -v hyprctl >/dev/null 2>&1; then
	hyprctl dispatch "hl.dsp.focus({ workspace = $WS })" >/dev/null 2>&1 || true
	sleep 1
fi

cd build

ENV="WLR_BACKENDS=$BACKEND"
# Default to NOT touching the host GTK/KDE/Qt theme (see --theme to opt in).
[ "$APPLY_THEME" -eq 0 ] && ENV="$ENV ONEWM_NO_THEME_APPLY=1"
[ -n "$DATA_DIR" ] && ENV="$ENV ONEWM_DATA_DIR=$DATA_DIR"

echo "Launching onewm (nested, backend=$BACKEND)  log=$LOG"
echo "Ctrl-C to stop."

# Run with logging (tee) when --log given, else straight to the terminal.
if [ -n "$LOG" ]; then
	env $ENV ./onewm 2>&1 | tee "$LOG" &
	PIPE=$!
	ONEWM_PID=$(pgrep -P "$PIPE" 2>/dev/null | head -1)
	[ -z "$ONEWM_PID" ] && ONEWM_PID=$PIPE
else
	env $ENV ./onewm &
	ONEWM_PID=$!
fi
echo "$ONEWM_PID" > "$PIDFILE"

cleanup() { kill "$ONEWM_PID" 2>/dev/null || true; rm -f "$PIDFILE"; }
trap cleanup INT TERM

wait "$ONEWM_PID" || true
rm -f "$PIDFILE"
