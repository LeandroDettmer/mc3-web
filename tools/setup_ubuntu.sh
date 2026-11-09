#!/usr/bin/env bash
# One-time setup of Ubuntu (or Ubuntu in WSL 2): what the lift needs (Python
# venv with capstone), and what the optional native Linux debug build needs.
# Emscripten (browser build) and Rust (server) are installed apart:
# CONTRIBUTING.md.
#
#   bash tools/setup_ubuntu.sh        (or: make setup)
#
# Safe to re-run. Needs sudo.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

echo "== 1/3 runtime build deps (the toolkit's own list: SDL2, GL/epoxy, OpenSSL, ...)"
bash "$XBOXRECOMP_DIR/tools/linux/install_deps.sh"

echo "== 2/3 tools: ninja, gdb, Vulkan (lavapipe for headless runs), rsync, python venv"
sudo apt-get install -y ninja-build gdb rsync python3-venv python3-pip \
    libvulkan-dev vulkan-tools mesa-vulkan-drivers glslang-dev glslang-tools \
    spirv-tools xvfb mesa-utils ffmpeg

echo "   perf for profiling (make profile), and permission to profile own processes"
sudo apt-get install -y "linux-tools-$(uname -r)" linux-tools-generic || \
    echo "   (no linux-tools for kernel $(uname -r); make profile will say so)"
echo 'kernel.perf_event_paranoid=1' | sudo tee /etc/sysctl.d/60-mc3-perf.conf >/dev/null
sudo sysctl -q -p /etc/sysctl.d/60-mc3-perf.conf || true

echo "== 3/3 Python venv with capstone for the lift pipeline ($MC3_WORK/venv)"
mkdir -p "$MC3_WORK"
# --copies: the work folder may sit on a share that takes no symlinks.
[ -x "$MC3_WORK/venv/bin/python3" ] || python3 -m venv --copies "$MC3_WORK/venv"
# Through the venv's python, not bin/pip: pip's script carries the venv's
# absolute path in its first line and breaks when the repo folder is renamed.
"$MC3_WORK/venv/bin/python3" -m pip install -q --upgrade pip capstone pytest numpy unicorn
# No shell profile changes: tools/env.sh puts work/venv on PATH for the scripts.

echo
echo "done. Then: make extract ISO=<iso> -> make lift -> make web (browser) or make build (native Linux)"
