#!/usr/bin/env python3
"""Generate update.jpg (the "Update Available" prompt screen) from
waiting.jpg, keeping the same background and icon.

The button rectangles drawn here MUST match the UPD_BTN_*/LATER_BTN_*
hit-test defines in src/main.c.
"""
from PIL import Image, ImageDraw, ImageFont

FONT_DIR = "/usr/share/fonts/truetype/dejavu/"

# Button geometry — mirrored in src/main.c
BTN_Y0, BTN_Y1 = 596, 660
UPD_X0, UPD_X1 = 222, 482
LATER_X0, LATER_X1 = 542, 802

img = Image.open("waiting.jpg").convert("RGB")
bg = img.getpixel((20, 700))
d = ImageDraw.Draw(img)

# erase the "Waiting for Connection..." caption
d.rectangle((0, 430, 1024, 520), fill=bg)

title_f = ImageFont.truetype(FONT_DIR + "DejaVuSans.ttf", 40)
sub_f = ImageFont.truetype(FONT_DIR + "DejaVuSans.ttf", 24)
btn_f = ImageFont.truetype(FONT_DIR + "DejaVuSans-Bold.ttf", 26)

def centered(text, y, font, fill, cx=512):
    w = d.textlength(text, font=font)
    d.text((cx - w / 2, y), text, font=font, fill=fill)

centered("Update Available", 445, title_f, (255, 255, 255))
centered("A new version of Second Screen is in the App Catalog.", 510, sub_f,
         (204, 204, 204))

d.rounded_rectangle((UPD_X0, BTN_Y0, UPD_X1, BTN_Y1), radius=12,
                    fill=(43, 143, 196))
d.rounded_rectangle((LATER_X0, BTN_Y0, LATER_X1, BTN_Y1), radius=12,
                    fill=(85, 85, 85), outline=(136, 136, 136), width=2)

by = BTN_Y0 + (BTN_Y1 - BTN_Y0 - 26) / 2 - 4
centered("Update Now", by, btn_f, (255, 255, 255), cx=(UPD_X0 + UPD_X1) / 2)
centered("Later", by, btn_f, (255, 255, 255), cx=(LATER_X0 + LATER_X1) / 2)

img.save("update.jpg", quality=90)  # baseline (not progressive): decoder is libjpeg 6.2-era
print("wrote update.jpg")
