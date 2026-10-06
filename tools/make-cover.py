#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 M5Burner 发布封面（320x200）—— 屏幕内容是**真渲染**（同一套键位表/字形/配方），
不是假图：直接复用 tools/preview-touch-overlay.py 的渲染函数，所以封面与固件永远一致。

用法:  python3 tools/make-cover.py [版本号，默认从 git describe 取]
输出:  dist/m5burner-<版本>/cover-320x200.png
"""
import importlib.util
import re
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
COVER_W, COVER_H = 320, 200

# 复用预览工具的渲染（键位表 / 字形 / 按键与圆灯配方）
_spec = importlib.util.spec_from_file_location("pv", ROOT / "tools/preview-touch-overlay.py")
pv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pv)


FONT_CANDIDATES = (
    "/System/Library/Fonts/STHeiti Medium.ttc",      # 本机实测可用（PingFang 在 macOS 26 上已不存在！）
    "/System/Library/Fonts/STHeiti Light.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/System/Library/Fonts/Helvetica.ttc",
)


def font(size, bold=False):
    """挑一个**真能画中文**的字体。找不到就直接报错 ——
    绝不能再静默退回 `ImageFont.load_default()`（它没有中文字形 → 图上全是方块/乱码，
    2026-09-29 就是这么出的图）。"""
    for p in FONT_CANDIDATES:
        if not Path(p).exists():
            continue
        try:
            f = ImageFont.truetype(p, size)
        except Exception:
            continue
        if f.getbbox("电量")[2] > 4:          # 有中文墨迹才认
            if bold:
                try:
                    f.set_variation_by_name("Bold")
                except Exception:
                    pass
            return f
    raise SystemExit("✗ 找不到能画中文的系统字体，别用默认位图字体出图（会乱码）")


def portrait_screen():
    """720x1280 的真实竖屏界面：游戏画面（假像素画）+ 触摸按键 + 电量圆灯。"""
    im = pv.render(100, pv.PALETTE_A, bg="game").convert("RGBA")
    im.alpha_composite(pv.build_led(pv.LED_GREEN), (pv.LED_CX - 13, 1025 - 13))
    return im.convert("RGB")


def rounded(img, box, radius, fill):
    layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
    ImageDraw.Draw(layer).rounded_rectangle(box, radius=radius, fill=fill)
    img.alpha_composite(layer)


def main():
    ver = sys.argv[1] if len(sys.argv) > 1 else "0.4"
    out_dir = ROOT / f"dist/m5burner-{ver}"
    out_dir.mkdir(parents=True, exist_ok=True)

    if "--hero" in sys.argv:
        # 只有 hero：给 README 用的整屏界面图（720x1280，真渲染）
        dst = Path(sys.argv[sys.argv.index("--hero") + 1])
        dst.parent.mkdir(parents=True, exist_ok=True)
        portrait_screen().save(dst)
        print("wrote", dst)
        return

    cover = Image.new("RGBA", (COVER_W, COVER_H), (0, 0, 0, 255))
    # 背景：自上而下的暗蓝渐变（比纯黑有层次，缩略图里不糊）
    d = ImageDraw.Draw(cover)
    for y in range(COVER_H):
        t = y / (COVER_H - 1)
        d.line([(0, y), (COVER_W, y)],
               fill=(int(22 - 10 * t), int(25 - 12 * t), int(34 - 16 * t), 255))
    d.rectangle([0, 0, COVER_W - 1, COVER_H - 1], outline=(58, 64, 82, 255))

    # 设备外框（Tab5 竖屏拿法）+ 真屏幕内容
    body = (9, 6, 127, 194)
    rounded(cover, body, 9, (34, 36, 42, 255))
    scr_box = (17, 13, 119, 187)                      # 102x174 ≈ 720:1280 的竖屏比例
    scr = portrait_screen().resize((scr_box[2] - scr_box[0], scr_box[3] - scr_box[1]),
                                   Image.LANCZOS)
    cover.paste(scr, (scr_box[0], scr_box[1]))
    ImageDraw.Draw(cover).rectangle(scr_box, outline=(12, 13, 16, 255))
    # 屏幕玻璃反光（一条斜向浅色带，缩略图里更像"屏幕"）
    gloss = Image.new("RGBA", cover.size, (0, 0, 0, 0))
    ImageDraw.Draw(gloss).polygon([(scr_box[0], scr_box[1] + 40),
                                   (scr_box[2], scr_box[1] + 8),
                                   (scr_box[2], scr_box[1] + 34),
                                   (scr_box[0], scr_box[1] + 66)],
                                  fill=(255, 255, 255, 14))
    cover.alpha_composite(gloss)

    # 右侧文案
    tx = 136
    dd = ImageDraw.Draw(cover)
    dd.text((tx, 22), "retro-go", font=font(26, True), fill=(240, 242, 248, 255))
    dd.text((tx, 52), "Tab5", font=font(26, True), fill=(122, 176, 232, 255))
    dd.text((tx, 86), "GBA 模拟器 · 竖屏版", font=font(13), fill=(206, 212, 224, 255))
    dd.text((tx, 104), "v" + ver, font=font(17, True), fill=(232, 162, 44, 255))
    dd.text((tx, 130), "M5Stack Tab5 (ESP32-P4)", font=font(11), fill=(150, 158, 174, 255))
    dd.text((tx, 146), "触摸虚拟手柄 · 中文界面", font=font(11), fill=(150, 158, 174, 255))

    # 电量圆灯图例（与固件同色同配方）
    ly = 170
    dd.text((tx, ly - 6), "电量", font=font(11), fill=(150, 158, 174, 255))
    for i, c in enumerate((pv.LED_GREEN, pv.LED_ORANGE, pv.LED_RED)):
        led = pv.build_led(c, r=5)
        cover.alpha_composite(led, (tx + 30 + i * 16, ly - 4))
    dd.text((tx + 30 + 3 * 16 + 2, ly - 6), "充满/中/低", font=font(10), fill=(126, 132, 148, 255))

    out = out_dir / "cover-320x200.png"
    cover.convert("RGB").save(out)
    print("wrote", out, cover.size)


if __name__ == "__main__":
    main()
