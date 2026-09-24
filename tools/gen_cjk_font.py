#!/usr/bin/env python3
"""从缝合像素字体（Fusion Pixel Font 12px）生成设备用的 CJK 点阵字库。

为什么用 BDF 而不是 TTF：BDF 是原生点阵，像素完美，不需要栅格化，也不会因
hinting/抗锯齿把 11x11 的笔画糊掉（12px 汉字容错只有 1 像素）。

字库格式（小端）：
    offset 0   char[4]  "RGF1"
    offset 4   uint16   cell_w      (12)
    offset 6   uint16   cell_h      (12)
    offset 8   uint32   count
    offset 12  uint32   index_off   (= 20)
    offset 16  uint32   glyph_off   (= 20 + 4*count)
    index:  count x uint32  码位（升序，便于二分查找）
    glyph:  count x 24B     12 行 x 每行 2 字节，最高位是行内最左像素

用法:
    python3 tools/gen_cjk_font.py <zh_hans.bdf> retro-go-p4/assets/cjk12.bin
"""
import re
import struct
import sys
from pathlib import Path

CELL_W, CELL_H = 12, 12
ROWS_PER_GLYPH = 2 * CELL_H          # 每行 2 字节（12 位有效）
BASE_ROW = 9                         # 基线所在行（0 起，从上往下）；CJK 11x11 字形占 0..10 行

# 菜单/UI 会用到、但可能不在 GB2312 一级字库里的字（安全网）
EXTRA_CHARS = (
    "虚拟按键透明度语言设置存档金手指重启退出保存载入重置音量亮度静音时钟电池"
    "网络关于帮助返回确定取消是否开关中英文快速读写信件手柄测试"
    "音量游戏机模拟器内存卡文件夹目录路径选择浏览"
    "，。！？：；、（）「」《》—…·　"
)


def parse_bdf(path):
    txt = Path(path).read_text(encoding="latin-1")
    glyphs = {}
    for enc, w, h, xo, yo, bits in re.findall(
        r"STARTCHAR .*?ENCODING (-?\d+).*?BBX (\d+) (\d+) (-?\d+) (-?\d+).*?BITMAP\n(.*?)ENDCHAR",
        txt, re.S,
    ):
        glyphs[int(enc)] = (int(w), int(h), int(xo), int(yo), [l.strip() for l in bits.strip().splitlines()])
    return glyphs


def gb2312_level1():
    out = []
    for hi in range(0xB0, 0xD8):        # 一级汉字区：0xB0A1..0xD7F9
        for lo in range(0xA1, 0xFF):
            try:
                out.append(bytes([hi, lo]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return out


def render_cell(g):
    """把 BDF 字形放进 12x12 单元格，返回 24 字节（每行 2 字节，MSB 在左）。"""
    w, h, xo, yo, rows = g
    cell = bytearray(ROWS_PER_GLYPH)
    for ry, hexline in enumerate(rows):
        # ⚠ BDF 的位图行是**左对齐**到整字节的：宽度 11 时有效位是 bit15..bit5。
        # 必须先右移到 w 位再取，否则整个字形会向左平移 (16-w) 像素（右半边丢失）。
        val = int(hexline, 16) >> (16 - w)
        y = yo + (h - 1 - ry)            # BDF 的 y 轴向上：位图第一行是字形最高行
        crow = BASE_ROW - y
        if crow < 0 or crow >= CELL_H:
            continue
        for rx in range(w):
            if (val >> (w - 1 - rx)) & 1:
                cx = xo + rx
                if 0 <= cx < CELL_W:
                    cell[crow * 2 + (cx >> 3)] |= 0x80 >> (cx & 7)
    return bytes(cell)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 1
    bdf_path, out_path = sys.argv[1], sys.argv[2]
    G = parse_bdf(bdf_path)
    print(f"BDF 字形数: {len(G)}")

    wanted = []
    seen = set()
    for ch in gb2312_level1() + list(EXTRA_CHARS):
        if ord(ch) not in seen:
            seen.add(ord(ch))
            wanted.append(ch)
    print(f"请求字符: {len(wanted)}（GB2312 一级 + 菜单安全网）")

    packed, missing = [], []
    for ch in sorted(wanted, key=ord):
        g = G.get(ord(ch))
        if g is None:
            missing.append(ch)
            continue
        packed.append((ord(ch), render_cell(g)))
    packed.sort(key=lambda t: t[0])

    n = len(packed)
    index_off = 20                      # 基础头 16B + glyph_off 字段 4B
    glyph_off = index_off + 4 * n
    blob = bytearray()
    blob += b"RGF1"
    blob += struct.pack("<HHII", CELL_W, CELL_H, n, index_off)
    blob += struct.pack("<I", glyph_off)
    for cp, _ in packed:
        blob += struct.pack("<I", cp)
    for _, data in packed:
        blob += data
    Path(out_path).write_bytes(blob)

    print(f"写入 {out_path}: {len(blob)} 字节 = {len(blob)/1024:.1f} KB，{n} 字形")
    if missing:
        print(f"字库缺失 {len(missing)} 字（将回退显示）: {''.join(missing[:40])}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
