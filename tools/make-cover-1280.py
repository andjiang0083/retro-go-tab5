#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 M5Burner 商店封面（1280×720 = burner.m5stack.com 规格）。

为什么单独一个脚本：
  上一张 1280×720 封面是临时手搓的，文案停留在"GBA 模拟器"时代（那时还只有 GBA），
  而且 dist 包里那张甚至印着 v0.4.3 —— 两者都会过期。这里把封面变成**可复现产物**：
    - 屏幕内容 = **真渲染**：复用 tools/preview-touch-overlay.py 的同一套键位表/字形/按键配方，
      封面上的虚拟手柄与固件永远一致（不是画一张"差不多"的图）
    - 文案改一处就够了（下面的 COPY 段），重新跑一次即可
    - **不印版本号**：版本号只在条目里出现，封面跨版本复用（2026-10-08 定的纪律）

用法:
  python3 tools/make-cover-1280.py                       # 出 releases/ 规范封面
  python3 tools/make-cover-1280.py --out /tmp/x.png      # 指定输出
  python3 tools/make-cover-1280.py --palette B           # 换叠加层配色
  python3 tools/make-cover-1280.py --also-dist 0.4.8     # 同时刷新 dist/m5burner-<ver>/cover-1280x720.png
"""

import argparse
import importlib.util
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
W, H = 1280, 720

# 复用预览工具的渲染（键位表 / 字形 / 按键与圆灯配方）—— 封面与固件同源
_spec = importlib.util.spec_from_file_location("pv", ROOT / "tools/preview-touch-overlay.py")
pv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pv)

FONT_CANDIDATES = (
    "/System/Library/Fonts/STHeiti Medium.ttc",      # 本机实测可用（PingFang 在 macOS 26 上已不存在）
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/System/Library/Fonts/Helvetica.ttc",
)

# ---------------------------------------------------------------- 文案（改这里就够了）
COPY = dict(
    brand_a="retro-go",                       # 白
    brand_b="Tab5",                           # 蓝
    headline="把 5 寸屏变成 11 台掌机",
    consoles="GBA · GB · GBC · NES · SNES · SMS · GG · COL · PCE · Lynx · G & W",
    feat1="触摸手柄 · 按键按真机布局 · 存档跨重启",
    feat2="中英双语界面 · ROM 放 SD 卡即玩",
    chip="GBA 支持 .zip：卡上省一半空间，快三成",
    dev="M5Stack Tab5 (ESP32-P4)",
    url="github.com/andjiang0083/retro-go-tab5",
)

# 配色：底色偏冷，强调色沿用固件里的琥珀（电量灯那支）
BG_TOP, BG_BOT = (20, 22, 30), (9, 10, 14)
INK, INK_DIM, INK_MID = (244, 246, 252), (128, 136, 152), (198, 205, 220)
ACCENT_BLUE = (122, 176, 232)
ACCENT_WARM = (232, 162, 44)


def font(size, bold=False):
    """挑一个**真能画中文**的字体；找不到就报错，绝不静默退回位图字体（会出方块）。"""
    for p in FONT_CANDIDATES:
        if not Path(p).exists():
            continue
        try:
            f = ImageFont.truetype(p, size)
        except Exception:
            continue
        if f.getbbox("电量")[2] > 4:            # 有中文墨迹才认
            if bold:
                try:
                    f.set_variation_by_name("Bold")
                except Exception:
                    pass
            return f
    raise SystemExit("✗ 找不到能画中文的系统字体")


def device_screen(palette, alpha=48):
    """720×1280 的真竖屏界面（游戏画面 + 触摸按键 + 电量圆灯），再缩到封面里的大小。

    ⚠ alpha 是**叠加层自身的不透明度**：0 = 按键完全看不见（屏幕下半全黑）。
      固件里这档值就是用户可调的"按键透明度"，封面取 48 让它既明显又不压游戏画面。
    """
    im = pv.render(alpha, palette, bg="game").convert("RGBA")
    im.alpha_composite(pv.build_led(pv.LED_GREEN), (pv.LED_CX - 13, 1025 - 13))
    return im.convert("RGB")


def rounded(draw_img, box, radius, fill):
    layer = Image.new("RGBA", draw_img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).rounded_rectangle(box, radius=radius, fill=fill)
    draw_img.alpha_composite(layer)


def shadow(canvas, box, radius, spread=14, alpha=110):
    """设备下方/右侧的柔和投影 —— 只做深度，不做发光（发光在小屏上一眼塑料感）。"""
    sh = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    d = ImageDraw.Draw(sh)
    for i, a in enumerate((alpha // 5, alpha // 3, alpha // 2)):
        grow = spread - i * (spread // 3)
        d.rounded_rectangle((box[0] - grow + 6, box[1] - grow + 10,
                             box[2] + grow + 6, box[3] + grow + 10),
                            radius=radius + grow, fill=(0, 0, 0, a))
    canvas.alpha_composite(sh.filter(__import__("PIL.ImageFilter", fromlist=["GaussianBlur"])
                                     .GaussianBlur(spread * 0.7)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(ROOT / "releases/cover-1280x720-retro-go-tab5-tab5.png"))
    ap.add_argument("--palette", default="A", choices=("A", "B"))
    ap.add_argument("--alpha", type=int, default=48, help="触摸叠加层不透明度（0=看不见）")
    ap.add_argument("--also-dist", metavar="VERSION", default=None,
                    help="同时刷新 dist/m5burner-<VERSION>/cover-1280x720.png")
    args = ap.parse_args()

    cover = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    d = ImageDraw.Draw(cover)
    for y in range(H):                                  # 冷色竖向渐变（比纯黑有层次）
        t = y / (H - 1)
        d.line([(0, y), (W, y)],
               fill=(int(BG_TOP[0] + (BG_BOT[0] - BG_TOP[0]) * t),
                     int(BG_TOP[1] + (BG_BOT[1] - BG_TOP[1]) * t),
                     int(BG_TOP[2] + (BG_BOT[2] - BG_TOP[2]) * t), 255))
    d.rectangle([0, 0, W - 1, H - 1], outline=(44, 48, 60, 255))

    # ---- 左：Tab5 竖屏设备（真渲染的屏幕）----
    scr_w, scr_h = 353, 628                             # 720:1280 比例
    sx, sy = 78, 46
    shell = (sx - 14, sy - 14, sx + scr_w + 14, sy + scr_h + 14)
    shadow(cover, shell, 16)
    rounded(cover, shell, 16, (36, 38, 45, 255))        # 机身
    rounded(cover, (shell[0] + 2, shell[1] + 2, shell[2] - 2, shell[3] - 2), 14, (28, 30, 36, 255))
    pal = pv.PALETTE_A if args.palette == "A" else pv.PALETTE_B
    cover.paste(device_screen(pal, args.alpha).resize((scr_w, scr_h), Image.LANCZOS), (sx, sy))
    ImageDraw.Draw(cover).rectangle((sx - 1, sy - 1, sx + scr_w, sy + scr_h), outline=(10, 11, 14, 255))

    # 屏幕玻璃反光：一条很淡的斜带（只做"这是块屏"的暗示，不做发光 —— 小屏上发光一眼塑料感）
    gloss = Image.new("RGBA", cover.size, (0, 0, 0, 0))
    ImageDraw.Draw(gloss).polygon([(sx, sy + 120), (sx + scr_w, sy + 30),
                                   (sx + scr_w, sy + 74), (sx, sy + 176)],
                                  fill=(255, 255, 255, 12))
    cover.alpha_composite(gloss)

    # 四角轻微压暗（纵向渐变之外的层次）
    vig = Image.new("L", (W, H), 0)
    vd = ImageDraw.Draw(vig)
    for i in range(170):
        vd.rectangle([i, i, W - 1 - i, H - 1 - i], outline=int(150 * (1 - i / 170)))
    cover.alpha_composite(Image.merge("RGBA", (
        Image.new("L", (W, H), 0), Image.new("L", (W, H), 0), Image.new("L", (W, H), 0),
        vig.filter(__import__("PIL.ImageFilter", fromlist=["GaussianBlur"]).GaussianBlur(48)))))

    # ---- 分隔线 ----
    d.line([(472, 92), (472, 470)], fill=(58, 64, 82, 255), width=2)

    # ---- 右：文案 ----
    tx = 516
    d.text((tx, 86), COPY["brand_a"], font=font(48, True), fill=INK)
    wa = d.textlength(COPY["brand_a"], font=font(48, True))
    d.text((tx + wa + 14, 86), COPY["brand_b"], font=font(48, True), fill=ACCENT_BLUE)

    d.text((tx, 172), COPY["headline"], font=font(46, True), fill=INK)

    # 机种清单：自动缩到放得下（放不下就拆两行）—— 防止右边被切掉（本轮踩过）
    max_w = 1230 - tx
    csize = 23
    while csize > 18 and d.textlength(COPY["consoles"], font=font(csize)) > max_w:
        csize -= 1
    if d.textlength(COPY["consoles"], font=font(csize)) > max_w:
        parts = [p.strip() for p in COPY["consoles"].split("·")]
        half = len(parts) // 2 + len(parts) % 2
        d.text((tx, 240), " · ".join(parts[:half]), font=font(csize), fill=ACCENT_BLUE)
        d.text((tx, 272), " · ".join(parts[half:]), font=font(csize), fill=ACCENT_BLUE)
        y1 = 320
    else:
        d.text((tx, 246), COPY["consoles"], font=font(csize), fill=ACCENT_BLUE)
        y1 = 306
    d.text((tx, y1), COPY["feat1"], font=font(23), fill=INK_MID)
    d.text((tx, y1 + 38), COPY["feat2"], font=font(23), fill=INK_MID)

    fchip = font(25)
    cw = d.textlength(COPY["chip"], font=fchip)
    chip_box = (tx - 16, 412 if y1 == 306 else 430, tx + cw + 24, (412 if y1 == 306 else 430) + 54)
    rounded(cover, chip_box, 10, (46, 36, 18, 255))
    d.rounded_rectangle(chip_box, radius=10, outline=(112, 78, 26, 255), width=2)
    d.text((tx, chip_box[1] + 12), COPY["chip"], font=fchip, fill=ACCENT_WARM)

    d.text((tx, 596), COPY["dev"], font=font(21), fill=INK_DIM)
    d.text((tx, 632), COPY["url"], font=font(21), fill=(150, 158, 174))

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    cover.convert("RGB").save(out)
    print("wrote", out, cover.size)
    if args.also_dist:
        dst = ROOT / f"dist/m5burner-{args.also_dist}/cover-1280x720.png"
        dst.parent.mkdir(parents=True, exist_ok=True)
        cover.convert("RGB").save(dst)
        print("wrote", dst)


if __name__ == "__main__":
    main()
