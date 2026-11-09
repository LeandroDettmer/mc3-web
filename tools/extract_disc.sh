#!/usr/bin/env bash
# Unpack an Xbox disc image (default.xbe + the data). Only needed for an ISO;
# an already-extracted disc just goes in the repo's game/ folder.
# Unpacks into $MC3_GAME_DIR (default: the repo's game/).
#
#   tools/extract_disc.sh path/to/mc3.iso
#
# Uses the toolkit's own XDVDFS reader (tools.xiso), which handles both plain
# XISO images and full Redump dumps (it finds the game partition itself).
# The build and the Switch both read the unpacked files, never the ISO.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mc3_python

ISO="${1:-}"
[ -n "$ISO" ] && [ -f "$ISO" ] || mc3_fail "usage: $0 <disc image>"
[ -e "$MC3_GAME_DIR/default.xbe" ] && mc3_fail "$MC3_GAME_DIR already has a default.xbe; move it away first"

mkdir -p "$MC3_GAME_DIR"
cd "$XBOXRECOMP_DIR"
"$MC3_PYTHON" -m tools.xiso ls "$ISO" | head -5 || true   # head closes the pipe early (pipefail)
"$MC3_PYTHON" -m tools.xiso unpack "$ISO" -o "$MC3_GAME_DIR"

[ -f "$MC3_GAME_DIR/default.xbe" ] || mc3_fail "no default.xbe after unpacking"
echo
echo "extracted to $MC3_GAME_DIR ($(du -sh "$MC3_GAME_DIR" | cut -f1))"
if [ -f "$MC3_GEN_DIR/recomp_funcs.h" ]; then
    echo "next: make run GAME=$MC3_GAME${EDITION:+ EDITION=$EDITION} (the editions in config/$MC3_GAME/xbe_editions.h share one lift)"
else
    echo "next: make lift GAME=$MC3_GAME"
fi
