#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""重新生成内置 CJK 点阵字库 cjk12.bin（RGF1 格式）。

为什么要它：assets/cjk12.bin 是**上游翻译表字符的子集**，本地新增的中文
（新手引导卡、launcher 新翻译）不在其中——缺字是**静默跳过**（画成空白），
屏幕上表现为"中文菜单缺一块/整行空"。加一条中文文案必须重跑本脚本。

格式（与 rg_cjk.c 的读取代码逐字对齐，2026-10-14 逆向核对）：
    header(24B): "RGF1" | u16 cell_w | u16 cell_h | u32 count | u32 index_off | u32 glyph_off
    index:      count × u32LE 码位，升序（二分查找）
    glyph:      count × (cell_h × row_bytes)，row_bytes=(w+7)//8，每行 **MSB 在左**

规则：
- **旧库里已有的字形逐字节保留**（观感已定版，不重渲）；只渲染新增字符。
- 新字渲染参数用"与旧字形像素差最小"自动校准（STHeiti Medium 12px，扫 y 偏移/字号）。
- 干跑默认：只报告覆盖与新增；--write 才落盘。

用法:
    python3 tools/gen_cjk_font_rgf1.py            # 干跑：报缺字与计划
    python3 tools/gen_cjk_font_rgf1.py --write    # 生成并覆盖 assets/cjk12.bin
"""
import re, struct, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN  = os.path.join(ROOT, "retro-go-p4/assets/cjk12.bin")
CELL_W = CELL_H = 12
ROW_BYTES = (CELL_W + 7) // 8

def scan_required():
    """收集所有会显示在屏幕上的 CJK 字符：翻译表 zh 列 + launcher/引导卡字面量。"""
    chars = set()
    def add_text(s):
        for ch in s:
            if ord(ch) >= 0x80 and not (0xFF61 <= ord(ch) <= 0xFF9E):
                chars.add(ch)
    th = open(os.path.join(ROOT, "retro-go-p4/components/retro-go/translations.h"), encoding="utf-8").read()
    for zh in re.findall(r'\[RG_LANG_ZH\]\s*=\s*"((?:[^"\\]|\\.)*)"', th):
        add_text(zh.replace("\\n", "\n").replace('\\"', '"'))
    for rel in ["retro-go-p4/launcher/main/gui.c", "retro-go-p4/launcher/main/applications.c",
                "retro-go-p4/launcher/main/main.c"]:
        src = open(os.path.join(ROOT, rel), encoding="utf-8").read()
        # 去注释再扫字符串字面量（注释里的中文不上屏，但扫上也无害——多几个字形而已）
        for m in re.findall(r'"((?:[^"\\\n]|\\.)*)"', src):
            add_text(m)
    return chars

def load_old():
    b = open(BIN, "rb").read()
    assert b[:4] == b"RGF1", "magic 不符"
    w, h = struct.unpack_from("<HH", b, 4)
    count, index_off, glyph_off = struct.unpack_from("<III", b, 8)
    assert (w, h) == (CELL_W, CELL_H), f"cell 尺寸变了 {(w,h)}"
    cps = struct.unpack_from("<%dI" % count, b, index_off)
    gsz = CELL_H * ROW_BYTES
    return {cp: b[glyph_off + i * gsz: glyph_off + (i + 1) * gsz] for i, cp in enumerate(cps)}

def glyph_to_bits(g):
    rows = []
    for y in range(CELL_H):
        v = 0
        for i in range(ROW_BYTES):
            v = (v << 8) | g[y * ROW_BYTES + i]
        rows.append([(v >> (ROW_BYTES * 8 - 1 - x)) & 1 for x in range(CELL_W)])
    return rows

def render_char(font, ch, dy):
    from PIL import Image, ImageDraw
    img = Image.new("L", (CELL_W + 8, CELL_H + 8), 0)
    d = ImageDraw.Draw(img)
    d.text((4, dy), ch, font=font, fill=255)
    crop = img.crop((4, 0, 4 + CELL_W, CELL_H))
    bits = [[1 if crop.getpixel((x, y)) > 96 else 0 for x in range(CELL_W)] for y in range(CELL_H)]
    return bits

def pack(bits):
    out = bytearray()
    for row in bits:
        v = 0
        for x, px in enumerate(row):
            if px:
                v |= 1 << (ROW_BYTES * 8 - 1 - x)
        out += v.to_bytes(ROW_BYTES, "big")
    return bytes(out)

def calibrate(old):
    """扫字号/偏移，使新渲染的字符与旧字形像素差最小（拿 6 个共有字校准）。"""
    from PIL import ImageFont
    font_path = "/System/Library/Fonts/STHeiti Medium.ttc"
    samples = [cp for cp in (0x4ece, 0x5220, 0x6e38, 0x6309, 0x4e2d, 0x5c4f) if cp in old]
    best = None
    for size in (11, 12):
        font = ImageFont.truetype(font_path, size)
        for dy in range(-3, 2):
            err = tot = 0
            for cp in samples:
                want = glyph_to_bits(old[cp])
                got = render_char(font, chr(cp), dy)
                err += sum(a != b for ra, rb in zip(want, got) for a, b in zip(ra, rb))
                tot += CELL_W * CELL_H
            rate = err / max(tot, 1)
            if best is None or rate < best[0]:
                best = (rate, size, dy)
    return best  # (diff率, 字号, y偏移)

def main():
    write = "--write" in sys.argv
    need = scan_required()
    old = load_old()
    new_chars = sorted(c for c in need if ord(c) not in old)
    print(f"需上屏字符: {len(need)} | 旧库字形: {len(old)} | 缺字(将新增渲染): {len(new_chars)}")
    print("缺字清单:", "".join(new_chars))
    rate, size, dy = calibrate(old)
    print(f"校准: 与旧字形平均差 {rate*100:.1f}% @ STHeiti Medium size={size} dy={dy}")
    all_cps = sorted(set(old) | {ord(c) for c in need})
    if not write:
        print("（干跑；--write 落盘）")
        return
    from PIL import ImageFont
    font = ImageFont.truetype("/System/Library/Fonts/STHeiti Medium.ttc", size)
    glyphs = []
    rendered = 0
    for cp in all_cps:
        if cp in old:
            glyphs.append(old[cp])
        else:
            glyphs.append(pack(render_char(font, chr(cp), dy)))
            rendered += 1
    count = len(all_cps)
    # header 实际 20B："RGF1"(4)+HHIII(16)。rg_cjk.c 从固定偏移读，index_off 必须=len(header)=20。
    index_off = 20
    glyph_off = index_off + count * 4
    out = bytearray()
    out += b"RGF1" + struct.pack("<HHIII", CELL_W, CELL_H, count, index_off, glyph_off)
    for cp in all_cps:
        out += struct.pack("<I", cp)
    for g in glyphs:
        assert len(g) == CELL_H * ROW_BYTES
        out += g
    tmp = BIN + ".new"
    open(tmp, "wb").write(bytes(out))
    os.replace(tmp, BIN)
    print(f"写入 {BIN}: {count} 字形（新渲染 {rendered}），{len(out)} 字节")

if __name__ == "__main__":
    main()
