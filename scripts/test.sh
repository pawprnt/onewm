#!/bin/bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_WM="${REPO_DIR}/build/onewm"
BIN_BOOT="${REPO_DIR}/build/onewm-boot"
DATA_DIR="${ONEWM_DATA_DIR:-$REPO_DIR/data}"
WM_LOG="/tmp/onewm-test.log"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

MODE="${1:-auto}"

die() { echo -e "${RED}$*${NC}" >&2; exit 1; }

require_bin() { [[ -x "$1" ]] || die "$(basename "$1") not built. Run: ninja -C ${REPO_DIR}/build"; }

case "$MODE" in
    headless)
        require_bin "$BIN_WM"
        echo -e "${CYAN}onewm headless smoke test${NC}"
        WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 timeout 3 "$BIN_WM" >/dev/null 2>&1 && rc=0 || rc=$?
        if [[ $rc -eq 124 || $rc -eq 0 ]]; then
            echo -e "${GREEN}headless OK (ran 3s, no crash)${NC}"
        else
            echo -e "${RED}headless FAILED (exit $rc)${NC}"
            exit $rc
        fi
        ;;
    nested)
        require_bin "$BIN_WM"
        if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
            echo -e "${CYAN}onewm nested (wayland parent)${NC}"
            WLR_BACKENDS=wayland WLR_LIBINPUT_NO_DEVICES=1 "$BIN_WM"
        elif [[ -n "${DISPLAY:-}" ]]; then
            echo -e "${CYAN}onewm nested (x11 parent)${NC}"
            WLR_BACKENDS=x11 "$BIN_WM"
        else
            die "No WAYLAND_DISPLAY or DISPLAY. Run from a graphical session, or use: $0 headless"
        fi
        ;;
    boot)
        require_bin "$BIN_BOOT"
        [[ -n "${WAYLAND_DISPLAY:-}" ]] || die "No WAYLAND_DISPLAY. The boot cutscene is a Wayland client — run from a Wayland session, or use: $0 combo"
        echo -e "${CYAN}onewm-boot in current session (WAYLAND_DISPLAY=$WAYLAND_DISPLAY)${NC}"
        echo -e "${CYAN}watch the cutscene; press any key to skip, or wait for it to finish.${NC}"
        ONEWM_DATA_DIR="$DATA_DIR" "$BIN_BOOT"
        ;;
    combo)
        require_bin "$BIN_WM"
        require_bin "$BIN_BOOT"
        if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
            PARENT=wayland
        elif [[ -n "${DISPLAY:-}" ]]; then
            PARENT=x11
        else
            die "No graphical session found (need WAYLAND_DISPLAY or DISPLAY)."
        fi
        echo -e "${CYAN}spawning onewm (nested, $PARENT backend) then onewm-boot${NC}"
        rm -f "$WM_LOG"
        WLR_BACKENDS="$PARENT" WLR_LIBINPUT_NO_DEVICES=1 "$BIN_WM" >"$WM_LOG" 2>&1 &
        wm_pid=$!
        sd=""
        for _ in $(seq 1 50); do
            sd="$(grep -oP 'WAYLAND_DISPLAY=\K\S+' "$WM_LOG" 2>/dev/null | head -1)"
            [[ -n "$sd" ]] && break
            sleep 0.1
        done
        [[ -n "$sd" ]] || { kill "$wm_pid" 2>/dev/null; die "onewm did not expose a WAYLAND_DISPLAY (see $WM_LOG)"; }
        echo -e "${CYAN}onewm on $sd — running cutscene inside it${NC}"
        WAYLAND_DISPLAY="$sd" ONEWM_DATA_DIR="$DATA_DIR" "$BIN_BOOT" || true
        kill "$wm_pid" 2>/dev/null
        ;;
    auto)
        if [[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]]; then
            exec "$0" nested
        else
            exec "$0" headless
        fi
        ;;
    *)
        echo "Usage: $0 [headless|nested|boot|combo|auto]"
        echo "  headless  onewm compositor smoke test (no display)"
        echo "  nested    run onewm compositor inside current session"
        echo "  boot      run onewm-boot cutscene in current Wayland session"
        echo "  combo     spawn onewm (nested) and run the cutscene inside it"
        exit 1
        ;;
esac
