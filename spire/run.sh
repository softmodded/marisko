#!/bin/bash
# spire — SP-1 Emulator launcher
# Builds firmware, starts Renode with the SP-1 platform and virtual GUI

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$DIR")"
FIRMWARE="$PROJECT_DIR/build/app/zephyr/zephyr.elf"

cleanup() {
    echo ""
    echo "[spire] shutting down..."
    kill $RENODE_PID 2>/dev/null
    kill $GUI_PID 2>/dev/null
    exit 0
}
trap cleanup INT TERM

if [ -z "$ZEPHYR_SDK_INSTALL_DIR" ]; then
    ZEPHYR_SDK_INSTALL_DIR="$(dirname "$PROJECT_DIR")/zephyr-sdk-0.17.0"
    if [ ! -d "$ZEPHYR_SDK_INSTALL_DIR" ]; then
        echo "[spire] ZEPHYR_SDK_INSTALL_DIR not set and default path not found" >&2
        echo "[spire] set it: export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.0" >&2
        exit 1
    fi
fi

echo "[spire] building firmware..."
cd "$PROJECT_DIR"
west build -b sp1 -d build app -- \
  -DBOARD_ROOT="$PROJECT_DIR" \
  -DZEPHYR_SDK_INSTALL_DIR="$ZEPHYR_SDK_INSTALL_DIR"

if [ ! -f "$FIRMWARE" ]; then
    echo "[spire] build failed — no $FIRMWARE" >&2
    exit 1
fi

echo "[spire] starting renode with sp-1 platform..."
if ! command -v renode &>/dev/null; then
    echo "[spire] renode not found — install it first:" >&2
    echo "  arch:  yay -S renode-bin" >&2
    echo "  mac:   brew install renode" >&2
    echo "  other: https://renode.io/#downloads" >&2
    exit 1
fi

# Start Renode, load platform, load firmware, start emulation
renode --console --port 3334 -e "
include @$DIR/sp1.repl
sysbus LoadELF @$FIRMWARE
sysbus.cpu VectorTableOffset 0x20000
start
" &
RENODE_PID=$!
sleep 2

echo "[spire] starting virtual device gui..."
python3 "$DIR/peripherals/sp1_gui.py" &
GUI_PID=$!
sleep 1

echo ""
echo "  +------------------------------------------+"
echo "  |         spire emulator ready             |"
echo "  |                                          |"
echo "  |  firmware:  $FIRMWARE"
echo "  |  renode:    localhost:3334               |"
echo "  |  gui:       virtual device window        |"
echo "  |                                          |"
echo "  |  press ctrl+c to exit                    |"
echo "  +------------------------------------------+"
echo ""

wait $RENODE_PID 2>/dev/null
wait $GUI_PID 2>/dev/null
