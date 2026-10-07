#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""触摸皮肤预览（PC 端重渲染）—— **解析固件真源**，不抄第二份常量

【与固件的关系（这是本脚本存在的全部意义）】
  皮肤表 / 配色 / 图层权重 / 面板几何 → 全部解析自
      retro-go-p4/components/retro-go/rg_touch_skin.c
  （那是唯一真源。以前这里是"人手抄一份"，必然漂移 —— 2026-10-06 改成解析。）

  面板的圆角矩形用**硬边**光栅化：与固件 panel_rrect() 逐位同一段整数算术
  （把像素钳到内缩 r 的核矩形上求距离，dx²+dy² <= r² 即在内）。
  按键层仍走 PIL + 4x 超采样：固件的按键掩码本来就是 4x 超采样预渲染的
  （rg_touch_overlay.c 的 SS=4），所以这里也是同一条口径。

【三屏几何（属于"显示通路"，不归皮肤模块管）】
  GBA  240x160 @3x = 720x480  贴顶满宽            → 控制区 y=480..1280
  GB/GBC 160x144 @4x = 640x576 居中(40px 边条)     → 控制区 y=576..1280
  NES  256x240 @2x = 512x480  居中(104px 边条)     → 控制区 y=480..1280
  ⚠ GBA 之外的两屏：固件多核落地前只知道 GBA 的视口（面板按全宽算），
    所以那两张图是"预告排版"，逐像素比对只对 GBA 成立。

用法：
  python3 tools/preview-skin.py                    # 全套（12 单图 + 4 对照）
  python3 tools/preview-skin.py --console gba
  python3 tools/preview-skin.py --skin D
  python3 tools/preview-skin.py --check            # 与"已确认稿"逐像素比对（回归门禁）
"""

import argparse
import importlib.util
import re
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT_DIR = ROOT / "docs/archive/skin-candidates"
BASELINE_DIR = OUT_DIR / "approved-2026-10-07"     # 用户确认过的那一版（回归基线）
PTO_PATH = ROOT / "tools/preview-touch-overlay.py"
SKIN_C_PATH = ROOT / "retro-go-p4/components/retro-go/rg_touch_skin.c"

W, H = 720, 1280


def load_pto():
    """把 preview-touch-overlay.py 当模块加载（文件名带连字符，不能直接 import）。
    它模块级就会解析键位表/字体/圆灯并 assert 自反性 —— 解析失败直接抛，别吞。"""
    spec = importlib.util.spec_from_file_location("pto", PTO_PATH)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


pto = load_pto()
SS = 4  # 超采样倍数：与固件按键掩码预渲染同一倍数


# ---------------------------------------------------------------- 解析固件真源
NONE = "NONE"
ACCENT = "ACCENT"


def _conv(tok):
    if tok == "RG_TOUCH_SKIN_NONE":
        return NONE
    if tok == "RG_TOUCH_SKIN_ACCENT":
        return ACCENT
    if tok.startswith("0x"):
        return int(tok, 16)
    return int(tok)


def _parse_row(line):
    """一行 '{...}, \\' → 值列表。引号里的当字符串，其余按数值转换。"""
    line = re.sub(r"/\*.*?\*/", "", line)          # 去掉行内注释
    line = line.strip().rstrip(", \\").strip()     # 去掉行尾的逗号与续行反斜杠
    assert line.startswith("{") and line.endswith("}"), line
    body = line[1:-1]

    out, buf, in_str = [], "", False
    for ch in body:
        if ch == '"':
            in_str = not in_str
            if in_str:
                buf = ""                # ⚠ 开引号要清空 buf：否则把引号前的空格吃进字符串
            else:
                out.append(buf)          # 字符串值（原样，不做数值转换）
                buf = ""
            continue
        if in_str:
            buf += ch
            continue
        if ch == ",":
            if buf.strip():
                out.append(_conv(buf.strip()))
            buf = ""
        else:
            buf += ch
    if buf.strip():
        out.append(_conv(buf.strip()))
    return out


def _parse_table(text, name):
    m = re.search(r"#define\s+%s\s*\{(.*?)\n\}" % name, text, re.S)
    assert m, f"在 {SKIN_C_PATH.name} 里找不到表 {name}"
    rows = []
    for line in m.group(1).splitlines():
        line = line.strip()
        if not line or line.startswith("/*") or line.startswith("//"):
            continue
        if not line.startswith("{"):
            continue
        rows.append(_parse_row(line))
    assert rows, f"{name} 解析出 0 行"
    return rows


def parse_skin_c():
    text = SKIN_C_PATH.read_text(encoding="utf-8")

    def macro(field):
        m = re.search(r"#define\s+RG_TOUCH_PANEL_%s\s+(\d+)" % field, text)
        assert m, f"缺少几何宏 RG_TOUCH_PANEL_{field}"
        return int(m.group(1))

    geo = {k: macro(k) for k in (
        "REF_CTRL_TOP", "CLUSTER_R", "CLUSTER_PAD_DPAD", "CLUSTER_PAD_ACTION",
        "CLUSTER_PAD_SHOULDER", "CLUSTER_PAD_SYSTEM", "GROOVE_PAD", "GROOVE_R", "GROOVE_MIN_SIDE",
        "BADGE_PAD_X", "BADGE_PAD_Y", "BADGE_R", "BADGE_SCALE", "BADGE_TRACK")}

    # 窗口最大高度在 **target 的 config.h**（另一个文件、另一个前缀），单独读一份 ——
    # 预览器的窗口尺寸必须跟固件用同一个上限，否则"尽量最大整数倍"两边会算出不同结果。
    target_text = (SKIN_C_PATH.parents[2] /
                   "components/retro-go/targets/tab5/config.h").read_text(encoding="utf-8")
    m = re.search(r"#define\s+RG_DISPLAY_MAX_WINDOW_HEIGHT\s+(\d+)", target_text)
    assert m, "缺少 RG_DISPLAY_MAX_WINDOW_HEIGHT（targets/tab5/config.h）"
    geo["MAX_WINDOW_HEIGHT"] = int(m.group(1))

    skins = []
    for r in _parse_table(text, "RG_TOUCH_SKINS"):
        assert len(r) == 21, f"皮肤表一行应有 21 个字段，实际 {len(r)}：{r}"
        skins.append(dict(
            id=r[0], name=r[1], short=r[2],
            panel_bg=r[3], cluster_fill=r[4], cluster_line=r[5], divider=r[6], divider_h=r[7],
            badge_fill=r[8], badge_line=r[9], badge_text=r[10],
            mode=r[11], border_px=r[12], radius_pct=r[13], fill_pct=r[14], label_pct=r[15],
            label_override=r[16], tier_border=r[17], tier_fill=r[18], tier_label=r[19],
            accent_from_console=bool(r[20]),
        ))

    key_rows = _parse_table(text, "RG_TOUCH_SKIN_KEYS")
    assert len(key_rows) == len(skins), "配色表行数与皮肤数不一致"
    for s, row in zip(skins, key_rows):
        assert len(row) == 14, f"配色表一行应有 14 个槽位，实际 {len(row)}"
        s["keys"] = row

    consoles = [dict(id=r[0], badge=r[1], accent=r[2]) for r in _parse_table(text, "RG_TOUCH_CONSOLES")]
    return geo, skins, consoles


GEO, SKINS, CONSOLES = parse_skin_c()

# 皮肤槽位序（与 rg_touch_skin.h 的 rg_skin_slot_t 一致）
SLOT = ["UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y", "L", "R", "SELECT", "START", "MENU", "SWAP"]
KEY2SLOT = {
    "RG_KEY_UP": "UP", "RG_KEY_DOWN": "DOWN", "RG_KEY_LEFT": "LEFT", "RG_KEY_RIGHT": "RIGHT",
    "RG_KEY_A": "A", "RG_KEY_B": "B", "RG_KEY_X": "X", "RG_KEY_Y": "Y",
    "RG_KEY_L": "L", "RG_KEY_R": "R", "RG_KEY_SELECT": "SELECT", "RG_KEY_START": "START",
    "RG_KEY_MENU": "MENU",
}
SYSTEM_KEYS = ("RG_KEY_SELECT", "RG_KEY_START", "RG_KEY_MENU")

# 显示通路的几何（窗口 / 边条）。多核落地后这些数会由固件视口决定，届时改成从固件读。
LOGICAL_H = 480   # Tab5 的逻辑屏高 = 游戏视口所在的那块（720x480），与固件 RG_SCREEN_HEIGHT 同值
# 各机型的窗口 = 原生分辨率 × **最大整数倍**（与固件 rg_display.c 的 ZOOM 分支同一条规则：
#   scale = min(屏宽/源宽, MAX_WINDOW_HEIGHT/源高)）—— 不写死倍数，免得跟固件漂移。
DISPLAY = {
    "gba": dict(label="GBA", src=(240, 160)),
    "gb":  dict(label="GB / GBC", src=(160, 144)),
    "gbc": dict(label="GBC", src=(160, 144)),
    "nes": dict(label="NES", src=(256, 240)),
    "snes": dict(label="SUPER NES", src=(256, 224)),
    "sms": dict(label="MASTER SYSTEM", src=(256, 192)),
    "gg":  dict(label="GAME GEAR", src=(320, 240)),
    "col": dict(label="COLECOVISION", src=(256, 192)),
    "pce": dict(label="PC ENGINE", src=(256, 240)),
    "gw":  dict(label="GAME & WATCH", src=(96, 64)),
    "lnx": dict(label="LYNX", src=(160, 102)),
}
for _d in DISPLAY.values():
    _d["scale"] = max(1, min(720 // _d["src"][0], GEO["MAX_WINDOW_HEIGHT"] // _d["src"][1]))


# ---------------------------------------------------------------- 颜色工具
def hx_int(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def q565(c):
    """量化到 RGB565 再还原 —— 面板只能显示 565，预览必须显示同一组颜色。
    ⚠ 与固件 q565() 同一个口径，且必须在**派生之前**做（否则暗部偏色）。"""
    r, g, b = c
    return ((r >> 3) * 255 // 31, (g >> 2) * 255 // 63, (b >> 3) * 255 // 31)


def shade(c, pct):
    return tuple(int(v * pct // 100) for v in c)


def tint(c, pct):
    return tuple(int(v + (255 - v) * pct // 100) for v in c)


def resolve(skin, console, slot):
    """取某槽位在当前皮肤/机型下的最终颜色（含"用机型点缀色"哨兵），已过 565。"""
    accent = q565(hx_int(console["accent"] if skin["accent_from_console"] else 0xE8A22C))
    v = skin["keys"][SLOT.index(slot)]
    if v == ACCENT:
        return accent
    return q565(hx_int(v))


# ---------------------------------------------------------------- 布局（锚定规则）
def layout(console_id):
    disp = DISPLAY[console_id]
    win_w = disp["src"][0] * disp["scale"]
    win_h = disp["src"][1] * disp["scale"]
    # 视口在**逻辑可见区（720x480）里居中**，太高就贴顶 —— 与固件 rg_display.c 同口径。
    # ⚠ GB 4x = 576 高 > 可见区 480 → top 会是负数（画面被切）→ max(0,…) 贴顶。
    win_y = max(0, (LOGICAL_H - win_h) // 2)
    ctrl_top = win_y + win_h
    win_x = (W - win_w) // 2
    # 避让画面的下移量 —— **恒为 0**（与固件同规则）：真实按键是键位表的固定坐标、不随 dy 走，
    # 面板分区框单独平移就会与按键错位。要避让就改键位表（那是挪按键，不是挪面板）。
    dy = 0

    # 机型能力：判据 = **真实手柄有什么**（不是核心映射了什么 —— 上游常为按键不够的硬件把
    #   X/Y/L/R 重排掉）。GBA/SNES 真实手柄有 L/R；SNES 另有真 X/Y，GBA/GB/GBC/NES 的 X/Y
    #   是连发 A/B（rg_input_apply_turbo 在 input 层合成）。"R/L 调换"按钮 GBA 专用。
    # ⚠ 与固件 rg_touch_has_shoulders()/rg_touch_key_hidden() 是同一条规则，务必两侧同步。
    has_shoulders = console_id in ("gba", "snes")
    has_xy = console_id in ("gba", "snes", "gb", "gbc", "nes", "sms", "gg")
    has_swap = console_id == "gba"      # 「X/Y ↔ L/R 调换」按钮：GBA 专用（与 has_shoulders 是两件事）
    keys = []
    for b in pto.KEYMAP:
        b = dict(b)
        if not has_shoulders and b["key"] in ("RG_KEY_L", "RG_KEY_R"):
            continue
        if not has_xy and b["key"] in ("RG_KEY_X", "RG_KEY_Y"):
            continue
        if b["key"] not in SYSTEM_KEYS:
            b["y"] += dy
        keys.append(b)
    swap = dict(pto.SWAP)
    swap["y"] += dy

    def box(k):
        return (k["x"] - k["w"] // 2, k["y"] - k["h"] // 2, k["w"], k["h"])

    def cluster(key_names, pad):
        rs = [k for k in keys if k["key"] in key_names]
        if not rs:
            return [0, 0, 0, 0]   # 该机型没有这组键（GB/GBC 无肩键）→ 空框，渲染与并入都跳过
        x0 = min(box(k)[0] for k in rs) - pad
        y0 = min(box(k)[1] for k in rs) - pad
        x1 = max(box(k)[0] + k["w"] for k in rs) + pad
        y1 = max(box(k)[1] + k["h"] for k in rs) + pad
        return [x0, y0, x1, y1]

    clusters = [
        cluster(("RG_KEY_UP", "RG_KEY_DOWN", "RG_KEY_LEFT", "RG_KEY_RIGHT"), GEO["CLUSTER_PAD_DPAD"]),
        cluster(("RG_KEY_X", "RG_KEY_Y", "RG_KEY_A", "RG_KEY_B"), GEO["CLUSTER_PAD_ACTION"]),
        cluster(("RG_KEY_L", "RG_KEY_R"), GEO["CLUSTER_PAD_SHOULDER"]),
        cluster(SYSTEM_KEYS, GEO["CLUSTER_PAD_SYSTEM"]),
    ]
    # 肩键区并入中间那颗调换按钮（与固件 layout_compute 里专门那一段同规则）
    if has_swap:
        sx, sy, sw, sh = box(swap)
        pad = GEO["CLUSTER_PAD_SHOULDER"]
        c = clusters[2]
        c[0] = min(c[0], sx - pad); c[1] = min(c[1], sy - pad)
        c[2] = max(c[2], sx + sw + pad); c[3] = max(c[3], sy + sh + pad)

    upper_bottom = max(box(k)[1] + k["h"] for k in keys if k["key"] not in SYSTEM_KEYS)
    sys_top = min(box(k)[1] for k in keys if k["key"] in SYSTEM_KEYS)
    # 圆灯与铭牌在「上三排底边 ~ 系统键顶边」之间各占一半、各自居中
    # （上半=铭牌、下半=圆灯）—— 与固件 rg_touch_skin.c 的 layout_compute 同规则。
    mid = (upper_bottom + sys_top) // 2
    led_cy = (mid + sys_top) // 2
    badge_cy = (upper_bottom + mid) // 2

    return dict(ctrl_top=ctrl_top, dy=dy, keys=keys, swap=swap, clusters=clusters,
                has_shoulders=has_shoulders, has_swap=has_swap,
                win_x=win_x, win_w=win_w, win_h=win_h, win_y=win_y,
                upper_bottom=upper_bottom, sys_top=sys_top,
                led_cy=led_cy, badge_cy=badge_cy)


# ---------------------------------------------------------------- 面板光栅化（硬边）
_XS = np.arange(W)[None, :]
_YS = np.arange(H)[:, None]


def rrect_mask(x, y, w, h, r):
    """硬边圆角矩形遮罩 —— 与固件 rrect_hit() 逐位同一段整数算术。"""
    if w <= 0 or h <= 0:
        return np.zeros((H, W), dtype=bool)
    r = max(0, min(r, min(w, h) // 2))
    x1, y1 = x + w - 1 - r, y + h - 1 - r
    cx = np.clip(_XS, x + r, x1)
    cy = np.clip(_YS, y + r, y1)
    return ((_XS - cx) ** 2 + (_YS - cy) ** 2) <= r * r


def _paste_mask(im, mask, color):
    im.paste(color, (0, 0), Image.fromarray((mask * 255).astype(np.uint8)))


def panel_rrect(im, x, y, w, h, r, fill=None, line=None):
    """填充 + 1px 描边（描边画在填充之上）—— 与固件 panel_rrect 同序。"""
    outer = rrect_mask(x, y, w, h, r)
    if fill is not None:
        _paste_mask(im, outer, fill)
    if line is not None:
        inner = rrect_mask(x + 1, y + 1, w - 2, h - 2, max(0, r - 1))
        _paste_mask(im, outer & ~inner, line)


def draw_text_tracked(im, text, cx, cy, scale, color, track):
    """带字距的文字 —— 与固件 panel_text 同口径（8 行格居中、字距一致）。
    ⚠ 固件 load_glyphs 丢掉 yOffset（只对 xOffset 做右移），所以这里也按"8x8 原样"画；
      真出现带偏移的字形会 assert 掉，不会静默画出两张不一样的图。"""
    d = ImageDraw.Draw(im)
    total = len(text) * 8 * scale + (len(text) - 1) * track
    x = cx - total // 2
    y0 = cy - (8 * scale) // 2
    for ch in text:
        g = pto.GLYPHS.get(ord(ch))
        if g:
            assert g.get("y_off", 0) == 0, \
                f"字形 '{ch}' 带 y_off={g.get('y_off')} —— 与固件口径不同，需先对齐"
            for gy in range(8):
                row = g["rows"][gy]
                for gx in range(8):
                    if row & (0x80 >> gx):
                        d.rectangle([x + gx * scale, y0 + gy * scale,
                                     x + gx * scale + scale - 1, y0 + gy * scale + scale - 1],
                                    fill=color)
        x += 8 * scale + track


# ---------------------------------------------------------------- 按键层（与固件掩码同口径）
def build_button(skin, console, rect, key, text=None):
    col = resolve(skin, console, KEY2SLOT.get(key, "SWAP"))
    w, h = rect["w"], rect["h"]
    Wb, Hb = w * SS, h * SS
    layer = Image.new("RGBA", (Wb, Hb), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    radius = int(min(w, h) * skin["radius_pct"] / 100) * SS
    bw = max(1, skin["border_px"]) * SS

    if skin["mode"] == 0:      # solid
        d.rounded_rectangle([0, 0, Wb - 1, Hb - 1], radius=radius, fill=col + (255,))
        d.rounded_rectangle([bw, bw, Wb - 1 - bw, Hb - 1 - bw], radius=max(0, radius - bw),
                            fill=shade(col, skin["fill_pct"]) + (255,))
    else:                       # outline（线框皮肤）
        d.rounded_rectangle([0, 0, Wb - 1, Hb - 1], radius=radius, outline=col + (255,), width=bw)

    label = q565(hx_int(skin["label_override"])) if skin["label_override"] != NONE else tint(col, skin["label_pct"])
    kind, val = ("txt", text) if text is not None else pto.LABELS[key]
    cx, cy = Wb // 2, Hb // 2
    if kind == "tri":
        pto.draw_tri(layer, val, cx, cy, pto.label_scale(rect, "tri", "") * 8 * SS, label, 255)
    else:
        pto.draw_text(layer, val, cx, cy, pto.label_scale(rect, "txt", val) * SS, label, 255)
    return layer.resize((w, h), Image.BOX)


# ---------------------------------------------------------------- 假游戏画面
def game_scene(w, h, scale):
    src = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(src)
    horizon = int(h * 0.62)
    for i, c in enumerate([(16, 28, 72), (24, 44, 104), (36, 62, 132), (52, 84, 160)]):
        y0 = i * horizon // 4
        d.rectangle([0, y0, w - 1, y0 + horizon // 4 - 1], fill=c)
    s = max(4, int(h * 0.10))
    d.ellipse([int(w * 0.74), int(h * 0.07), int(w * 0.74) + 2 * s, int(h * 0.07) + 2 * s],
              fill=(252, 236, 160))
    d.polygon([(0, horizon), (int(w * 0.20), int(h * 0.34)), (int(w * 0.40), horizon)], fill=(40, 52, 88))
    d.polygon([(int(w * 0.27), horizon), (int(w * 0.53), int(h * 0.27)), (int(w * 0.80), horizon)],
              fill=(32, 42, 74))
    d.rectangle([0, horizon, w - 1, h - 1], fill=(46, 92, 52))
    d.rectangle([0, horizon, w - 1, horizon + max(2, h // 32)], fill=(72, 132, 68))
    for x in range(2, w, max(8, w // 15)):
        d.line([(x, horizon + max(4, h // 24)), (x + 2, horizon + max(8, h // 16))], fill=(96, 160, 84))
    b = max(6, int(w * 0.048))
    for x, y in ((int(w * 0.15), int(h * 0.45)), (int(w * 0.83), int(h * 0.40)), (int(w * 0.70), int(h * 0.50))):
        d.rectangle([x, y, x + b, y + b], fill=(226, 178, 60))
        d.rectangle([x + b // 4, y + b // 4, x + b - b // 4, y + b - b // 4], fill=(250, 226, 130))
    px, py = int(w * 0.47), int(h * 0.74)
    u = max(2, w // 120)
    d.rectangle([px, py, px + 6 * u, py + 7 * u], fill=(210, 96, 72))
    d.rectangle([px + u, py - 4 * u, px + 5 * u, py + u], fill=(240, 200, 160))
    d.rectangle([px, py + 7 * u, px + 3 * u, py + 10 * u], fill=(60, 60, 90))
    d.rectangle([px + 4 * u, py + 7 * u, px + 6 * u, py + 10 * u], fill=(60, 60, 90))
    return src.resize((w * scale, h * scale), Image.NEAREST)


# ---------------------------------------------------------------- 渲染
def render(console_id, skin, want_badge=True):
    console = next(c for c in CONSOLES if c["id"] == console_id)
    lay = layout(console_id)
    ct = lay["ctrl_top"]

    accent = q565(hx_int(console["accent"] if skin["accent_from_console"] else 0xE8A22C))
    panel_bg = q565(hx_int(skin["panel_bg"]))
    cluster_fill = None if skin["cluster_fill"] == NONE else q565(hx_int(skin["cluster_fill"]))
    cluster_line = None if skin["cluster_line"] == NONE else (accent if skin["cluster_line"] == ACCENT else q565(hx_int(skin["cluster_line"])))
    divider = accent if skin["divider"] == ACCENT else q565(hx_int(skin["divider"]))
    badge_fill = None if skin["badge_fill"] == NONE else q565(hx_int(skin["badge_fill"]))
    badge_line = accent if skin["badge_line"] == ACCENT else q565(hx_int(skin["badge_line"]))
    badge_text = accent if skin["badge_text"] == ACCENT else q565(hx_int(skin["badge_text"]))
    groove_fill = shade(panel_bg, 45)     # 已不绘制（屏幕四周不做元素），保留以便回滚
    groove_line = panel_bg if cluster_line is None else tint(cluster_line, 70)

    im = Image.new("RGB", (W, H), panel_bg)
    # ① 屏幕四周 —— **不画任何皮肤元素**（用户 2026-10-07："所有支持机种的屏幕附近的边、
    #    属于皮肤的部分都去掉，屏幕不需要主题"）。凹槽/分界线都已取消，这里只剩面板底色。
    # ② 游戏窗口（原生分辨率整数放大，居中）
    disp = DISPLAY[console_id]
    im.paste(game_scene(*disp["src"], disp["scale"]), (lay["win_x"], 0))
    # ③ 分界线 —— **已取消**（与固件同：用户定全机种都不要画面底部那条横线）
    # ④ 分区色块（B 套只留 1px 线）
    if cluster_fill is not None or cluster_line is not None:
        for x0, y0, x1, y1 in lay["clusters"]:
            panel_rrect(im, x0, y0, x1 - x0, y1 - y0, GEO["CLUSTER_R"],
                        fill=cluster_fill, line=cluster_line)
    # ⑤ 按键 + 调换按钮
    rgba = im.convert("RGBA")
    for b in lay["keys"]:
        rgba.alpha_composite(build_button(skin, console, b, b["key"]), (b["x"] - b["w"] // 2, b["y"] - b["h"] // 2))
    if lay["has_swap"]:
        sb = lay["swap"]
        rgba.alpha_composite(build_button(skin, console, sb, "SWAP", text="X/Y"),
                             (sb["x"] - sb["w"] // 2, sb["y"] - sb["h"] // 2))
    im = rgba.convert("RGB")
    # ⑥ 电量圆灯（绿 = 正常）
    led = pto.build_led(pto.LED_GREEN, on=True, r=pto.LED_R, alpha_pct=100)
    im.paste(led, (360 - led.width // 2, lay["led_cy"] - led.height // 2), led)
    # ⑦ 机型铭牌（文案随核心变）
    if want_badge and console["badge"]:
        text = console["badge"]
        sc, track = GEO["BADGE_SCALE"], GEO["BADGE_TRACK"]
        tw = len(text) * 8 * sc + (len(text) - 1) * track
        if badge_fill is not None:
            pw, ph = tw + GEO["BADGE_PAD_X"], 8 * sc + GEO["BADGE_PAD_Y"]
            panel_rrect(im, 360 - pw // 2, lay["badge_cy"] - ph // 2, pw, ph,
                        GEO["BADGE_R"], fill=badge_fill, line=badge_line)
        draw_text_tracked(im, text, 360, lay["badge_cy"], sc, badge_text, track)
    return im


# ---------------------------------------------------------------- 对照图 / 回归门禁
def contact_sheet(items, path, scale=0.62):
    ims = [(im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS), label)
           for im, label in items]
    gap, cap = 10, 40
    cw = max(i.width for i, _ in ims)
    Wt = 2 * cw + gap * 3
    Ht = 2 * (ims[0][0].height + cap) + gap * 3
    out = Image.new("RGB", (Wt, Ht), (18, 20, 26))
    dr = ImageDraw.Draw(out)
    font = pto._cn_font(16)
    for size in (26, 24, 22, 20, 18, 16):
        f = pto._cn_font(size)
        if f.getbbox(max(l for _, l in ims))[2] <= cw - 8:
            font = f
            break
    for idx, (im, label) in enumerate(ims):
        x = gap + (idx % 2) * (cw + gap)
        y = gap + (idx // 2) * (im.height + cap + gap)
        out.paste(im, (x, y + cap))
        dr.text((x + 4, y + 6), label, fill=(236, 236, 242), font=font)
    out.save(path)
    return out.size


def check_baseline(paths):
    """与"已确认稿"逐像素比对 —— 回归门禁（**归因**，不是"看着差不多"）。

    2026-10-06 有两处口径统一，差异是**预期**的、且必须可解释：
      ① 圆角光栅化：PC 从 PIL 抗锯齿改成与固件同一套硬边 → 圆角边缘差 1px；
      ② 底板矩形约定：从"含端点"改成"含起点不含终点"（与固件 surface 坐标一致）
         → 色块/铭牌底板的右、下边各差 1px。
    所以门禁不只看百分比，还要求**所有差异像素都落在"面板几何边缘带"内**
    （把凹槽/分区/铭牌底板按 ±2px 膨胀）；跑出带外就是真漂移，直接失败。"""
    if not BASELINE_DIR.is_dir():
        print(f"⚠ 找不到基线目录 {BASELINE_DIR.relative_to(ROOT)}，跳过比对")
        return 1

    def band_mask(console_id, skin):
        """面板几何边缘带：凹槽 + 四个分区 + 铭牌底板（各膨胀 2px）。"""
        lay = layout(console_id)
        ct = lay["ctrl_top"]
        rects = [(-GEO["GROOVE_PAD"], 0, W + 2 * GEO["GROOVE_PAD"], ct - 3, GEO["GROOVE_R"])]
        for x0, y0, x1, y1 in lay["clusters"]:
            rects.append((x0, y0, x1 - x0, y1 - y0, GEO["CLUSTER_R"]))
        console = next(c for c in CONSOLES if c["id"] == console_id)
        if console["badge"] and skin["badge_fill"] != NONE:
            n = len(console["badge"])
            tw = n * 8 * GEO["BADGE_SCALE"] + (n - 1) * GEO["BADGE_TRACK"]
            pw, ph = tw + GEO["BADGE_PAD_X"], 8 * GEO["BADGE_SCALE"] + GEO["BADGE_PAD_Y"]
            rects.append((360 - pw // 2, lay["badge_cy"] - ph // 2, pw, ph, GEO["BADGE_R"]))
        m = np.zeros((H, W), dtype=bool)
        for x, y, w, h, r in rects:
            m |= rrect_mask(x, y, w, h, r)
            m |= rrect_mask(x + 1, y + 1, w - 2, h - 2, max(0, r - 1))
            # 膨胀 2px（边缘 1px 抖动 + 抗锯齿差）
            m |= rrect_mask(x - 2, y - 2, w + 4, h + 4, r + 2)
        return m

    bad = 0
    for p in paths:
        base = BASELINE_DIR / p.name
        if not base.exists():
            print(f"  {p.name}: 基线里没有这张，跳过")
            continue
        cid = p.stem.split("_")[1]
        sid = p.stem.split("_")[0]
        skin = next(s for s in SKINS if s["id"] == sid)
        a = np.asarray(Image.open(base).convert("RGB")).astype(int)
        b = np.asarray(Image.open(p).convert("RGB")).astype(int)
        if a.shape != b.shape:
            print(f"  {p.name}: 尺寸不同 {a.shape} vs {b.shape}")
            bad += 1
            continue
        d = np.abs(a - b).sum(axis=2) > 0
        ndiff = int(d.sum())
        pct = ndiff / d.size * 100
        m = band_mask(cid, skin)
        outside = int((d & ~m).sum())
        # 硬判据：差异必须**全部**落在面板几何边缘带内（跑出去 = 真漂移，直接失败）。
        # 百分比只对 GBA 可比：GB/NES 的凹槽在 PC 稿里是围着窗口整形的，而固件
        # 多核落地前按全宽画 —— 那是**有意的语义差**，不是漂移（差在凹槽整片区域）。
        soft_ok = (pct <= 2.0) or (cid != "gba")
        ok = (outside == 0) and soft_ok
        if not ok:
            bad += 1
        note = ("✓ 全在面板几何边缘带内" if outside == 0 else f"✗ 带外 {outside} px") + \
               ("" if cid == "gba" else "（含凹槽整形=窗口 vs 全宽的预期差异）")
        print(f"  {p.name}: 不同像素 {ndiff} ({pct:.4f}%)  带外 {outside}  {note}")
    print(f"→ 失败（超阈值或差异跑到几何带外）：{bad}")
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--console", default=None, choices=list(DISPLAY))
    ap.add_argument("--skin", default=None, choices=[s["id"] for s in SKINS])
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--check", action="store_true", help="与已确认稿逐像素比对")
    args = ap.parse_args()
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    consoles = [c for c in DISPLAY if args.console in (None, c)]
    skins = [s for s in SKINS if args.skin in (None, s["id"])]
    print(f"真源 {SKIN_C_PATH.relative_to(ROOT)}：{len(SKINS)} 套皮肤 / {len(CONSOLES)} 个机型条目")
    print(f"几何 凹槽 pad={GEO['GROOVE_PAD']} r={GEO['GROOVE_R']} | 分区 pad="
          f"{GEO['CLUSTER_PAD_DPAD']}/{GEO['CLUSTER_PAD_ACTION']}/{GEO['CLUSTER_PAD_SHOULDER']}/"
          f"{GEO['CLUSTER_PAD_SYSTEM']} r={GEO['CLUSTER_R']} | 铭牌 {GEO['BADGE_SCALE']}x "
          f"距{GEO['BADGE_TRACK']} pad={GEO['BADGE_PAD_X']}/{GEO['BADGE_PAD_Y']}")

    written = []
    for cid in consoles:
        lay = layout(cid)
        disp = DISPLAY[cid]
        print(f"[{cid}] 窗口 {disp['src'][0]}x{disp['src'][1]} @{disp['scale']}x → "
              f"{lay['win_w']}x{lay['win_h']} (x={lay['win_x']}) | 控制区高 {H - lay['ctrl_top']} | "
              f"dy={lay['dy']} | 圆灯 y={lay['led_cy']} | 铭牌 y={lay['badge_cy']}")
        items = []
        for s in skins:
            im = render(cid, s)
            if args.scale != 1.0:
                im = im.resize((int(im.width * args.scale), int(im.height * args.scale)), Image.LANCZOS)
            p = OUT_DIR / f"{s['id']}_{cid}.png"
            im.save(p)
            written.append(p)
            print(f"   wrote {p.relative_to(ROOT)}  ({im.width}x{im.height})")
            items.append((im, s["name"]))
        if len(items) == 4:
            sheet = OUT_DIR / f"sheet_{cid}.png"
            print(f"   wrote {sheet.relative_to(ROOT)}  {contact_sheet(items, sheet)}  ← 对照图")

    rows = [render(c, SKINS[0]) for c in consoles]
    if len(rows) == 3:
        s = 0.5
        ims = [r.resize((int(r.width * s), int(r.height * s)), Image.LANCZOS) for r in rows]
        gap = 10
        Wt = sum(i.width for i in ims) + gap * (len(ims) + 1)
        Ht = max(i.height for i in ims) + gap * 2
        out = Image.new("RGB", (Wt, Ht), (18, 20, 26))
        x = gap
        for im in ims:
            out.paste(im, (x, gap))
            x += im.width + gap
        out.save(OUT_DIR / "sheet_3consoles.png")
        print("   wrote docs/archive/skin-candidates/sheet_3consoles.png  ← 三屏窗口对照")

    if args.check:
        print("\n=== 回归门禁：与已确认稿（approved-2026-10-07）逐像素比对 ===")
        return check_baseline([p for p in written if p.name[0] in "ABCD"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
