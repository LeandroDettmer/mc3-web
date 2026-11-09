#!/usr/bin/env bash
# Record a run: the exact frames the game presents and the exact audio it
# sends to the speakers, turned into a video.
#
#   tools/record.sh [seconds]        (default 60; make record SECS=n EVERY=k)
#
#   WINDOW=1   run in a window (the GPU via virgl) instead of headless
#              (llvmpipe, software rendering: slower, so timing differs)
#   VIDEO=0    audio only: no frame dumps, which cost disk time per frame
#
# Writes work/capture/<time>/:
#   run.log                  the run's log
#   f00000.bmp ...           every EVERY-th presented frame (default 1), as the
#                            game rendered it -- independent of window size.
#                            Kept with KEEP=1; deleted after encoding otherwise.
#   audio.wav                what the game sent to the speakers (48 kHz stereo)
#   capture.mp4              frames + audio (needs ffmpeg: sudo apt install ffmpeg)
#
# Headless: nothing to watch, nothing to close. Disk: a 640x480 frame is about
# 1.2 MB as BMP; 60 s at 30 fps is about 2 GB before encoding.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

SECS="${1:-${SECS:-60}}"
EVERY="${EVERY:-1}"
BIN="${BIN:-$MC3_WORK/build-linux/$MC3_APP}"
[ -x "$BIN" ] || mc3_fail "no binary at $BIN (make build)"
mc3_need_game

OUT="$MC3_WORK/capture/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT" "$MC3_WORK/save"
export MC3_GAME_DIR MC3_SAVE_DIR="${MC3_SAVE_DIR:-$MC3_WORK/save}"
# The audio dump is written once its buffer is full: stop it before the run
# is killed.
AUDIO_SECS=$(( SECS > 15 ? SECS - 10 : SECS ))
[ "${VIDEO:-1}" = "0" ] || export RECOMP_GL_DUMP="$OUT/f,$EVERY"
export RECOMP_AUDIO_DUMP="$OUT/audio.raw,0,$AUDIO_SECS"
export RECOMP_FPS_LOG=1
cd "$MC3_WORK"

if [ "${WINDOW:-0}" = "1" ]; then
    echo "recording ${SECS}s in a window -> $OUT"
    timeout --signal=KILL "$SECS" "$BIN" > "$OUT/run.log" 2>&1 || true
else
    echo "recording ${SECS}s headless (every ${EVERY} frame(s)) -> $OUT"
    xvfb-run -a timeout --signal=KILL "$SECS" "$BIN" > "$OUT/run.log" 2>&1 || true
fi

FRAMES=$(find "$OUT" -name 'f*.bmp' | wc -l)
echo "captured $FRAMES frame(s)"
[ -f "$OUT/audio.raw" ] && echo "captured $(( $(stat -c %s "$OUT/audio.raw") / 192000 )) s of audio"

if ! command -v ffmpeg >/dev/null; then
    echo "no ffmpeg: frames and audio.raw kept as they are (sudo apt install ffmpeg)"
    exit 0
fi
if [ -f "$OUT/audio.raw" ]; then
    ffmpeg -loglevel error -y -f s16le -ar 48000 -ac 2 -i "$OUT/audio.raw" "$OUT/audio.wav" \
        && rm -f "$OUT/audio.raw"
fi
if [ "$FRAMES" -gt 0 ]; then
    # Frame rate as captured: frames written x EVERY over the recorded time.
    FPS=$(awk -v n="$FRAMES" -v e="$EVERY" -v s="$SECS" 'BEGIN { r = n * e / s; if (r < 1) r = 1; printf "%.2f", r / e }')
    AUDIO_IN=()
    [ -f "$OUT/audio.wav" ] && AUDIO_IN=(-i "$OUT/audio.wav" -c:a aac -b:a 192k)
    ffmpeg -loglevel error -y -framerate "$FPS" -pattern_type glob -i "$OUT/f*.bmp" \
        "${AUDIO_IN[@]}" -c:v libx264 -crf 12 -pix_fmt yuv420p -shortest "$OUT/capture.mp4" \
        && echo "video: $OUT/capture.mp4 (${FPS} fps)"
    [ "${KEEP:-0}" = "1" ] || find "$OUT" -name 'f*.bmp' -delete
fi
