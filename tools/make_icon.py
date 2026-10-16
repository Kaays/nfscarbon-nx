#!/usr/bin/env python3
"""Makes a personal Homebrew Menu icon from any picture of your choice.

    python3 tools/make_icon.py my_picture.png

Crops the picture to a centered square, scales it to 256x256 and saves it as
tools/switch/icon/nfscarbon_icon_local.jpg. The build uses that file instead of the repository's icon when it
exists. It is ignored by git, so a picture you are not allowed to share (for example, art from the game) stays on
your computer and never reaches GitHub. Rebuild the NRO afterwards (tools/build.sh).

Needs Pillow: pip install --break-system-packages pillow
"""
import os
import sys

from PIL import Image

if len(sys.argv) != 2:
    raise SystemExit(__doc__)
src = Image.open(sys.argv[1]).convert("RGB")
side = min(src.size)
left = (src.width - side) // 2
top = (src.height - side) // 2
icon = src.crop((left, top, left + side, top + side)).resize((256, 256), Image.LANCZOS)
dst = os.path.join(os.path.dirname(os.path.abspath(__file__)), "switch", "icon", "nfscarbon_icon_local.jpg")
icon.save(dst, quality=92)
print(f"wrote {dst} (256x256). Rebuild the NRO to use it.")
