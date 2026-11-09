#!/usr/bin/env bash
# Fetch the texture upscaler (Real-ESRGAN ncnn-vulkan, BSD-3) into
# work/tools/realesrgan/ for this OS (make hd-tools). Used by
# tools/hd_upscale.sh. Releases exist for macOS, Windows and x86-64 Linux;
# not for ARM64 Linux (the VM): there, upscale on the Mac -- the repo folder
# is shared, so hd/dump and hd/textures are the same on both.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"
REL="https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0"
case "$(uname -s)-$(uname -m)" in
    Darwin-*)       ZIP=realesrgan-ncnn-vulkan-20220424-macos.zip ;;
    Linux-x86_64)   ZIP=realesrgan-ncnn-vulkan-20220424-ubuntu.zip ;;
    *) mc3_fail "no Real-ESRGAN build for $(uname -s) $(uname -m): run make hd-upscale on the Mac" ;;
esac
DEST="$MC3_WORK/tools/realesrgan"
if [ -x "$DEST/realesrgan-ncnn-vulkan" ]; then
    echo "already in $DEST"; exit 0
fi
mkdir -p "$DEST"
curl -fL --progress-bar -o "$DEST.zip" "$REL/$ZIP"
unzip -q -o "$DEST.zip" -d "$DEST"
rm -f "$DEST.zip"
chmod +x "$DEST/realesrgan-ncnn-vulkan"
xattr -d com.apple.quarantine "$DEST/realesrgan-ncnn-vulkan" 2>/dev/null || true
echo "upscaler in $DEST"
