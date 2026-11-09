#!/usr/bin/env bash
# Where the time goes: a sampling profile of a headless run (Linux perf).
#
#   tools/profile.sh [seconds]      (default 40; make profile SECS=n)
#
# Samples every thread ~1000x a second and writes the hottest functions --
# lifted sub_XXXXXXXX, runtime, renderer, libc -- to
# work/logs/profile_<time>.txt (per thread, per library, per function).
# KEEP=1 keeps the perf data (work/logs/profile_<time>.data) for perf report.
#
# One-time setup (tools/setup_ubuntu.sh does it): perf for the running kernel,
#   sudo apt install linux-tools-$(uname -r) linux-tools-generic
# and permission to profile your own processes:
#   sudo sysctl kernel.perf_event_paranoid=1
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

SECS="${1:-${SECS:-40}}"
BIN="${BIN:-$MC3_WORK/build-linux/$MC3_APP}"
[ -x "$BIN" ] || mc3_fail "no binary at $BIN (make build)"
mc3_need_game
command -v perf >/dev/null && perf --version >/dev/null 2>&1 \
    || mc3_fail "perf not usable: sudo apt install linux-tools-\$(uname -r) linux-tools-generic"
if [ "$(cat /proc/sys/kernel/perf_event_paranoid)" -gt 1 ]; then
    mc3_fail "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid): sudo sysctl kernel.perf_event_paranoid=1"
fi

mkdir -p "$MC3_WORK/logs" "$MC3_WORK/save"
STAMP=$(date +%Y%m%d_%H%M%S)
DATA="$MC3_WORK/logs/profile_$STAMP.data"
OUT="$MC3_WORK/logs/profile_$STAMP.txt"
export MC3_GAME_DIR MC3_SAVE_DIR="${MC3_SAVE_DIR:-$MC3_WORK/save}"
cd "$MC3_WORK"

echo "profiling ${SECS}s headless -> $OUT"
xvfb-run -a perf record -q -F 999 -o "$DATA" -- \
    timeout --signal=KILL "$SECS" "$BIN" > "$MC3_WORK/logs/profile_$STAMP.run.log" 2>&1 || true
# Reports never fail the script: an empty section is information too.
set +e +o pipefail
report() { perf report -i "$DATA" --stdio --no-children "$@" 2>/dev/null | grep -v '^#' | grep -v '^$'; }
{
    echo "== threads: share of all samples, and how busy each was"
    echo "   (perf's 'pid' is the thread; busy = samples / (999 Hz x ${SECS} s))"
    perf report -i "$DATA" --stdio --no-children --sort pid -F overhead,sample,pid 2>/dev/null \
        | grep -v '^#' | grep -v '^$' | head -25 \
        | awk -v s="$SECS" '{ printf "%s  busy %5.1f%%\n", $0, 100 * $2 / (999 * s) }'
    echo
    echo "== where the samples land (binary or library; [unknown] = JIT code, e.g. llvmpipe)"
    report --sort dso | head -15
    echo
    echo "== hottest functions, all threads"
    report --sort symbol | head -40
    echo
    echo "== hottest functions per thread"
    report --sort pid,symbol | head -60
} > "$OUT"
grep -a -m1 '\[GL\] .*/' "$MC3_WORK/logs/profile_$STAMP.run.log" >> "$OUT"
grep -a '\[fps\]' "$MC3_WORK/logs/profile_$STAMP.run.log" >> "$OUT"
[ "${KEEP:-0}" = "1" ] || rm -f "$DATA"
echo "done: $OUT"
head -30 "$OUT"
