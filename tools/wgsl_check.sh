#!/usr/bin/env bash
# wgsl_check.sh -- validate WGSL modules offline (ADR 0011, docs/webgpu-plan.md).
#
#   tools/wgsl_check.sh FILE.wgsl|DIR ...
#
# Each file (a directory: every *.wgsl in it) goes through naga (naga-cli:
# `cargo install naga-cli`) or, without it, tint (Dawn's CLI). The first one
# found on PATH is used; WGSL_CHECK=naga|tint picks one. Neither installed:
# says so and exits 0, so a build without them still passes (the browser
# compiles every module again and logs errors with the shader id).
# Exit status: the number of rejected files (capped at 125).
set -u

tool=${WGSL_CHECK:-}
if [ -z "$tool" ]; then
    if command -v naga >/dev/null 2>&1; then tool=naga
    elif command -v tint >/dev/null 2>&1; then tool=tint
    fi
fi
if [ -z "$tool" ]; then
    echo "wgsl_check: neither naga nor tint on PATH; skipped (cargo install naga-cli)"
    exit 0
fi

files=()
for a in "$@"; do
    if [ -d "$a" ]; then
        for f in "$a"/*.wgsl; do [ -e "$f" ] && files+=("$f"); done
    else
        files+=("$a")
    fi
done
if [ ${#files[@]} -eq 0 ]; then
    echo "usage: $0 FILE.wgsl|DIR ..." >&2
    exit 2
fi

bad=0
for f in "${files[@]}"; do
    case $tool in
        naga) out=$(naga "$f" 2>&1) ;;                      # no output file: parse + validate
        tint) out=$(tint --format wgsl -o /dev/null "$f" 2>&1) ;;
        *)    echo "wgsl_check: unknown WGSL_CHECK=$tool" >&2; exit 2 ;;
    esac
    if [ $? -ne 0 ]; then
        bad=$((bad + 1))
        echo "FAIL $f"
        printf '%s\n' "$out" | sed 's/^/  /'
    fi
done
echo "wgsl_check ($tool): $((${#files[@]} - bad)) of ${#files[@]} modules ok"
[ $bad -gt 125 ] && bad=125
exit $bad
