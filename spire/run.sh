#!/bin/bash
# spire — SP-1 Emulator launcher
# Starts Renode with the SP-1 platform and the virtual GUI

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

echo "[spire] starting renode with sp-1 platform..."
renode --console "$DIR/sp1.repl" &
RENODE_PID=$!
sleep 2

echo "[spire] starting virtual device gui..."
python3 "$DIR/peripherals/sp1_gui.py" &
GUI_PID=$!
sleep 1

echo ""
echo "  ╔══════════════════════════════════════╗"
echo "  ║         spire emulator ready         ║"
echo "  ║                                      ║"
echo "  ║  renode console: localhost:3334      ║"
echo "  ║  virtual device:  gui window         ║"
echo "  ║                                      ║"
echo "  ║  type 'q' then 'quit' in renode to   ║"
echo "  ║  stop.  press ctrl+c here to exit.   ║"
echo "  ╚══════════════════════════════════════╝"
echo ""

wait $RENODE_PID 2>/dev/null
wait $GUI_PID 2>/dev/null
