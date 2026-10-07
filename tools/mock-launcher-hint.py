#!/usr/bin/env python3
"""launcher 首页「加双语提示」的 PC 模拟图（720x1280，与真机同尺寸）。

⚠ 关键：真机中文字库是 **12x12 点阵**（开机日志 cjk: 3773 glyphs, 12x12 cells），
   不是矢量字体。所以这里的中文也按"小字号渲染 → 最近邻放大"来画，
   如实体现真机上的点阵颗粒感 —— 否则 PC 稿会骗人。

坐标与真机一致（物理屏 720x1280；逻辑游戏区 720x480 在上方）。
"""
from PIL import Image, ImageDraw, ImageFont

W, H = 720, 1280
BG = (16, 18, 22)
FG = (235, 238, 242)
DIM = (128, 136, 148)
ACCENT = (232, 162, 44)
PANEL = (36, 41, 49)
LINE = (92, 101, 115)

CJK = "/System/Library/Fonts/Hiragino Sans GB.ttc"
MONO = "/System/Library/Fonts/Menlo.ttc"

# 真机点阵字是 12x12；放大 2x 显示 = 24px 视觉大小
CJK_CELL = 12
CJK_SCALE = 2


def font(path, size, index=0):
    try:
        return ImageFont.truetype(path, size, index=index)
    except Exception:
        return ImageFont.load_default()


def cjk_text(im, cx, y, s, color, scale=CJK_SCALE, bold=False):
    """按真机点阵字的方式绘制中文：12px 渲染 → 最近邻放大 scale 倍。"""
    f = font(CJK, CJK_CELL)
    w = int(sum(f.getlength(c) for c in s)) + 4
    h = CJK_CELL + 6
    layer = Image.new("L", (max(w, 1), h), 0)
    ImageDraw.Draw(layer).text((2, 1), s, font=f, fill=255)
    layer = layer.resize((w * scale, h * scale), Image.NEAREST)   # ← 点阵放大，不是平滑
    im.paste(Image.new("RGB", layer.size, color), (int(cx - w * scale / 2), y), layer)


def en_text(d, cx, y, s, color, size=24):
    f = font(MONO, size)
    d.text((cx - d.textlength(s, font=f) / 2, y), s, font=f, fill=color)


def rrect(d, box, r, fill=None, outline=None, width=2):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def draw_chrome(d, im):
    """复刻真机：机型列表项（贴顶）+ 触摸键位。
    ⚠ 真实坐标系：launcher 的 GUI 是**逻辑屏 720x480**，显示在物理屏的**上 480px**；
       触摸按键在物理屏下半（y=545 起）。标题贴顶 y=8，不是垂直居中。"""
    d.rectangle([36, 8, 66, 38], outline=FG, width=2)
    d.text((86, 10), "Nintendo Gameboy Advance", font=font(MONO, 26), fill=FG)

    def key(cx, cy, w, h, label, color=FG):
        rrect(d, [cx - w // 2, cy - h // 2, cx + w // 2, cy + h // 2], 12, outline=color, width=2)
        f = font(MONO, 24)
        d.text((cx - d.textlength(label, font=f) / 2, cy - 14), label, font=f, fill=color)

    key(140, 545, 180, 84, "L")
    key(360, 545, 180, 84, "X/Y", ACCENT)
    key(580, 545, 180, 84, "R")
    for lbl, cx, cy in [("UP", 170, 695), ("LEFT", 85, 780), ("RIGHT", 255, 780), ("DOWN", 170, 865)]:
        key(cx, cy, 84, 84, lbl, DIM)
    for lbl, cx, cy in [("X", 550, 695), ("Y", 465, 780), ("A", 635, 780), ("B", 550, 865)]:
        key(cx, cy, 84, 84, lbl)
    key(150, 1180, 150, 76, "SELECT", DIM)
    key(360, 1180, 150, 76, "START", DIM)
    key(570, 1180, 150, 76, "MENU", ACCENT)


def variant_a(im, d):
    """方案 A：纯文字，不画框。"""
    cjk_text(im, 360, 180, "按 A 键进入游戏列表", FG)
    en_text(d, 360, 226, "Press A to open the game list", DIM)
    cjk_text(im, 360, 320, "游戏 ROM 放在 /roms/gba/ 目录下", ACCENT)
    en_text(d, 360, 366, "Put ROM files in /roms/gba/", DIM)


def variant_b(im, d):
    """方案 B：提示卡（选中方案）。位置按真机最终布局：标题贴顶 8~58，
    卡片放在剩余区域正中 → y = 58 + (480-58-204)/2 = 167，高 204。"""
    rrect(d, [60, 167, 660, 371], 18, fill=PANEL, outline=LINE, width=2)
    cjk_text(im, 360, 195, "按 A 键进入游戏列表", FG)
    en_text(d, 360, 241, "Press A to open the game list", DIM)
    cjk_text(im, 360, 305, "游戏 ROM 放在 /roms/gba/ 目录下", ACCENT)
    en_text(d, 360, 351, "Put ROM files in /roms/gba/", DIM)


def main():
    for name, fn in [("A", variant_a), ("B", variant_b)]:
        im = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(im)
        draw_chrome(d, im)
        fn(im, d)
        d.line([0, 480, W, 480], fill=(46, 52, 60), width=1)   # 逻辑屏边界参考线
        im.save(f"docs/launcher-mock-{name}.png")
        print(f"wrote docs/launcher-mock-{name}.png")

    a = Image.open("docs/launcher-mock-A.png")
    b = Image.open("docs/launcher-mock-B.png")
    sheet = Image.new("RGB", (W * 2 + 24, H), (8, 8, 10))
    sheet.paste(a, (0, 0))
    sheet.paste(b, (W + 24, 0))
    sheet.save("docs/launcher-mock-compare.png")
    print("wrote docs/launcher-mock-compare.png")


if __name__ == "__main__":
    main()
