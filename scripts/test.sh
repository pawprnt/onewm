#!/bin/bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${REPO_DIR}/build/onewm"

RED='\033[0;31m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
NC='\033[0m'

MODE="${1:-auto}"

if [[ ! -x "$BIN" ]]; then
    echo -e "${RED}onewm not built. Run: ninja -C ${REPO_DIR}/build${NC}"
    exit 1
fi

case "$MODE" in
    headless)
        echo -e "${CYAN}onewm headless smoke test${NC}"
        WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 timeout 3 "$BIN" >/dev/null 2>&1 && rc=0 || rc=$?
        if [[ $rc -eq 124 || $rc -eq 0 ]]; then
            echo -e "${GREEN}headless OK (ran 3s, no crash)${NC}"
        else
            echo -e "${RED}headless FAILED (exit $rc)${NC}"
            exit $rc
        fi
        ;;
    nested)
        if [[ -n "${WAYLAND_DISPLAY:-}" ]]; then
            echo -e "${CYAN}onewm nested (wayland parent)${NC}"
            WLR_BACKENDS=wayland WLR_LIBINPUT_NO_DEVICES=1 "$BIN"
        elif [[ -n "${DISPLAY:-}" ]]; then
            echo -e "${CYAN}onewm nested (x11 parent)${NC}"
            WLR_BACKENDS=x11 "$BIN"
        else
            echo -e "${RED}No WAYLAND_DISPLAY or DISPLAY. Run from a graphical session, or use: $0 headless${NC}"
            exit 1
        fi
        ;;
    auto)
        if [[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]]; then
            exec "$0" nested
        else
            exec "$0" headless
        fi
        ;;
    *)
        echo "Usage: $0 [headless|nested|auto]"
        exit 1
        ;;
esac
