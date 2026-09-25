#!/usr/bin/env python3
"""把 IDF 的 DPI 帧缓冲分配改成 128 字节对齐（本地 IDF 补丁，幂等）。

背景（2026-09-25 实测）
------------------------------------------------------------
PPA（像素处理加速器）要求缓冲 **128 字节对齐**，但 IDF 用
`heap_caps_calloc` 分配 MIPI-DPI 的面板帧缓冲，只保证 **cache line（64 字节）** 对齐
（IDF 源码自己的注释也这么写）。后果：**每一次把 PPA 写进面板帧缓冲的 op 都被拒**
（返回 ESP_ERR_INVALID_ARG = 0x102），真机实测 fb 地址低 7 位 = 64。

这条错配是我们"PPA 慢 25 倍"的真正原因 —— 不是参数错，是目的地缓冲根本没资格被 PPA 写。
对比：R8T5（Tab5 上的 PICO-8 模拟器）的 PPA 能用，是因为它的帧缓冲来自 M5Unified/LovyanGFX
（自己管、128 对齐），**不是** IDF 分配的这块。

为什么必须改 IDF 而不能在项目里绕
------------------------------------------------------------
面板必须扫描它自己那块 fb，而 IDF 无条件分配、没有传入外部缓冲的 hook，
所以想让"面板正在扫的那块缓冲"满足 128 对齐，只能改分配处。
（备选方案"PPA 写进自己的对齐缓冲再拷过去"实测不可行：多一趟 ~10ms 拷贝。）

本脚本幂等：已改过就跳过。改的是**本地 IDF**（~/.espressif/...），不是项目代码，
所以**换机器要重跑一次**。用法：

    python3 tools/patch-idf-dpi-fb-align.py

可用 IDF_PATH 环境变量指定要改的 IDF；否则取 ~/.espressif/*/esp-idf 里最新的一个。
"""

import glob
import os
import sys

OLD = ("uint8_t *frame_buffer = heap_caps_calloc(1, fb_size, "
       "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);")

NEW = ("/* [本地改动 patch-idf-dpi-fb-align] PPA 要求 128 字节对齐，而 calloc 只保证\n"
       "         * cache line(64) 对齐 —— 会导致所有 PPA op 被拒(ESP_ERR_INVALID_ARG)。 */\n"
       "        uint8_t *frame_buffer = heap_caps_aligned_calloc(128, 1, fb_size, "
       "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);")

MARKER = "heap_caps_aligned_calloc(128, 1, fb_size"


def find_idf():
    """⚠ 不要"猜一个"。本地常装着多个 IDF（项目在用的可能是旧版本，~/.espressif 下
    还躺着更新的），按"最新"猜会改错工具链 —— 第一次运行就踩了这个坑（改到了 v6.0.2，
    而项目用的是 v5.5.4）。所以：只认 IDF_PATH，没设就拒绝执行并列出候选。"""
    if os.environ.get("IDF_PATH"):
        return os.environ["IDF_PATH"]
    cands = sorted(glob.glob(os.path.expanduser("~/.espressif/*/esp-idf")))
    if cands:
        print("✗ 未设置 IDF_PATH，拒绝猜测要改哪个 IDF。本机候选：")
        for c in cands:
            print(f"    IDF_PATH={c} python3 tools/patch-idf-dpi-fb-align.py")
    return None


def main():
    idf = find_idf()
    if not idf:
        print("✗ 找不到 IDF。设置 IDF_PATH，或确认 ~/.espressif/*/esp-idf 存在。")
        return 1

    path = os.path.join(idf, "components/esp_lcd/dsi/esp_lcd_panel_dpi.c")
    if not os.path.exists(path):
        print(f"✗ 文件不存在：{path}")
        return 1

    with open(path, encoding="utf-8") as fh:
        src = fh.read()

    if MARKER in src:
        print(f"✓ 已经是 128 对齐，无需改动：{path}")
        return 0

    if OLD not in src:
        print("✗ 没找到预期的那行分配代码（IDF 版本可能不同，需要人工核对后再改）。")
        print(f"  文件：{path}")
        return 1

    with open(path, "w", encoding="utf-8") as fh:
        fh.write(src.replace(OLD, NEW, 1))

    print(f"✓ 已把 DPI 帧缓冲分配改为 128 字节对齐：{path}")
    print("  下一步：重新编译 + 刷机，然后看 PPA-FRAME 探针的 err 是否变成 0x0。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
