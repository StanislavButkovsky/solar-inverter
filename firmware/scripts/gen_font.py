#!/usr/bin/env python3
# Шрифты дисплея: Noto Sans (OFL) → src/disp_font.h, сглаженные, 4 бита на
# точку. Запускается руками при смене размеров или набора букв, не при
# сборке: нужен Pillow, а у платформы его нет.
#
#   python3 -m venv /tmp/f && /tmp/f/bin/pip install pillow
#   /tmp/f/bin/python scripts/gen_font.py /usr/share/fonts/google-noto
import sys
from PIL import Image, ImageDraw, ImageFont

DIR = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/google-noto"
TEXT = ([chr(c) for c in range(32, 127)] + [chr(c) for c in range(0x410, 0x450)] +
        ["Ё", "ё", "°", "·", "−", "→", "…", "₽", "–", "—"])
BIG = list("0123456789 ,.%−-:")
FONTS = [  # имя, файл, размер, набор
    ("FONT_S", "NotoSans-Regular.ttf", 16, TEXT),
    ("FONT_M", "NotoSans-SemiBold.ttf", 22, TEXT),
    ("FONT_L", "NotoSans-Bold.ttf", 50, BIG),
]


def gen(name, file, size, chars):
    f = ImageFont.truetype(DIR + "/" + file, size)
    asc, desc = f.getmetrics()
    glyphs, bmp = [], bytearray()
    for ch in sorted(set(chars), key=ord):
        l, t, r, b = f.getbbox(ch, anchor="ls")
        w, h = max(0, r - l), max(0, b - t)
        adv = round(f.getlength(ch))
        off = len(bmp)
        if w and h:
            im = Image.new("L", (w, h), 0)
            ImageDraw.Draw(im).text((-l, -t), ch, font=f, fill=255, anchor="ls")
            px = list(im.tobytes())
            for y in range(h):
                row = [px[y * w + x] >> 4 for x in range(w)]
                if len(row) % 2:
                    row.append(0)
                for i in range(0, len(row), 2):
                    bmp.append(row[i] << 4 | row[i + 1])
        glyphs.append((ord(ch), w, h, l, t, adv, off))
    out = ["static const uint8_t %s_BMP[] = {" % name]
    for i in range(0, len(bmp), 24):
        out.append("    " + ",".join("0x%02x" % v for v in bmp[i:i + 24]) + ",")
    out.append("};")
    out.append("static const DGlyph %s_G[] = {" % name)
    for g in glyphs:
        out.append("    { 0x%04x, %d, %d, %d, %d, %d, %d }," % g)
    out.append("};")
    out.append("static const DFont %s = { %s_G, %d, %s_BMP, %d, %d };" %
               (name, name, len(glyphs), name, asc, asc + desc))
    return "\n".join(out), len(bmp)


parts, total = [], 0
for spec in FONTS:
    s, n = gen(*spec)
    parts.append(s)
    total += n
hdr = """// Сгенерировано scripts/gen_font.py из Noto Sans (SIL Open Font License) — не править.
// Глиф: код, ширина, высота, сдвиг x и y от точки на базовой линии, шаг,
// смещение в BMP (по 4 бита на точку, строки добиты до байта).
#pragma once
#include <stdint.h>

struct DGlyph { uint16_t cp; uint8_t w, h; int8_t x, y; uint8_t adv; uint32_t off; };
struct DFont { const DGlyph* g; uint16_t n; const uint8_t* bmp; uint8_t asc, lineH; };

"""
open("src/disp_font.h", "w").write(hdr + "\n\n".join(parts) + "\n")
print("disp_font.h: %d байт глифов" % total)
