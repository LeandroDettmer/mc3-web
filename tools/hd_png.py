#!/usr/bin/env python3
"""PNG helpers for tools/hd_upscale.sh (needs Pillow).

  hd_png.py split  <in.png> <rgb.png>            colour only, fully opaque
  hd_png.py merge  <rgb.png> <orig.png> <out.png> <w> <h>
                   colour scaled to w x h (Lanczos), the original's alpha
                   scaled to w x h (Lanczos), joined
  hd_png.py opaque <in.png>                      exit 0 if alpha is all 255

Why: MC3 keeps data in many textures' alpha (gloss, the night window glow:
values like 6 or 60 everywhere), not transparency. Real-ESRGAN treats
alpha as transparency and darkened the colour where it was low -- grass,
roads and facades came out black (2026-10-21). So the colour is upscaled
on its own, opaque, and the alpha is only resized: no detail is invented
in a channel the game reads as numbers.
"""
import sys
from PIL import Image


def main(argv):
    cmd = argv[1]
    if cmd == "split":
        Image.open(argv[2]).convert("RGB").save(argv[3])
    elif cmd == "merge":
        w, h = int(argv[5]), int(argv[6])
        rgb = Image.open(argv[2]).convert("RGB")
        if rgb.size != (w, h):
            rgb = rgb.resize((w, h), Image.LANCZOS)
        orig = Image.open(argv[3]).convert("RGBA")
        alpha = orig.getchannel("A").resize((w, h), Image.LANCZOS)
        out = rgb.copy()
        out.putalpha(alpha)
        out.save(argv[4])
    elif cmd == "opaque":
        im = Image.open(argv[2])
        if im.mode != "RGBA":
            return 0
        lo, _ = im.getchannel("A").getextrema()
        return 0 if lo == 255 else 1
    else:
        print(__doc__, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
