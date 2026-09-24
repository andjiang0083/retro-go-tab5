#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
虚拟按键视觉预览（PC 端重渲染）—— 刷机前先看效果，替代"刷完才知道好不好看"。

为什么要有这个工具：
  叠加层是在显示驱动里"推给面板前"合成的（见 mipi_dsi_tab5.h），改一次要刷机才知道
  好不好看。本工具把同一套渲染规则搬到 PC 上：
    - 键位表直接从 targets/tab5/touch_layout.h 解析（不复制一份，避免"画的和点的不一致"）
      P2.5 起键位表已从 config.h 抽成单一数据源；这里跟着改，别再去 config.h 找
    - 字形直接从 fonts/basic8x8.c 解析（与固件同一个 8x8 点阵，所见即所得）
    - 边框/填充/文字的分层不透明度与固件一致（填充 α*0.50 / 边框 α*0.90 / 文字 α*1.00）
    - 形状用 4x 超采样求覆盖率 => 固件预渲染时同样能白拿抗锯齿（一次性成本，每帧零开销）

用法：
  python3 tools/preview-touch-overlay.py                # 出全套（hero / 调色板 / 按下 / 透明度）
  python3 tools/preview-touch-overlay.py --alpha 40     # 只出某档透明度的一张
输出：docs/touch-overlay-*.png
"""

import argparse
import re
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
LAYOUT_H = ROOT / "retro-go-p4/components/retro-go/targets/tab5/touch_layout.h"
FONT_C = ROOT / "retro-go-p4/components/retro-go/fonts/basic8x8.c"
OUT_DIR = ROOT / "docs"

SCR_W, SCR_H = 1280, 720
GAME_W, GAME_H = 720, 480          # 240x160 @ 3x 整数缩放
GAME_X, GAME_Y = (SCR_W - GAME_W) // 2, (SCR_H - GAME_H) // 2   # (280, 120)
SS = 4                              # 超采样倍数（固件预渲染用同一倍数）

ALPHA_FILL, ALPHA_BORDER, ALPHA_LABEL = 0.50, 0.90, 1.00

# ---------------------------------------------------------------- 键位表解析
def parse_keymap(path):
    text = path.read_text(encoding="utf-8")
    # P2.5 起键位表在 touch_layout.h（单一数据源）；config.h 只是 include 它
    block = re.search(r"#define RG_TAB5_TOUCH_MAP\s*\{(.*?)\n\}", text, re.S)
    if not block:
        raise SystemExit("找不到 RG_TAB5_TOUCH_MAP（键位表已移到 targets/tab5/touch_layout.h）")
    pat = re.compile(r"\{\s*(RG_KEY_\w+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\}")
    return [
        dict(key=m.group(1), x=int(m.group(2)), y=int(m.group(3)),
             w=int(m.group(4)), h=int(m.group(5)))
        for m in pat.finditer(block.group(1))
    ]


# ---------------------------------------------------------------- 字形解析
def parse_font(path):
    text = path.read_text(encoding="utf-8")
    body = text.split(".data = {", 1)[1].rsplit("}", 1)[0]
    body = re.sub(r"//.*", "", body)
    data = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
    glyphs, i = {}, 0
    while i + 7 <= len(data):
        code = data[i] | (data[i + 1] << 8)
        if code == 0:
            break
        y_off, w, h, x_off, x_delta = data[i + 2], data[i + 3], data[i + 4], data[i + 5], data[i + 6]
        i += 7
        nbytes = ((w * h - 1) // 8) + 1 if w else 0
        raw = data[i:i + nbytes]
        i += nbytes
        rows = []
        for y in range(h):
            row = 0
            for x in range(w):
                idx = y * w + x
                if idx % 8 == 0:
                    cur = raw[idx // 8]
                if cur & (0x80 >> (idx % 8)):
                    row |= 1 << (x_off + x)
            rows.append(row)
        glyphs[code] = dict(w=w, h=h, rows=rows, y_off=y_off, x_delta=x_delta)
    return glyphs


KEYMAP = parse_keymap(LAYOUT_H)
GLYPHS = parse_font(FONT_C)


# ---------------------------------------------------------------- 调色板
def hx(s):
    return (int(s[1:3], 16), int(s[3:5], 16), int(s[5:7], 16))


def v565(v):
    return (((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63, (v & 0x1F) * 255 // 31)


# A：重新调和的配色（D-pad 归一为一个十字单元；ABXY 用主机惯例四色）
PALETTE_A = {
    "RG_KEY_UP": hx("#7C8CA6"), "RG_KEY_DOWN": hx("#7C8CA6"),
    "RG_KEY_LEFT": hx("#7C8CA6"), "RG_KEY_RIGHT": hx("#7C8CA6"),
    "RG_KEY_A": hx("#E24B3F"), "RG_KEY_B": hx("#E8C33A"),
    "RG_KEY_X": hx("#3F7AD8"), "RG_KEY_Y": hx("#4CB05A"),
    "RG_KEY_L": hx("#A8B2C0"), "RG_KEY_R": hx("#A8B2C0"),
    "RG_KEY_SELECT": hx("#6C7686"), "RG_KEY_START": hx("#6C7686"),
    "RG_KEY_MENU": hx("#E8A22C"),
}
# B：保持现状（固件里那 13 个 565 值，每键一色）
PALETTE_B = {
    "RG_KEY_UP": v565(0x07E0), "RG_KEY_DOWN": v565(0x07FF),
    "RG_KEY_LEFT": v565(0xF800), "RG_KEY_RIGHT": v565(0xFD20),
    "RG_KEY_A": v565(0xF81F), "RG_KEY_B": v565(0xFFE0),
    "RG_KEY_X": v565(0x001F), "RG_KEY_Y": v565(0x781F),
    "RG_KEY_L": v565(0xC618), "RG_KEY_R": v565(0x8410),
    "RG_KEY_SELECT": v565(0xAFE0), "RG_KEY_START": v565(0xFC00),
    "RG_KEY_MENU": v565(0xFFFF),
}

LABELS = {
    "RG_KEY_UP": ("tri", "up"), "RG_KEY_DOWN": ("tri", "down"),
    "RG_KEY_LEFT": ("tri", "left"), "RG_KEY_RIGHT": ("tri", "right"),
    "RG_KEY_A": ("txt", "A"), "RG_KEY_B": ("txt", "B"),
    "RG_KEY_X": ("txt", "X"), "RG_KEY_Y": ("txt", "Y"),
    "RG_KEY_L": ("txt", "L"), "RG_KEY_R": ("txt", "R"),
    "RG_KEY_SELECT": ("txt", "SELECT"), "RG_KEY_START": ("txt", "START"),
    "RG_KEY_MENU": ("txt", "MENU"),
}


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c)


def tint(c, k):
    """向白色靠拢：标签用键色提亮版，比纯白更有色彩层次"""
    return tuple(int(v + (255 - v) * k) for v in c)


def label_scale(btn, kind, text):
    """标签尺寸：按按键盒子自适应（固件用同一套规则）"""
    if kind == "tri":
        return max(1, int(min(btn["w"], btn["h"]) * 0.46) // 8)
    # 文字：宽度留 20% 边距，取能放下的最大整数倍
    fit = int((btn["w"] * 0.80) / (len(text) * 8))
    fit = max(1, min(fit, int(btn["h"] * 0.62) // 8))
    return fit


def draw_text(im, text, cx, cy, scale, color, alpha):
    d = ImageDraw.Draw(im)
    total_w = len(text) * 8 * scale
    x = cx - total_w // 2
    y0 = cy - (8 * scale) // 2
    for ch in text:
        g = GLYPHS.get(ord(ch))
        if g:
            for gy in range(g["h"]):
                row = g["rows"][gy]
                for gx in range(g["w"]):
                    if row & (0x80 >> gx):
                        px = x + gx * scale
                        py = y0 + (g["y_off"] + gy) * scale
                        d.rectangle([px, py, px + scale - 1, py + scale - 1], fill=color + (alpha,))
        x += 8 * scale


def draw_tri(im, direction, cx, cy, size, color, alpha):
    d = ImageDraw.Draw(im)
    h = size // 2
    if direction == "up":
        pts = [(cx, cy - h), (cx - h, cy + h), (cx + h, cy + h)]
    elif direction == "down":
        pts = [(cx, cy + h), (cx - h, cy - h), (cx + h, cy - h)]
    elif direction == "left":
        pts = [(cx - h, cy), (cx + h, cy - h), (cx + h, cy + h)]
    else:
        pts = [(cx + h, cy), (cx - h, cy - h), (cx - h, cy + h)]
    d.polygon(pts, fill=color + (alpha,))


def build_button(btn, color, alpha_pct, pressed):
    """按 4x 超采样画一张按键层（边框环 + 填充 + 标签），再 BOX 降采样 => 覆盖率抗锯齿。
    固件在 lcd_init() 里做同样的一次性预渲染，每帧只做覆盖率*α 的混合。"""
    w, h = btn["w"], btn["h"]
    W, H = w * SS, h * SS
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)

    a = alpha_pct / 100.0
    radius = int(min(w, h) * 0.18) * SS
    bw = 3 * SS

    if pressed:
        fill_rgb, border_rgb, label_rgb = shade(color, 0.95), (255, 255, 255), (255, 255, 255)
        fa = int(255 * a * 0.80)
    else:
        fill_rgb, border_rgb, label_rgb = shade(color, 0.55), color, tint(color, 0.70)
        fa = int(255 * a * ALPHA_FILL)
    ba = int(255 * a * ALPHA_BORDER)
    la = int(255 * a * ALPHA_LABEL)

    d.rounded_rectangle([0, 0, W - 1, H - 1], radius=radius, fill=border_rgb + (ba,))
    d.rounded_rectangle([bw, bw, W - 1 - bw, H - 1 - bw],
                        radius=max(0, radius - bw), fill=fill_rgb + (fa,))

    kind, val = LABELS[btn["key"]]
    cx, cy = W // 2, H // 2
    if kind == "tri":
        s = label_scale(btn, "tri", "") * 8 * SS
        draw_tri(layer, val, cx, cy, s, label_rgb, la)
    else:
        s = label_scale(btn, "txt", val) * SS
        draw_text(layer, val, cx, cy, s, label_rgb, la)

    return layer.resize((w, h), Image.BOX)


# ---------------------------------------------------------------- 背景（假游戏画面）
def game_scene():
    """240x160 的程序化像素画，3x 最近邻放大 => 用来判断按键在真实画面旁的观感"""
    src = Image.new("RGB", (240, 160), (0, 0, 0))
    d = ImageDraw.Draw(src)
    bands = [(16, 28, 72), (24, 44, 104), (36, 62, 132), (52, 84, 160)]
    for i, c in enumerate(bands):
        d.rectangle([0, i * 16, 239, i * 16 + 15], fill=c)
    d.ellipse([188, 12, 216, 40], fill=(252, 236, 160))
    d.polygon([(0, 96), (48, 60), (96, 96)], fill=(40, 52, 88))
    d.polygon([(64, 96), (128, 48), (192, 96)], fill=(32, 42, 74))
    d.rectangle([0, 96, 239, 159], fill=(46, 92, 52))
    d.rectangle([0, 96, 239, 101], fill=(72, 132, 68))
    for x in range(4, 240, 16):
        d.line([(x, 104), (x + 2, 112)], fill=(96, 160, 84))
    for x, y in ((36, 72), (200, 64), (168, 80)):
        d.rectangle([x, y, x + 11, y + 11], fill=(226, 178, 60))
        d.rectangle([x + 2, y + 2, x + 9, y + 9], fill=(250, 226, 130))
    d.rectangle([112, 118, 127, 135], fill=(210, 96, 72))
    d.rectangle([115, 110, 124, 121], fill=(240, 200, 160))
    d.rectangle([112, 136, 118, 143], fill=(60, 60, 90))
    d.rectangle([121, 136, 127, 143], fill=(60, 60, 90))
    return src.resize((GAME_W, GAME_H), Image.NEAREST)


def background(kind="game"):
    im = Image.new("RGBA", (SCR_W, SCR_H), (0, 0, 0, 255))
    if kind == "game":
        im.paste(game_scene(), (GAME_X, GAME_Y))
    elif kind == "menu":       # launcher 是全屏 UI：按键会压在半亮背景上，另测一次可读性
        d = ImageDraw.Draw(im)
        d.rectangle([0, 0, SCR_W - 1, SCR_H - 1], fill=(28, 32, 48, 255))
        d.rectangle([60, 60, SCR_W - 60, SCR_H - 60], fill=(48, 56, 84, 255))
        d.rectangle([60, 60, SCR_W - 60, 132], fill=(96, 112, 168, 255))
        for i in range(6):
            d.rectangle([96, 168 + i * 72, SCR_W - 96, 216 + i * 72],
                        fill=(60, 70, 104, 255) if i % 2 else (72, 84, 124, 255))
    return im


def render(alpha_pct, palette, pressed_keys=(), bg="game", scale=1.0):
    im = background(bg)
    for btn in KEYMAP:
        layer = build_button(btn, palette[btn["key"]], alpha_pct, btn["key"] in pressed_keys)
        im.alpha_composite(layer, (btn["x"] - btn["w"] // 2, btn["y"] - btn["h"] // 2))
    if scale != 1.0:
        im = im.resize((int(SCR_W * scale), int(SCR_H * scale)), Image.LANCZOS)
    return im.convert("RGB")


def stack(rows, gap=8, bgcol=(16, 16, 20)):
    w = max(r.width for r in rows)
    h = sum(r.height for r in rows) + gap * (len(rows) - 1)
    out = Image.new("RGB", (w, h), bgcol)
    y = 0
    for r in rows:
        out.paste(r, (0, y))
        y += r.height + gap
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--alpha", type=int, default=None, help="只出这一档透明度")
    ap.add_argument("--scale", type=float, default=1.0, help="整体缩放（默认 1:1）")
    args = ap.parse_args()
    OUT_DIR.mkdir(exist_ok=True)

    if args.alpha is not None:
        out = OUT_DIR / f"touch-overlay-a{args.alpha}.png"
        render(args.alpha, PALETTE_A, (), scale=args.scale).save(out)
        print("wrote", out)
        return

    # 1) 主图：α=100%，调和配色 A，游戏中
    hero = render(100, PALETTE_A)
    hero.save(OUT_DIR / "touch-overlay-hero.png")

    # 2) 调色板 A / B 对比（缩到 0.58 便于同屏对比）
    pal = stack([render(100, PALETTE_A, scale=0.58), render(100, PALETTE_B, scale=0.58)])
    pal.save(OUT_DIR / "touch-overlay-palettes.png")

    # 3) 按下反馈：D-pad 左 + A + START 同时按下
    pressed = render(100, PALETTE_A, ("RG_KEY_LEFT", "RG_KEY_A", "RG_KEY_START"))
    pressed.save(OUT_DIR / "touch-overlay-pressed.png")

    # 4) 透明度 5 档（100/80/60/40/20），缩到 0.5
    rows = [render(a, PALETTE_A, scale=0.5) for a in (100, 80, 60, 40, 20)]
    stack(rows).save(OUT_DIR / "touch-overlay-opacity.png")

    # 5) 可读性：launcher 全屏 UI 背景 + 20% 最低档（最坏情况）
    leg = stack([render(100, PALETTE_A, bg="menu", scale=0.5),
                 render(20, PALETTE_A, bg="menu", scale=0.5)])
    leg.save(OUT_DIR / "touch-overlay-legibility.png")

    for f in sorted(OUT_DIR.glob("touch-overlay-*.png")):
        print("wrote", f)


if __name__ == "__main__":
    main()
