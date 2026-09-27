"""
Generate the launcher icons (Clock / Music / AI Chat / Settings) for the Brookesia launcher.

Style: iOS-like rounded square, diagonal gradient background, simple white glyph.
Each icon is drawn at 4x resolution and downsampled for anti-aliasing, then written as:
  - <component>/assets/<name>.png   (preview, easy to open and check)
  - <component>/assets/<name>.c     (LVGL v9 lv_image_dsc_t, LV_COLOR_FORMAT_ARGB8888)

Usage (system Python with Pillow):
    python tools/make_icons.py

To replace an icon with your own artwork: put a square PNG with transparency somewhere and run
    python tools/make_icons.py --png my_icon.png --name esp_brookesia_app_icon_clock --out components/brookesia_app_clock/assets
"""
import argparse
import math
import os

from PIL import Image, ImageDraw

SIZE = 112          # final icon size in pixels (launcher scales it to the stylesheet size)
SS = 4              # supersampling factor
S = SIZE * SS
RADIUS = int(S * 0.225)   # corner radius, close to the iOS look
MARGIN = 2 * SS           # small transparent border so the scaled edge stays smooth

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def gradient_square(top_left, bottom_right):
    """Rounded square filled with a diagonal gradient."""
    grad = Image.new("RGBA", (S, S))
    px = grad.load()
    for y in range(S):
        for x in range(S):
            t = (x + y) / (2 * (S - 1))
            px[x, y] = tuple(int(top_left[i] + (bottom_right[i] - top_left[i]) * t) for i in range(3)) + (255,)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (MARGIN, MARGIN, S - 1 - MARGIN, S - 1 - MARGIN), radius=RADIUS, fill=255
    )
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    img.paste(grad, (0, 0), mask)
    # Soft top highlight, like the glossy official icons
    hl = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    hd = ImageDraw.Draw(hl)
    for i in range(S // 2):
        a = int(40 * (1 - i / (S / 2)))
        hd.line((0, MARGIN + i, S, MARGIN + i), fill=(255, 255, 255, a))
    hl_mask = Image.new("L", (S, S), 0)
    hl_mask.paste(mask, (0, 0))
    img = Image.alpha_composite(img, Image.composite(hl, Image.new("RGBA", (S, S), (0, 0, 0, 0)), hl_mask))
    return img


def line(d, p0, p1, width, fill):
    d.line((p0, p1), fill=fill, width=width)
    r = width / 2
    for (x, y) in (p0, p1):
        d.ellipse((x - r, y - r, x + r, y + r), fill=fill)


def draw_clock():
    img = gradient_square((58, 123, 213), (27, 38, 107))
    d = ImageDraw.Draw(img)
    c = S / 2
    r = S * 0.34
    d.ellipse((c - r, c - r, c + r, c + r), fill=(255, 255, 255, 255))
    # Hour ticks
    for i in range(12):
        a = math.radians(i * 30)
        r0 = r * (0.78 if i % 3 == 0 else 0.84)
        w = int(S * (0.022 if i % 3 == 0 else 0.012))
        line(d, (c + r0 * math.sin(a), c - r0 * math.cos(a)),
             (c + r * 0.92 * math.sin(a), c - r * 0.92 * math.cos(a)), w, (40, 48, 72, 255))
    # 10:10 hands
    def hand(deg, length, width, color):
        a = math.radians(deg)
        line(d, (c, c), (c + length * math.sin(a), c - length * math.cos(a)), width, color)
    hand(-60, r * 0.48, int(S * 0.04), (30, 36, 56, 255))
    hand(60, r * 0.68, int(S * 0.03), (30, 36, 56, 255))
    hand(150, r * 0.74, int(S * 0.012), (255, 120, 40, 255))
    k = S * 0.03
    d.ellipse((c - k, c - k, c + k, c + k), fill=(255, 120, 40, 255))
    return img


def draw_music():
    img = gradient_square((255, 110, 120), (230, 30, 80))
    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    # Two note heads (tilted ellipses approximated by drawing rotated layers)
    def note_head(cx, cy):
        head = Image.new("RGBA", (S, S), (0, 0, 0, 0))
        hd = ImageDraw.Draw(head)
        rx, ry = S * 0.085, S * 0.065
        hd.ellipse((cx - rx, cy - ry, cx + rx, cy + ry), fill=white)
        return head.rotate(20, center=(cx, cy), resample=Image.BICUBIC)
    x1, y1 = S * 0.36, S * 0.70
    x2, y2 = S * 0.66, S * 0.63
    img.alpha_composite(note_head(x1, y1))
    img.alpha_composite(note_head(x2, y2))
    d = ImageDraw.Draw(img)
    w = int(S * 0.04)
    top1, top2 = S * 0.27, S * 0.20
    sx1, sx2 = x1 + S * 0.065, x2 + S * 0.065
    d.rectangle((sx1 - w / 2, top1, sx1 + w / 2, y1 - S * 0.02), fill=white)
    d.rectangle((sx2 - w / 2, top2, sx2 + w / 2, y2 - S * 0.02), fill=white)
    # Beam
    bh = S * 0.075
    d.polygon([(sx1 - w / 2, top1), (sx2 + w / 2, top2), (sx2 + w / 2, top2 + bh), (sx1 - w / 2, top1 + bh)], fill=white)
    return img


def draw_ai_chat():
    img = gradient_square((79, 195, 247), (21, 101, 192))
    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    c = S / 2
    # Antenna
    line(d, (c, S * 0.22), (c, S * 0.32), int(S * 0.025), white)
    k = S * 0.04
    d.ellipse((c - k, S * 0.19 - k, c + k, S * 0.19 + k), fill=(255, 214, 90, 255))
    # Ears
    ew, eh = S * 0.05, S * 0.14
    d.rounded_rectangle((S * 0.17, c - eh / 2 + S * 0.05, S * 0.17 + ew, c + eh / 2 + S * 0.05), radius=ew / 2, fill=white)
    d.rounded_rectangle((S * 0.83 - ew, c - eh / 2 + S * 0.05, S * 0.83, c + eh / 2 + S * 0.05), radius=ew / 2, fill=white)
    # Head
    d.rounded_rectangle((S * 0.22, S * 0.31, S * 0.78, S * 0.79), radius=S * 0.13, fill=white)
    # Visor
    d.rounded_rectangle((S * 0.29, S * 0.41, S * 0.71, S * 0.62), radius=S * 0.1, fill=(22, 40, 90, 255))
    # Eyes
    er = S * 0.045
    for ex in (S * 0.405, S * 0.595):
        d.ellipse((ex - er, S * 0.515 - er, ex + er, S * 0.515 + er), fill=(90, 230, 255, 255))
    # Smile
    d.arc((S * 0.42, S * 0.62, S * 0.58, S * 0.74), start=20, end=160, fill=(22, 40, 90, 255), width=int(S * 0.02))
    return img


def draw_settings():
    img = gradient_square((144, 150, 164), (72, 78, 92))
    d = ImageDraw.Draw(img)
    white = (255, 255, 255, 255)
    c = S / 2
    # Gear: 8 rounded teeth around a ring, like the iOS Settings glyph
    teeth, r_out, r_body = 8, S * 0.33, S * 0.25
    tw = S * 0.11
    for i in range(teeth):
        a = 2 * math.pi * i / teeth
        p0 = (c + math.cos(a) * r_body * 0.8, c + math.sin(a) * r_body * 0.8)
        p1 = (c + math.cos(a) * (r_out - tw / 2), c + math.sin(a) * (r_out - tw / 2))
        line(d, p0, p1, int(tw), white)
    d.ellipse((c - r_body, c - r_body, c + r_body, c + r_body), fill=white)
    hole = S * 0.105
    d.ellipse((c - hole, c - hole, c + hole, c + hole), fill=(100, 106, 120, 255))
    return img


C_TEMPLATE = """/*
 * Generated by tools/make_icons.py - do not edit by hand.
 * {w}x{h} LV_COLOR_FORMAT_ARGB8888
 */
#include "lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

static const LV_ATTRIBUTE_MEM_ALIGN uint8_t {name}_map[] = {{
{data}
}};

const lv_image_dsc_t {name} = {{
    .header.cf = LV_COLOR_FORMAT_ARGB8888,
    .header.magic = LV_IMAGE_HEADER_MAGIC,
    .header.w = {w},
    .header.h = {h},
    .header.stride = {w} * 4,
    .data_size = {w} * {h} * 4,
    .data = {name}_map,
}};
"""


def write_icon(img, name, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    if img.size != (SIZE, SIZE):
        img = img.resize((SIZE, SIZE), Image.LANCZOS)
    img.save(os.path.join(out_dir, name + ".png"))
    rows = []
    px = img.load()
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            r, g, b, a = px[x, y]
            row.append("0x%02x,0x%02x,0x%02x,0x%02x" % (b, g, r, a))   # ARGB8888 is B,G,R,A in memory
        rows.append("    " + ",".join(row) + ",")
    with open(os.path.join(out_dir, name + ".c"), "w", newline="\n") as f:
        f.write(C_TEMPLATE.format(name=name, w=SIZE, h=SIZE, data="\n".join(rows)))
    print("wrote", os.path.join(out_dir, name + ".c"))


ICONS = [
    (draw_clock, "esp_brookesia_app_icon_clock", "components/brookesia_app_clock/assets"),
    (draw_music, "esp_brookesia_app_icon_music", "components/brookesia_app_music/assets"),
    (draw_ai_chat, "esp_brookesia_app_icon_ai_chat", "components/brookesia_app_xiaozhi/assets"),
    (draw_settings, "esp_brookesia_app_icon_settings", "components/brookesia_app_settings/assets"),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png", help="convert this PNG instead of drawing the built-in icons")
    ap.add_argument("--name", help="C symbol name (with --png)")
    ap.add_argument("--out", help="output directory (with --png)")
    args = ap.parse_args()

    if args.png:
        write_icon(Image.open(args.png).convert("RGBA"), args.name, os.path.join(ROOT, args.out))
        return

    previews = []
    for draw, name, out in ICONS:
        img = draw().resize((SIZE, SIZE), Image.LANCZOS)
        write_icon(img, name, os.path.join(ROOT, out))
        previews.append(img)
    sheet = Image.new("RGBA", (len(previews) * (SIZE + 10) + 10, SIZE + 20), (20, 20, 28, 255))
    for i, p in enumerate(previews):
        sheet.alpha_composite(p, (10 + i * (SIZE + 10), 10))
    sheet.save(os.path.join(ROOT, "tools", "icons_preview.png"))


if __name__ == "__main__":
    main()
