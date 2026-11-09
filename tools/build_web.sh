#!/usr/bin/env bash
# Build for the browser: WebAssembly + WebGL 2 with Emscripten, into
# work-<game>/build-web (<app>.js, <app>.wasm, index.html). Serve it with
# tools/web_serve.py (make web-serve): threads need COOP/COEP headers, and
# the disc is read from that server, never copied into the build.
#
#   tools/build_web.sh            the browser build
#   NODE=1 tools/build_web.sh     Node.js instead (work-<game>/build-web-node):
#                                 the host's files (NODERAWFS), for a headless
#                                 boot test without a browser
#
# Needs Emscripten on PATH (emcmake, emcc): source emsdk_env.sh first, or set
# EMSDK to the emsdk folder. No FFmpeg (the lifted Bink decoder plays the
# movies) and no Vulkan.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
mc3_check_lift_fresh

if ! command -v emcmake >/dev/null && [ -f "${EMSDK:-/opt/emsdk}/emsdk_env.sh" ]; then
    # shellcheck source=/dev/null
    . "${EMSDK:-/opt/emsdk}/emsdk_env.sh" >/dev/null 2>&1
fi
command -v emcmake >/dev/null || mc3_fail "no Emscripten: install emsdk and source emsdk_env.sh (or set EMSDK)"

# ccache when installed (CCACHE=0 off): emcc runs clang through it, so an
# unchanged lifted chunk is not recompiled -- also across build folders
# (CCACHE_BASEDIR makes the repo's paths relative) and after a relift that
# left a chunk's text the same. The cache lives in the Linux home: fast.
if [ "${CCACHE:-1}" != 0 ] && command -v ccache >/dev/null; then
    export EM_COMPILER_WRAPPER=ccache
    export CCACHE_DIR="${CCACHE_DIR:-$HOME/.cache/ccache-mc3}" CCACHE_BASEDIR="$MC3_REPO"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-20G}" CCACHE_NOHASHDIR=1
    export CCACHE_SLOPPINESS="${CCACHE_SLOPPINESS:-time_macros,include_file_mtime,include_file_ctime}"
fi

SUFFIX=""; ARGS=()
[ "${NODE:-0}" = "1" ] && { SUFFIX="-node"; ARGS=(-DMC3_WEB_NODE=ON); }
BUILD="${BUILD_DIR:-$MC3_WORK/build-web$SUFFIX}"

# The build stamp in the boot banner (src/main.c): commit (+dirty: uncommitted
# changes went in) and UTC build time, so a log names the module that ran.
# MC3_BUILD_ID set by the caller (CI) wins. main.c recompiles each build.
if [ -z "${MC3_BUILD_ID:-}" ]; then
    _git=(git -C "$MC3_REPO" -c safe.directory='*')
    MC3_BUILD_ID="$("${_git[@]}" rev-parse --short HEAD 2>/dev/null || echo nogit)"
    "${_git[@]}" diff --quiet HEAD 2>/dev/null || MC3_BUILD_ID+="+dirty"
    MC3_BUILD_ID+=" built $(date -u +%Y-%m-%dT%H:%MZ)"
fi

# -O3 + wasm SIMD, no debug info: the lifted C is ~3 M lines. MC3_GEN_OPT
# is passed too, so an older build folder's cached -O2 is replaced. No
# -ffast-math: lifted float code must stay exact.
emcmake cmake -S "$MC3_REPO" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="-O3 -msimd128 -DNDEBUG" -DMC3_GEN_OPT="-O3;-msimd128" \
    -DXBOXRECOMP_DIR="$XBOXRECOMP_DIR" -DMC3_GEN_DIR="$MC3_GEN_DIR" \
    -DMC3_NATIVE_BINK=OFF -DMC3_VULKAN=OFF "-DMC3_BUILD_ID=$MC3_BUILD_ID" ${ARGS[@]+"${ARGS[@]}"} >/dev/null
ninja -C "$BUILD" -j"${JOBS:-$(mc3_default_jobs)}"

if [ "${NODE:-0}" = "1" ]; then
    echo "built $BUILD/$MC3_APP.js   (run: MC3_GAME_DIR=$MC3_GAME_DIR MC3_GPU=0 node $BUILD/$MC3_APP.js)"
else
    echo "built $BUILD/$MC3_APP.js   (serve: make web-serve${GAME:+ GAME=$GAME}, then open http://localhost:3000/)"
fi
