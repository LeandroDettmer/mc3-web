#!/usr/bin/env bash
# Upscale dumped textures into a replacement pack (see xboxrecomp/src/nv2a_gl/hd_pack.c).
#
#   tools/hd_upscale.sh [factor] [file ...]     factor 2 (default), 3 or 4
#   make hd-upscale [FACTOR=2] [MIN=64] [FILES="hd/dump/x.png ..."]
#
# Reads hd/dump/<hash>.png (written by a run with RECOMP_GL_TEX_DUMP=1, or
# make hd-dump) and writes hd/textures/<hash>.png, the same name: the
# renderer picks those up at its next start. Packs aggregate: a texture
# already in hd/textures/ is skipped (so one you edited by hand is never
# overwritten), and a later dump of another scene adds only new files.
#   files given     only those (paths or bare <hash>.png), redone even if done
#   MIN=<px>        skip textures smaller than this on both sides (default 4)
#   RESR=<path>     the upscaler binary (default work/tools/realesrgan/,
#                   fetched by make hd-tools for this OS)
#   MODEL=<name>    default realesrgan-x4plus (the photo model)
# Real-ESRGAN (realesrgan-ncnn-vulkan, BSD-3) upscales the colour 4x, fully
# opaque; tools/hd_png.py (Pillow) scales it to the factor and puts back the
# original alpha, only resized: MC3 keeps data in alpha (gloss, window glow),
# and Real-ESRGAN treated it as transparency -- grass, roads and facades came
# out black. A whole city at 4x costs a lot of memory on the Switch; 2x is a
# clear step there.
# Packs made from the game's own textures are the game's art: for your own
# use, not for shipping with the project.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

FACTOR="${1:-${FACTOR:-2}}"
[ $# -gt 0 ] && shift
case "$FACTOR" in 2|3|4) ;; *) mc3_fail "factor must be 2, 3 or 4" ;; esac
RESR="${RESR:-$MC3_WORK/tools/realesrgan/realesrgan-ncnn-vulkan}"
MODEL="${MODEL:-realesrgan-x4plus}"
MIN="${MIN:-4}"
SRC="$MC3_REPO/hd/dump"
DST="$MC3_REPO/hd/textures"
[ -x "$RESR" ] || mc3_fail "no upscaler at $RESR: make hd-tools (or RESR=<path>)"
PNG="$MC3_REPO/tools/hd_png.py"
PY="${PYTHON:-}"
if [ -z "$PY" ]; then
    for c in "$MC3_WORK/macvenv/bin/python3" "$MC3_WORK/venv/bin/python3" python3; do
        if "$c" -c 'import PIL' >/dev/null 2>&1; then PY="$c"; break; fi
    done
fi
[ -n "$PY" ] || mc3_fail "no Python with Pillow: python3 -m pip install pillow (or PYTHON=<path>)"
mkdir -p "$DST"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Image size (w h), with what this host has.
img_size() {
    if command -v sips >/dev/null; then
        sips -g pixelWidth -g pixelHeight "$1" 2>/dev/null | awk '/pixelWidth/ {w=$2} /pixelHeight/ {h=$2} END {print w, h}'
    else
        python3 -c 'import struct,sys; d=open(sys.argv[1],"rb").read(24); print(*struct.unpack(">II", d[16:24]))' "$1"
    fi
}

# What to do: the files given (forced), or every dump not yet in the pack.
FORCE=0
LIST=()
if [ $# -gt 0 ] || [ -n "${FILES:-}" ]; then
    FORCE=1
    read -ra EXTRA <<< "${FILES:-}"
    for f in "$@" ${EXTRA[@]+"${EXTRA[@]}"}; do
        [ -e "$f" ] || f="$SRC/$(basename "$f")"
        [ -e "$f" ] || mc3_fail "no such texture: $f"
        LIST+=("$f")
    done
else
    [ -d "$SRC" ] || mc3_fail "no $SRC: run the game once with RECOMP_GL_TEX_DUMP=1 (make hd-dump)"
    for f in "$SRC"/*.png; do
        [ -e "$f" ] || break
        [ -e "$DST/$(basename "$f")" ] && continue
        LIST+=("$f")
    done
fi
TOTAL=${#LIST[@]}
[ "$TOTAL" -gt 0 ] || { echo "nothing to upscale: every dumped texture is already in hd/textures"; exit 0; }
echo "upscaling $TOTAL texture(s) ${FACTOR}x with $MODEL into hd/textures"

done_n=0 skip_n=0 fail_n=0 i=0 t0=$(date +%s)
for f in "${LIST[@]}"; do
    i=$((i + 1))
    name="$(basename "$f")"
    read -r w h <<< "$(img_size "$f")"
    if [ -z "${w:-}" ] || [ "$w" -lt "$MIN" ] || [ "$h" -lt "$MIN" ]; then
        skip_n=$((skip_n + 1)); continue
    fi
    el=$(( $(date +%s) - t0 ))
    eta=""
    if [ "$done_n" -gt 0 ]; then
        left=$(( el * (TOTAL - i + 1) / done_n ))
        eta=$(printf ' ~%dm%02ds left' $((left / 60)) $((left % 60)))
    fi
    printf '[%d/%d] %s (%sx%s)%s\n' "$i" "$TOTAL" "$name" "$w" "$h" "$eta"
    # -m: the models folder beside the binary (it looks in the working
    # directory otherwise, and crashes when they are not there).
    if "$PY" "$PNG" split "$f" "$TMP/rgb.png" \
        && "$RESR" -i "$TMP/rgb.png" -o "$TMP/up.png" -n "$MODEL" -s 4 -m "$(dirname "$RESR")/models" >/dev/null 2>&1 \
        && "$PY" "$PNG" merge "$TMP/up.png" "$f" "$DST/$name" $((w * FACTOR)) $((h * FACTOR)); then
        done_n=$((done_n + 1))
    else
        fail_n=$((fail_n + 1))
        echo "  failed: $name" >&2
    fi
done
el=$(( $(date +%s) - t0 ))
printf 'upscaled %d texture(s) %sx into hd/textures in %dm%02ds (%d skipped, %d failed)%s\n' \
    "$done_n" "$FACTOR" $((el / 60)) $((el % 60)) "$skip_n" "$fail_n" \
    "$([ "$FORCE" = 1 ] && echo ', forced')"
