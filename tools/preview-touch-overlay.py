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

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
LAYOUT_H = ROOT / "retro-go-p4/components/retro-go/targets/tab5/touch_layout.h"
OVERLAY_H = ROOT / "retro-go-p4/components/retro-go/rg_touch_overlay.h"
FONT_C = ROOT / "retro-go-p4/components/retro-go/fonts/basic8x8.c"
OUT_DIR = ROOT / "docs"

SCR_W, SCR_H = 720, 1280          # 【竖屏分支】逻辑空间 720x1280（横屏 1280x720 是旧版）
GAME_W, GAME_H = 720, 480          # 240x160 @ 3x 整数缩放，贴顶
GAME_X, GAME_Y = 0, 0
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
    """解析 8x8 点阵字体 —— **必须与固件同口径**（`rg_touch_overlay.c` 的 load_glyphs()）：
      每个字形 = [code:2][yOffset][width][height][xOffset][xDelta][bitmap]
      bitmap = **每行一个字节**（字节数 = height，不是按位紧密打包！），**MSB = 最左列**，
      xOffset = 整行右移的列数。
    ⚠ 2026-09-29 的用户反馈「图上文字镜像/乱码」就是这里错了：原来把 bitmap 当成"连续位流"解，
    位边界与真实排法错位（width<8 的字形会错位一整个字节），画出来每个字形都左右镜像。"""
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
        nbytes = h                                    # 每行一字节
        raw = data[i:i + nbytes]
        i += nbytes
        rows = [(raw[y] >> x_off) & 0xFF if y < len(raw) else 0 for y in range(h)]
        glyphs[code] = dict(w=w, h=h, rows=rows, y_off=y_off, x_delta=x_delta)
    return glyphs


KEYMAP = parse_keymap(LAYOUT_H)
GLYPHS = parse_font(FONT_C)


# ---------------------------------------------------------------- 圆灯常量解析
OVERLAY_C = ROOT / "retro-go-p4/components/retro-go/rg_touch_overlay.c"


def parse_led(path):
    """电量圆灯的几何与配色 —— **从固件源码解析**，不再手工抄一份。
    ⚠ 2026-09-29 走查 P2-18：这里原来是手抄的常量（LED_CX/LED_R + 三个色值），
    和固件各存一份、靠人工同步；结果灯的改动（充电由硬闪改呼吸）没反映到图里，
    图注还在写"充电时闪的就是它"。改成解析后就和键位表一样是单一数据源。"""
    text = path.read_text(encoding="utf-8")

    def num(name):
        m = re.search(rf"#define\s+{name}\s+(\d+)", text)
        if not m:
            raise SystemExit(f"解析固件失败：找不到 {name}（rg_touch_overlay.c 的圆灯宏改名字了？）")
        return int(m.group(1))

    block = re.search(r"static uint16_t batt_led_color\(int idx\)\s*\{(.*?)\n\}", text, re.S)
    if not block:
        raise SystemExit("解析固件失败：找不到 batt_led_color()")
    cols = re.findall(
        r"c565\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\)",
        block.group(1))
    if len(cols) < 3:
        raise SystemExit(f"解析固件失败：batt_led_color() 里只找到 {len(cols)} 个颜色（应为 绿/橙/红 三个）")
    return dict(cx=num("RG_BATT_LED_CX"), cy=num("RG_BATT_LED_CY"),
                r=num("RG_BATT_LED_R"), ring=num("RG_BATT_LED_RING"),
                green=tuple(int(v, 0) for v in cols[0]),
                orange=tuple(int(v, 0) for v in cols[1]),
                red=tuple(int(v, 0) for v in cols[2]))


LED = parse_led(OVERLAY_C)
LED_CX, LED_CY, LED_R, LED_RING = LED["cx"], LED["cy"], LED["r"], LED["ring"]
LED_GREEN, LED_ORANGE, LED_RED = LED["green"], LED["orange"], LED["red"]
# 充电呼吸的 4 档亮度（固件 batt_led_state() 里的 breath[4]，周期 340ms/档 ≈1.4s 一圈）
LED_BREATH = (255, 196, 148, 196)


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


# ---------------------------------------------------------------- X/Y ↔ L/R 调换
def parse_swap(path):
    """L/R 之间那颗调换按钮的几何 —— 从键位表解析（与圆灯同规矩：不手抄一份）。"""
    text = path.read_text(encoding="utf-8")

    def num(name):
        m = re.search(rf"#define\s+{name}\s+(\d+)", text)
        if not m:
            raise SystemExit(f"解析键位表失败：找不到 {name}（touch_layout.h 的调换按钮宏改名字了？）")
        return int(m.group(1))

    return dict(x=num("RG_TAB5_SWAP_X"), y=num("RG_TAB5_SWAP_Y"),
                w=num("RG_TAB5_SWAP_W"), h=num("RG_TAB5_SWAP_H"))


SWAP = parse_swap(LAYOUT_H)
SWAP_COLOR = hx("#E8A22C")     # = MENU 琥珀：调色板里专属于"系统/UI 控件"，不是游戏键


def parse_swap_pairs():
    """调换规则 —— 直接解析固件 rg_touch_overlay.h 里的 rg_touch_swap_key()，
    杜绝 PC 稿与固件各写一份（与圆灯/键位表同一个规矩）。"""
    text = OVERLAY_H.read_text(encoding="utf-8")
    body = re.search(r"rg_touch_swap_key\(rg_key_t k\)\s*\{(.*?)\n\}", text, re.S)
    if not body:
        raise SystemExit("解析失败：rg_touch_overlay.h 里找不到 rg_touch_swap_key()")
    pairs = dict(re.findall(r"case\s+(RG_KEY_\w+):\s*return\s+(RG_KEY_\w+);", body.group(1)))
    if not pairs:
        raise SystemExit("解析失败：rg_touch_swap_key() 里没有 case 分支（改写法了？）")
    for a, b in pairs.items():   # 自反性：应用两次必须回原样（"换回"就是同一个调用）
        if pairs.get(b) != a:
            raise SystemExit(f"固件的调换规则不是自反的：{a} -> {b}，但 {b} 没换回来")
    return pairs


SWAP_PAIRS = parse_swap_pairs()


def swap_key(k):
    return SWAP_PAIRS.get(k, k)


def shade(c, k):
    return tuple(max(0, min(255, int(v * k))) for v in c)


def tint(c, k):
    """向白色靠拢：标签用键色提亮版，比纯白更有色彩层次"""
    return tuple(int(v + (255 - v) * k) for v in c)


def label_scale(btn, kind, text):
    """标签尺寸：按按键盒子自适应（固件用同一套规则）
    ⚠ 2026-09-29 修：纵向留白原本写 0.62，固件是 **45%**（rg_touch_overlay.c 的
    label_geom：sy = h*45/100/8）—— 单字符键（A/B/X/Y）在预览里被放大到 6 倍，
    真机上只有 4 倍，PC 稿比实机"字大"。改成同一口径。"""
    if kind == "tri":
        return max(1, int(min(btn["w"], btn["h"]) * 0.46) // 8)
    # 文字：宽度留 20% 边距，取能放下的最大整数倍
    fit = int((btn["w"] * 0.80) / (len(text) * 8))
    fit = max(1, min(fit, int(btn["h"] * 0.45) // 8))
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
                    if row & (0x80 >> gx):        # 字模 MSB = 最左列（与固件一致）
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


def build_button(btn, color, alpha_pct, pressed, text=None):
    """按 4x 超采样画一张按键层（边框环 + 填充 + 标签），再 BOX 降采样 => 覆盖率抗锯齿。
    固件在 lcd_init() 里做同样的一次性预渲染，每帧只做覆盖率*α 的混合。
    text != None 时用它当标签（L/R 之间那颗调换按钮不是游戏键，标签是运行时的字符串）。"""
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

    kind, val = ("txt", text) if text is not None else LABELS[btn["key"]]
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


def render(alpha_pct, palette, pressed_keys=(), bg="game", scale=1.0, swap=False):
    """swap=True 时按"X/Y ↔ L/R 已调换"渲染（标签/配色都跟着功能走，与固件一致）。"""
    im = background(bg)
    for btn in KEYMAP:
        key = swap_key(btn["key"]) if swap else btn["key"]
        b = dict(btn, key=key)
        layer = build_button(b, palette[key], alpha_pct, key in pressed_keys)
        im.alpha_composite(layer, (btn["x"] - btn["w"] // 2, btn["y"] - btn["h"] // 2))
    # L/R 之间那颗调换按钮：常驻（琥珀 = 系统/UI 控件），标签显示"菱形位上现在是哪一对"
    sb = dict(key="SWAP", x=SWAP["x"], y=SWAP["y"], w=SWAP["w"], h=SWAP["h"])
    layer = build_button(sb, SWAP_COLOR, alpha_pct, False, text="L/R" if swap else "X/Y")
    im.alpha_composite(layer, (SWAP["x"] - SWAP["w"] // 2, SWAP["y"] - SWAP["h"] // 2))
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


# ---------------------------------------------------------------- 电量圆灯（按键同款配方）
# 用户反馈两轮：① 位置（"不居中"）② 风格（"其他按键都是加个框的"）。
# 结论：圆灯 = 把按键那套视觉配方套到圆上 —— 外圈 = 键色(α*0.90)、内芯 = 压暗 0.55(α*0.50)、
# 圈宽与按键边框同宽，颜色取现有调色板里的三个色，天然与整屏同一语言。
# 👉 几何（CX/CY/R/RING）与配色（绿=Y 键绿 / 橙=MENU 琥珀 / 红=A 键红）现在都由上面的
#    parse_led() **从固件源码**读出，这里不再手抄一份（走查 P2-18：手抄必然漂移，
#    充电由硬闪改呼吸那次图注就没跟上）。要改灯 → 改 rg_touch_overlay.c，再跑本工具。


def build_led(color, on=True, r=LED_R, alpha_pct=100, bright=255):
    """按键配方套到圆上。on=False = 熄灭（圈还在但压暗 → 闪烁时不会整块凭空消失）。
    bright = 亮度系数（0~255）：固件用它做**充电呼吸**（背景纯黑 → 按亮度压暗等价于
    按 alpha 向黑混合，所以这里用 alpha 近似即可）。圈宽取自固件的 RG_BATT_LED_RING。"""
    size = 2 * r + 2
    W = size * SS
    layer = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    a = alpha_pct / 100.0
    if on:
        ring, fill = color, shade(color, 0.55)
        ra, fa = int(255 * a * ALPHA_BORDER), int(255 * a * ALPHA_FILL)
    else:
        ring, fill = shade(color, 0.26), shade(color, 0.06)
        ra, fa = int(255 * a * ALPHA_BORDER * 0.8), int(255 * a * ALPHA_FILL)
    b = bright / 255.0
    ra, fa = int(ra * b), int(fa * b)
    d.ellipse([0, 0, W - 1, W - 1], fill=ring + (ra,))
    bw = LED_RING * SS
    d.ellipse([bw, bw, W - 1 - bw, W - 1 - bw], fill=fill + (fa,))
    return layer.resize((size, size), Image.BOX)


def led_control_area(led_y, on=True, color=LED_GREEN, crop=(150, 880, 570, 1230), scale=1.8,
                     bright=255):
    """整屏（含按键）渲染后在指定 y 合成圆灯，再裁控制区（带上/下两排按键做参照）。"""
    im = render(100, PALETTE_A).convert("RGBA")
    led = build_led(color, on=on, bright=bright)
    im.alpha_composite(led, (int(LED_CX) - led.width // 2, int(led_y) - led.height // 2))
    im = im.crop(crop).convert("RGB")
    return im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)


def _cn_font(size):
    """中文字体：**不许静默回退**（load_default 没有 CJK 字形 → 图上全是方块，
    2026-09-29 就是这么出的图）。候选串按本机实测可用排序，最后校验中文墨迹。"""
    for p in ("/System/Library/Fonts/STHeiti Medium.ttc",   # 本机实测可用
              "/System/Library/Fonts/Hiragino Sans GB.ttc",
              "/System/Library/Fonts/PingFang.ttc",          # macOS 26 起已不存在
              "/System/Library/Fonts/Supplemental/Songti.ttc"):
        try:
            f = ImageFont.truetype(p, size)
        except Exception:
            continue
        if f.getbbox("电量")[2] > 4:
            return f
    raise SystemExit("✗ 找不到能画中文的系统字体，别用默认位图字体出图（会乱码）")


def label_row(im, text):
    """图注条：自动缩到放得下（否则长句子会被静默裁掉 —— 2026-09-29 加的）"""
    band = Image.new("RGB", (im.width, 36), (22, 24, 30))
    dr = ImageDraw.Draw(band)
    f = _cn_font(22)
    for size in (22, 20, 18, 16, 14, 12):
        f = _cn_font(size)
        if f.getbbox(text)[2] <= im.width - 20:
            break
    dr.text((10, 6), text, fill=(232, 232, 238), font=f)
    return stack([band, im], gap=0)


def led_variants():
    """圆灯在最终位置的全部状态（刷机前存证 + 以后调色的参照）。
    位置/半径/圈宽/配色都来自固件源码（parse_led），不是手抄值。"""
    rows = [
        label_row(led_control_area(LED_CY, color=LED_GREEN), "绿：电量 ≥60%"),
        label_row(led_control_area(LED_CY, color=LED_ORANGE), "橙：20~60%"),
        label_row(led_control_area(LED_CY, color=LED_RED), "红：10~20%"),
        label_row(led_control_area(LED_CY, on=False, color=LED_RED),
                  "低电告警（<10%）：500ms 硬闪的暗相位"),
        label_row(led_control_area(LED_CY, on=False, color=LED_GREEN),
                  "普通闪烁的暗相位：只灭内芯、外圈压暗保留"),
    ]
    out = stack(rows)
    out.save(OUT_DIR / "led-variants.png")
    print("wrote", OUT_DIR / "led-variants.png", out.size)


def led_breath_variants():
    """**充电中 = 绿呼吸**（v0.4.1 起；以前是硬闪）：4 档亮度 255/196/148/196、每档 340ms。
    给用户看"呼吸长什么样"、也是以后改节奏的参照图。"""
    rows = [
        label_row(led_control_area(LED_CY, color=LED_GREEN, bright=b),
                  f"充电呼吸 第 {i + 1}/4 档：亮度 {b}/255")
        for i, b in enumerate(LED_BREATH)
    ]
    out = stack(rows)
    out.save(OUT_DIR / "led-charging-breath.png")
    print("wrote", OUT_DIR / "led-charging-breath.png", out.size)


# ---------------------------------------------------------------- 方向键：矢量扇区判定（提案）
# 目标（用户 2026-09-29 拍板）：**十字外观一个字不改**，只把"4 个矩形命中"换成"矢量扇区判定"。
# 收益：① 一指能出斜向 ② 滑过角区不断键（现在从上滑到左要穿过中心的 86px 空洞）
#      ③ 可触区从 4 个矩形扩到整圆（拇指不用对着准）
# 风险控制：轴区给 ±30°（普通按压不误触斜向），斜区只有 30° 宽（要明确推到角上才出斜向）。
# 本文件里的判定就是**将来要抄进 rg_input.c 的那份**（先在 PC 上把边界看顺眼，再移植）。
DPAD_PAD_R    = 135     # 可触半径（臂外沿 = 臂距 85 + 半宽 42 = 127，留 8px 余量）
DPAD_DEAD_R   = 30      # 中心死区半径（现状是 86×86 的空洞 → 直径缩到 60）
DPAD_AXIS_DEG = 30      # 轴区半角：与轴夹角 < 30° 判单轴；30°~60° 进入斜区
DPAD_HYST_DEG = 5       # 滞回：已经在斜向时，斜区放宽到 25°~65°（手抖不来回跳）
DPAD_DIAG_R   = 70      # 斜区还要"推出去"：半径 ≥ 70px 才认斜向（贴中心蹭到角不算）
DPAD_DIAG_R_LEAVE = 60  # 滞回：已斜向时退到 60px 以内才掉回单轴
DPAD_SCALE    = 2.2     # 出图时的放大倍数（原图 300×320 → 660×704，够看清边界）


def dpad_geom():
    """从键位表算十字几何（中心/臂距/到臂外沿的半径）——布局改了这里跟着走，不硬编码。"""
    m = {b["key"]: b for b in KEYMAP}
    up, dn, lf, rt = (m["RG_KEY_UP"], m["RG_KEY_DOWN"], m["RG_KEY_LEFT"], m["RG_KEY_RIGHT"])
    if not (up["x"] == dn["x"] and lf["y"] == rt["y"]
            and abs(lf["y"] - up["y"]) == abs(lf["x"] - up["x"])):
        print("⚠ 方向键不是规整十字（上下不同 x / 左右不同 y / 臂距不对称），"
              "判定按 上 与 左 的交点当中心：", up["x"], lf["y"])
    return dict(cx=up["x"], cy=lf["y"], arm=abs(lf["x"] - up["x"]),
                size=up["w"], r_edge=abs(lf["x"] - up["x"]) + up["w"] // 2)


DPAD = dpad_geom()


def dpad_judge(dx, dy, was_diag=False):
    """矢量扇区判定 → 键元组（空 = 无方向）。坐标系：原点 = 十字中心，y 向下为正。

    ⚠ 移植到 C 时只改这三处：死区半径 / 轴区半角 / 滞回带宽；三角函数用 atan2f。"""
    import math
    r = math.hypot(dx, dy)
    if r < DPAD_DEAD_R or r > DPAD_PAD_R:
        return ()
    a = math.degrees(math.atan2(abs(dy), abs(dx)))      # 0° = 横轴，90° = 纵轴
    lo, hi = (DPAD_AXIS_DEG - DPAD_HYST_DEG, 90 - DPAD_AXIS_DEG + DPAD_HYST_DEG) if was_diag \
        else (DPAD_AXIS_DEG, 90 - DPAD_AXIS_DEG)
    r_gate = DPAD_DIAG_R_LEAVE if was_diag else DPAD_DIAG_R
    if lo < a < hi and r >= r_gate:                      # 斜区：角度对 + 推得够远
        return (("RG_KEY_DOWN" if dy > 0 else "RG_KEY_UP"),
                ("RG_KEY_RIGHT" if dx > 0 else "RG_KEY_LEFT"))
    if a <= (lo + hi) / 2:                              # 横轴为主
        return ("RG_KEY_RIGHT" if dx > 0 else "RG_KEY_LEFT",)
    return ("RG_KEY_DOWN" if dy > 0 else "RG_KEY_UP",)


def dpad_judge_rects(dx, dy):
    """**现状**：4 个矩形各自命中（照抄 `rg_input.c` 当前实现）——用来同图对比。"""
    hits = []
    for b in KEYMAP:
        if b["key"] not in ("RG_KEY_UP", "RG_KEY_DOWN", "RG_KEY_LEFT", "RG_KEY_RIGHT"):
            continue
        if abs(dx - (b["x"] - DPAD["cx"])) <= b["w"] // 2 and \
           abs(dy - (b["y"] - DPAD["cy"])) <= b["h"] // 2:
            hits.append(b["key"])
    return tuple(hits)


def _key_zh(mask):
    """键元组 → 中文方位标签（图上给人看）。"""
    if not mask:
        return "无"
    order = {"RG_KEY_UP": "上", "RG_KEY_DOWN": "下", "RG_KEY_LEFT": "左", "RG_KEY_RIGHT": "右"}
    parts = [order[k] for k in ("RG_KEY_UP", "RG_KEY_DOWN", "RG_KEY_LEFT", "RG_KEY_RIGHT")
             if k in mask]
    return "+".join(parts)


# 采样点（相对中心，y 向下为正）——故意覆盖"现在的空洞/缝隙"和"新出现的斜区"
DPAD_SAMPLES = [
    (0, -85, "上臂正中"),
    (0, -40, "中心与上臂之间"),
    (0, 0, "正中"),
    (60, -60, "45° 角上"),
    (48, -48, "45° 但没推远"),
    (85, -30, "右臂偏上"),
    (30, -75, "上臂偏右"),
    (95, 20, "右臂偏下"),
    (25, 20, "近中心小偏移"),
    (-85, -85, "左上角远端"),
    (100, -100, "右上角再远"),
]


def _dpad_map(model):
    """把判定画到十字上：返回 (图, 每个采样点的结果文字)。
    model = "rects"（现状） / "vector"（提案）。"""
    from PIL import Image as _I
    g = DPAD
    x0, y0 = max(0, g["cx"] - 150), max(0, g["cy"] - 160)
    crop = (x0, y0, min(SCR_W, g["cx"] + 150), min(SCR_H, g["cy"] + 160))
    im = render(100, PALETTE_A).convert("RGBA").crop(crop)
    sc = DPAD_SCALE
    im = im.resize((int(im.width * sc), int(im.height * sc)), _I.LANCZOS).convert("RGBA")

    # 1) 铺判定底色（按最终分辨率逐像素算，边缘干净）
    import math
    zones = _I.new("RGBA", im.size, (0, 0, 0, 0))
    zp = zones.load()
    for Y in range(im.height):
        for X in range(im.width):
            lx = x0 + (X + 0.5) / sc
            ly = y0 + (Y + 0.5) / sc
            dx, dy = lx - g["cx"], ly - g["cy"]
            if model == "vector":
                r = math.hypot(dx, dy)
                if r > DPAD_PAD_R:
                    continue
                if r < DPAD_DEAD_R:
                    zp[X, Y] = (150, 150, 160, 62)          # 死区：灰
                    continue
                a = math.degrees(math.atan2(abs(dy), abs(dx)))
                if DPAD_AXIS_DEG < a < 90 - DPAD_AXIS_DEG and r >= DPAD_DIAG_R:
                    zp[X, Y] = (232, 150, 40, 74)           # 斜区：橙
                else:
                    zp[X, Y] = (70, 124, 224, 58)           # 轴区：蓝
            else:
                if dpad_judge_rects(dx, dy):
                    zp[X, Y] = (70, 124, 224, 58)
    im.alpha_composite(zones)

    # 2) 边界线（30 条黑描边 + 斜区射线 + 死区圆 + 可触圆）
    d = ImageDraw.Draw(im)

    def P(lx, ly):
        return ((lx - x0) * sc, (ly - y0) * sc)

    if model == "vector":
        for sx, sy in ((1, 1), (1, -1), (-1, 1), (-1, -1)):
            for ang in (DPAD_AXIS_DEG, 90 - DPAD_AXIS_DEG):
                rad = math.radians(ang)
                dx, dy = sx * math.cos(rad) * DPAD_PAD_R, sy * math.sin(rad) * DPAD_PAD_R
                d.line([P(g["cx"], g["cy"]), P(g["cx"] + dx, g["cy"] + dy)],
                       fill=(255, 214, 120, 190), width=1)
            # 斜区的"内边界"：半径 = DPAD_DIAG_R 的短弧（弧外才认斜向）
            inner = []
            for deg in range(DPAD_AXIS_DEG, 91 - DPAD_AXIS_DEG, 4):
                rad = math.radians(deg)
                inner.append(P(g["cx"] + sx * math.cos(rad) * DPAD_DIAG_R,
                               g["cy"] + sy * math.sin(rad) * DPAD_DIAG_R))
            for i in range(0, len(inner) - 1, 2):
                d.line([inner[i], inner[i + 1]], fill=(255, 214, 120, 190), width=1)
        for rr, col in ((DPAD_DEAD_R, (235, 235, 245, 200)), (DPAD_PAD_R, (170, 200, 255, 170))):
            c = P(g["cx"], g["cy"])
            rad = rr * sc
            d.ellipse([c[0] - rad, c[1] - rad, c[0] + rad, c[1] + rad], outline=col, width=1)
    else:
        for b in KEYMAP:
            if b["key"] in ("RG_KEY_UP", "RG_KEY_DOWN", "RG_KEY_LEFT", "RG_KEY_RIGHT"):
                p0, p1 = P(b["x"] - b["w"] / 2, b["y"] - b["h"] / 2), P(b["x"] + b["w"] / 2, b["y"] + b["h"] / 2)
                d.rectangle([p0, p1], outline=(170, 200, 255, 170), width=1)

    # 3) 采样点：黄点 + 序号
    font = _cn_font(int(15 * sc / 1.6))
    for i, (dx, dy, _) in enumerate(DPAD_SAMPLES, 1):
        c = P(g["cx"] + dx, g["cy"] + dy)
        rad = 5 * sc / 1.6
        d.ellipse([c[0] - rad, c[1] - rad, c[0] + rad, c[1] + rad],
                  fill=(255, 226, 90, 245), outline=(20, 20, 24, 255), width=2)
        d.text((c[0] + rad + 2, c[1] - rad - 2), str(i), fill=(255, 240, 150, 255), font=font,
               stroke_width=2, stroke_fill=(10, 10, 12, 255))

    # 4) 划一条"从左滑到上"的路径（半径 = 臂距的四分之一圆弧）——现状会断键，提案不断
    arc = []
    for deg in range(0, 91, 5):
        rad = math.radians(deg)
        arc.append(P(g["cx"] + g["arm"] * math.cos(rad), g["cy"] - g["arm"] * math.sin(rad)))
    for i in range(0, len(arc) - 1, 2):                       # 隔段画 = 虚线
        d.line([arc[i], arc[i + 1]], fill=(120, 240, 255, 230), width=2)

    cap = "现状：4 个矩形各自命中（中心 86px 空洞）" if model == "rects" else \
          "提案：矢量扇区（死区 30px / 轴区 ±30° / 斜区 30°~60° 且 r≥70）"
    res = []
    for i, (dx, dy, desc) in enumerate(DPAD_SAMPLES, 1):
        mask = dpad_judge_rects(dx, dy) if model == "rects" else dpad_judge(dx, dy)
        res.append((i, desc, dx, dy, _key_zh(mask)))
    return label_row(im.convert("RGB"), cap), res


def dpad_zone_figure():
    """出图：现状 vs 提案，同一批采样点逐个对照 + 一张结果表。"""
    top, res_now = _dpad_map("rects")
    bot, res_new = _dpad_map("vector")
    lines = ["采样点（相对十字中心，px）｜现状 → 提案"]
    for (i, desc, dx, dy, now), (_, _, _, _, new) in zip(res_now, res_new):
        mark = "  ← 变好" if (not now or now != new) else ""
        flag = "✓" if (not now or now != new) else " "
        lines.append(f"{flag} {i:2d}. {desc:<16s}({dx:4d},{dy:4d})   {now} → {new}{mark}")
    lines.append("")
    lines.append("蓝 = 轴区（单轴）　橙 = 斜区（双轴，且要推到 r≥70）　灰 = 死区　"
                 "青虚线 = 手指从左滑到上的路径")
    W = top.width
    for size in (20, 18, 16, 14):
        f = _cn_font(size)
        if max(f.getbbox(t)[2] for t in lines) <= W - 16:
            break
    band = Image.new("RGB", (W, int(size * 1.5) * len(lines) + 12), (18, 20, 26))
    dr = ImageDraw.Draw(band)
    for i, t in enumerate(lines):
        dr.text((8, 6 + i * int(size * 1.5)), t, fill=(226, 230, 238), font=f)
    out = stack([top, bot, band], gap=6)
    out.save(OUT_DIR / "dpad-zones.png")
    print("wrote", OUT_DIR / "dpad-zones.png", out.size)
    for l in lines[1:11]:
        print("   ", l)


def swap_figure():
    """X/Y ↔ L/R 调换：两个状态对照（默认 / 已调换）。
    裁控制区上半段 —— 肩键那一排（含中间的切换按钮）+ ABXY 菱形，只看标签与配色怎么变。"""
    crop = (0, 490, SCR_W, 920)
    rows = [
        label_row(render(100, PALETTE_A).convert("RGB").crop(crop),
                  "默认：菱形位 X/Y、肩键位 L/R；中间那颗切换按钮显示 X/Y"),
        label_row(render(100, PALETTE_A, swap=True).convert("RGB").crop(crop),
                  "调换后：菱形位变 R/L（蓝/绿→浅灰）、肩键位变 X/Y；切换按钮显示 L/R"),
    ]
    out = stack(rows, gap=6)
    out.save(OUT_DIR / "swap-yx-lr.png")
    print("wrote", OUT_DIR / "swap-yx-lr.png", out.size)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--alpha", type=int, default=None, help="只出这一档透明度")
    ap.add_argument("--scale", type=float, default=1.0, help="整体缩放（默认 1:1）")
    ap.add_argument("--led", action="store_true", help="只出电量圆灯的位置/样式对比图")
    ap.add_argument("--dpad", action="store_true",
                    help="只出方向键判定对比图（现状 4 矩形 vs 矢量扇区提案）")
    ap.add_argument("--swap", action="store_true",
                    help="只出 X/Y ↔ L/R 调换的两个状态对照图")
    args = ap.parse_args()
    OUT_DIR.mkdir(exist_ok=True)

    if args.swap:
        swap_figure()
        return

    if args.dpad:
        dpad_zone_figure()
        return

    if args.led:
        led_variants()
        led_breath_variants()
        return

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

    # 6) 电量圆灯：电量三色 + 低电告警暗相位 + **充电呼吸四档**（v0.4.1 起充电是呼吸）
    led_variants()
    led_breath_variants()

    # 7) X/Y ↔ L/R 调换：两个状态对照（那颗切换按钮的标签 + 四个键的标签/配色）
    swap_figure()

    for f in sorted(OUT_DIR.glob("touch-overlay-*.png")):
        print("wrote", f)


if __name__ == "__main__":
    main()
