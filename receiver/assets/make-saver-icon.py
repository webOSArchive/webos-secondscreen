#!/usr/bin/env python3
"""Generate saver-icon.jpg (the bouncing screensaver sprite) from the
app icon: 128x128, composited on black so it blits opaquely onto the
black screensaver background (the GL path has no alpha blending).

The sprite size MUST match the SAVER_ICON_W/H defines in src/main.c.
"""
from PIL import Image

SIZE = 128

src = Image.open("../icon-256.png").convert("RGBA")
src = src.resize((SIZE, SIZE), Image.LANCZOS)
out = Image.new("RGB", (SIZE, SIZE), (0, 0, 0))
out.paste(src, (0, 0), src)
out.save("saver-icon.jpg", quality=92)  # baseline: decoder is libjpeg 6.2-era
print("wrote saver-icon.jpg")
