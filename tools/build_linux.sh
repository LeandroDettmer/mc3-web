#!/usr/bin/env bash
# Build the desktop binary: Linux (the day-to-day debug build) or macOS
# (native, into work/build-mac*). Same lifted code and runtime as the
# Switch; opens an SDL2 window.
#
#   tools/build_linux.sh              OpenGL renderer, RelWithDebInfo
#   VULKAN=1 tools/build_linux.sh     Vulkan renderer (what the Switch ships)
#   DEBUG=1 tools/build_linux.sh      -O0 for the hand-written code (gdb stepping);
#                                     the lifted C stays at MC3_GEN_OPT
#   GEN_DEBUGINFO=1 ...               -g for the lifted C too (line-level stepping
#                                     in sub_XXXXXXXX; costs gigabytes of disk)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mc3_mac_native "$0" "$@"
mc3_check_lift_fresh

SUFFIX=""; ARGS=(-DMC3_VULKAN=OFF)
[ "${VULKAN:-0}" = "1" ] && { SUFFIX="-vk"; ARGS=(-DMC3_VULKAN=ON); }
TYPE=RelWithDebInfo
[ "${DEBUG:-0}" = "1" ] && { TYPE=Debug; SUFFIX="$SUFFIX-dbg"; }
ARGS+=(-DMC3_GEN_DEBUGINFO="$([ "${GEN_DEBUGINFO:-0}" = "1" ] && echo ON || echo OFF)")
BUILD="${BUILD_DIR:-$MC3_WORK/build-$MC3_HOST$SUFFIX}"

# Library paths CMake remembered (pkg-config) that are gone: Homebrew's
# upgrades delete the old version's folder (FFmpeg 8.1 -> 9.0, 2026-10-21),
# and the link then fails on files that no longer exist. Forget those
# entries so CMake looks them up again.
UNSET=()
if [ -f "$BUILD/CMakeCache.txt" ]; then
    while IFS= read -r path; do
        [ -e "$path" ] && continue
        echo "libraries moved since the last configure (e.g. a Homebrew upgrade): looking them up again"
        UNSET=(-U 'pkgcfg_lib_*' -U '__pkg_config_checked_*' -U '__pkg_config_arguments_*')
        break
    done < <(sed -n 's/^pkgcfg_lib_[^:]*:FILEPATH=//p' "$BUILD/CMakeCache.txt")
fi
cmake -S "$MC3_REPO" -B "$BUILD" -G Ninja ${UNSET[@]+"${UNSET[@]}"} -DCMAKE_BUILD_TYPE="$TYPE" \
    -DXBOXRECOMP_DIR="$XBOXRECOMP_DIR" -DMC3_GEN_DIR="$MC3_GEN_DIR" "${ARGS[@]}" >/dev/null
ninja -C "$BUILD" -j"${JOBS:-$(mc3_default_jobs)}"
echo "built $BUILD/$MC3_APP   (run: make run${GAME:+ GAME=$GAME})"
