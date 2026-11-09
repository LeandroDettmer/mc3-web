#!/usr/bin/env bash
# Host backtraces of every thread after N seconds: what each thread is really
# doing -- running lifted code (sub_XXXXXXXX frames), waiting on the guest
# lock (xbox_gil_*), in a kernel bridge call, or in the renderer.
#
#   tools/host_stacks.sh [seconds]      (default 45; make hoststacks SECS=n)
#
# Runs headless under gdb, interrupts after the delay, prints `thread apply all
# bt` into work/logs/hoststacks_<time>.log, then ends the run. The guest-stack
# watchdog (make snapshot) shows where the *game* is; this shows why a thread
# is not moving, which the guest stack cannot.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

SECS="${1:-${SECS:-45}}"
BIN="${BIN:-$MC3_WORK/build-linux/$MC3_APP}"
[ -x "$BIN" ] || mc3_fail "no binary at $BIN (make build)"
mc3_need_game
command -v gdb >/dev/null || mc3_fail "gdb not installed (sudo apt install gdb)"

mkdir -p "$MC3_WORK/logs" "$MC3_WORK/save"
LOG="$MC3_WORK/logs/hoststacks_$(date +%Y%m%d_%H%M%S).log"
export MC3_GAME_DIR MC3_SAVE_DIR="${MC3_SAVE_DIR:-$MC3_WORK/save}"
cd "$MC3_WORK"

echo "running headless under gdb for ${SECS}s; backtraces -> $LOG"
# timeout (inside xvfb-run, so its child is gdb itself) sends SIGINT to gdb,
# which stops the program; -batch then runs the
# remaining commands and exits (killing it). Real-time signals some runtimes
# use for thread control pass straight through.
xvfb-run -a timeout --signal=INT "$SECS" gdb -q -batch \
    -ex 'set pagination off' \
    -ex 'handle SIGUSR1 SIGUSR2 SIGPIPE SIG32 SIG33 SIG34 SIG35 nostop noprint pass' \
    -ex run \
    -ex 'info threads' \
    -ex 'thread apply all bt 30' \
    --args "$BIN" > "$LOG" 2>&1 || true
echo "done: $(grep -c '^Thread ' "$LOG") threads in $LOG"
