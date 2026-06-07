#!/bin/bash
# spire — SP-1 Emulator launcher
# Usage: ./run.sh [path/to/firmware.elf]
# If no argument given, tries ../build/app/zephyr/zephyr.elf

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"

cleanup() {
    echo ""
    echo "[spire] shutting down..."
    kill $RENODE_PID 2>/dev/null
    kill $GUI_PID 2>/dev/null
    exit 0
}
trap cleanup INT TERM

FIRMWARE="$1"
if [ -z "$FIRMWARE" ]; then
    FIRMWARE="$(dirname "$DIR")/build/app/zephyr/zephyr.elf"
fi

if [ ! -f "$FIRMWARE" ]; then
    echo "[spire] firmware not found: $FIRMWARE" >&2
    echo "[spire] usage: $0 [path/to/firmware.elf]" >&2
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

kill $(lsof -ti:3334) 2>/dev/null || true
sleep 1

renode --port 3334 -e "
include @$DIR/sp1.repl

sysbus LoadELF @$FIRMWARE
sysbus.cpu VectorTableOffset 0x20000

sysbus WriteDoubleWord 0x40000518 0x2
sysbus WriteDoubleWord 0x40000008 0x1
sysbus WriteDoubleWord 0x40000418 0x1
sysbus WriteDoubleWord 0x40000414 0x1
sysbus WriteDoubleWord 0x4000041C 0x2
sysbus WriteDoubleWord 0x40000104 0x1

sysbus.cpu PerformanceMode true

macro reset
\"\"\"
    sysbus LoadELF @$FIRMWARE
    sysbus.cpu VectorTableOffset 0x20000
    sysbus WriteDoubleWord 0x40000518 0x2
    sysbus WriteDoubleWord 0x40000008 0x1
    sysbus WriteDoubleWord 0x40000418 0x1
    sysbus WriteDoubleWord 0x40000414 0x1
    sysbus WriteDoubleWord 0x4000041C 0x2
    sysbus WriteDoubleWord 0x40000104 0x1
\"\"\"

start

showAnalyzer sysbus.gpio0
showAnalyzer sysbus.gpio1
" &
RENODE_PID=$!
sleep 3

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
