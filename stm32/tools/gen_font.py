#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 8x16 ASCII 字模表（供 ILI9341 界面用）。

为什么用脚本生成而不是手抄点阵
    手抄的点阵没人能复核，改一个字号就得重抄。这里用 Pillow 从系统等宽字体
    渲染 -> 二值化 -> 打包成 C 表，并且**同时输出一张预览图**，人眼一看就知道
    字模对不对（见 --preview）。

用法
    python tools/gen_font.py                      # 生成 port/lcd_font_8x16.c/.h
    python tools/gen_font.py --preview out.png    # 同时导出全部字符的预览图

字形约定（渲染代码必须一致）
    每个字符 8 列 x 16 行 = 16 字节；每字节是**一行**，MSB 在左（bit7 = 最左列）。
    字符集 = ASCII 0x20..0x7E（95 个），表项 i 对应字符 chr(0x20+i)。
"""
import argparse
import datetime
import io
import os
import sys

try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

from PIL import Image, ImageDraw, ImageFont      # noqa: E402

W, H = 8, 16
FIRST, LAST = 0x20, 0x7E
FONT_PATH = r"C:\Windows\Fonts\lucon.ttf"        # Lucida Console：小字号可读性最好
FONT_SIZE = 14                                   # 实测 14 号正好落在 8x16 格内


def render_glyph(ch, font):
    """渲染一个字符为 8x16 的 0/1 网格（1 = 点亮）"""
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    # 用 bbox 做居中：不同字形的墨迹范围不同，不居中的话基线会跳
    bbox = d.textbbox((0, 0), ch, font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (W - tw) // 2 - bbox[0]
    y = (H - th) // 2 - bbox[1]
    d.text((x, y), ch, fill=255, font=font)
    return img


def pack(img):
    """8x16 图像 -> 16 字节（每字节一行，MSB 在左）"""
    px = img.load()
    out = []
    for row in range(H):
        b = 0
        for col in range(W):
            if px[col, row] > 127:
                b |= (0x80 >> col)
        out.append(b)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-c", default=r"port\lcd_font_8x16.c")
    ap.add_argument("--out-h", default=r"port\lcd_font_8x16.h")
    ap.add_argument("--preview", default=None, help="导出全部字符的预览 PNG")
    ap.add_argument("--font", default=FONT_PATH)
    ap.add_argument("--size", type=int, default=FONT_SIZE)
    a = ap.parse_args()

    if not os.path.exists(a.font):
        print("  [FAIL] 找不到字体：%s" % a.font)
        return 2
    font = ImageFont.truetype(a.font, a.size)

    glyphs = []
    for code in range(FIRST, LAST + 1):
        glyphs.append(pack(render_glyph(chr(code), font)))

    stamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")
    src = io.StringIO()
    src.write("/* 由 tools/gen_font.py 生成 —— 不要手改。\n"
              " * 字体：%s  %d px；字符集：ASCII 0x%02X..0x%02X（%d 个）\n"
              " * 布局：每字符 16 字节，每字节一行，MSB 在左\n"
              " * 生成时间：%s\n */\n"
              % (os.path.basename(a.font), a.size, FIRST, LAST,
                 LAST - FIRST + 1, stamp))
    src.write('#include "lcd_font_8x16.h"\n\n')
    src.write("const uint8_t g_lcd_font8x16[LCD_FONT_CHARS][LCD_FONT_BYTES] = {\n")
    for i, g in enumerate(glyphs):
        ch = FIRST + i
        name = chr(ch) if 0x21 <= ch <= 0x7E else "sp"
        if name in ("'", "\\"):
            name = "0x%02X" % ch
        src.write("    { %s },  /* 0x%02X '%s' */\n"
                  % (", ".join("0x%02X" % b for b in g), ch, name))
    src.write("};\n")
    with open(a.out_c, "w", encoding="utf-8", newline="\n") as f:
        f.write(src.getvalue())

    hdr = io.StringIO()
    hdr.write("/* 由 tools/gen_font.py 生成 —— 不要手改。 */\n"
              "#ifndef LCD_FONT_8X16_H\n#define LCD_FONT_8X16_H\n\n"
              "#include <stdint.h>\n\n"
              "#define LCD_FONT_W        8u\n"
              "#define LCD_FONT_H        16u\n"
              "#define LCD_FONT_BYTES    16u\n"
              "#define LCD_FONT_FIRST    0x%02Xu\n"
              "#define LCD_FONT_CHARS    %du\n\n"
              "/* [字符序号][行]，字符序号 = ch - LCD_FONT_FIRST */\n"
              "extern const uint8_t g_lcd_font8x16[LCD_FONT_CHARS][LCD_FONT_BYTES];\n\n"
              "#endif /* LCD_FONT_8X16_H */\n"
              % (FIRST, LAST - FIRST + 1))
    with open(a.out_h, "w", encoding="utf-8", newline="\n") as f:
        f.write(hdr.getvalue())

    print("  [ OK ] %s（%d 字符 x %d 字节 = %d B）"
          % (a.out_c, len(glyphs), 16, len(glyphs) * 16))
    print("  [ OK ] %s" % a.out_h)

    if a.preview:
        cols, rows = 16, (len(glyphs) + 15) // 16
        pad = 2
        img = Image.new("RGB", (cols * (W + pad) + pad, rows * (H + pad) + pad),
                        (0, 0, 0))
        d = ImageDraw.Draw(img)
        for i, g in enumerate(glyphs):
            cx = pad + (i % cols) * (W + pad)
            cy = pad + (i // cols) * (H + pad)
            for row in range(H):
                for col in range(W):
                    if g[row] & (0x80 >> col):
                        d.point((cx + col, cy + row), fill=(0, 255, 0))
        img = img.resize((img.width * 3, img.height * 3), Image.NEAREST)
        img.save(a.preview)
        print("  [ OK ] 预览图 %s" % a.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
