#!/usr/bin/env bash
# Run the native desktop build (Linux; macOS untested) in a window, logging to
# $MC3_WORK/logs/.
#
#   tools/run_linux.sh                        play: window, keyboard/controller
#   GDB=1 tools/run_linux.sh                  under gdb (break sub_XXXXXXXX works:
#                                             every lifted function is a C symbol)
#   HEADLESS=1 tools/run_linux.sh             no window (xvfb); pair with
#                                             RECOMP_GL_DUMP=frames/f,60 and
#                                             RECOMP_PAD_SCRIPT for scripted runs
#   SECS=120 tools/run_linux.sh               stop after N seconds
#
# Any RECOMP_* / MC3_* variable passes straight through, e.g.
#   RECOMP_KEYBOARD=1 RECOMP_WATCHDOG_SECS=30 tools/run_linux.sh
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mc3_mac_native "$0" "$@"

SUFFIX=""
[ "${VULKAN:-0}" = "1" ] && SUFFIX="-vk"
[ "${DEBUG:-0}" = "1" ] && SUFFIX="$SUFFIX-dbg"
BIN="${BIN:-$MC3_WORK/build-$MC3_HOST$SUFFIX/$MC3_APP}"
[ -x "$BIN" ] || mc3_fail "no binary at $BIN (make build)"
# A run of a stale binary looks exactly like the fix did not work.
STALE=$(find "$MC3_REPO/src" "$MC3_REPO/config" "$XBOXRECOMP_DIR/src" "$MC3_GEN_DIR" \
            -newer "$BIN" -type f \( -name '*.c' -o -name '*.h' \) 2>/dev/null | head -3)
if [ -n "$STALE" ]; then
    echo "warning: $BIN is older than its sources, e.g.:" >&2
    echo "$STALE" | sed 's/^/  /' >&2
    echo "  rebuild first: make build run   (continuing in 5 s; Ctrl-C to stop)" >&2
    sleep 5
fi
mc3_need_game

mkdir -p "$MC3_WORK/logs"
LOG="$MC3_WORK/logs/run_$(date +%Y%m%d_%H%M%S).log"
export MC3_GAME_DIR
# The title's drive letters and partition images under work/, not ~/.local
# (Partition1\TDATA and \UDATA map into the game folder; see src/main.c).
export MC3_SAVE_DIR="${MC3_SAVE_DIR:-$MC3_WORK/save}"
mkdir -p "$MC3_SAVE_DIR"
# Relative outputs (frame dumps, progcache.bin) land in the workspace.
cd "$MC3_WORK"

CMD=("$BIN")
if [ -n "${SECS:-}" ]; then
    if command -v timeout >/dev/null; then
        CMD=(timeout --signal=KILL "$SECS" "${CMD[@]}")
    elif command -v gtimeout >/dev/null; then              # macOS + coreutils
        CMD=(gtimeout --signal=KILL "$SECS" "${CMD[@]}")
    else                                                   # macOS: perl's alarm
        CMD=(perl -e 'alarm shift; exec @ARGV' "$SECS" "${CMD[@]}")
    fi
fi
if [ "${HEADLESS:-0}" = "1" ]; then
    [ "$MC3_HOST" = linux ] || mc3_fail "HEADLESS=1 needs xvfb: Linux only"
    CMD=(xvfb-run -a "${CMD[@]}")
fi

if [ "${GDB:-0}" = "1" ]; then
    # gdb stops at the fault itself, before the crash reporter runs:
    # `bt` gives the host chain (lifted sub_XXXXXXXX frames),
    # `call print_guest_state()` gives the guest's.
    exec gdb -q -ex run --args "$BIN"
fi
echo "log: $LOG"
"${CMD[@]}" 2>&1 | tee "$LOG"
