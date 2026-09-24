#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* 内置 CJK 点阵字库（缝合像素字体 12px，OFL-1.1）。
 *
 * 数据来源：flash 只读分区 "cjkfont"（type=data, subtype=0x40），由 rg_tool.py 在打包镜像时
 * 从 retro-go-p4/assets/cjk12.bin 写入；该文件由 tools/gen_cjk_font.py 从 BDF 生成。
 * 用 esp_partition_mmap 直接映射：零加载时间（不走 PSRAM 拷贝）、不占 app 分区、
 * 所有 app 共享一份、换/拔 SD 卡都不影响。
 *
 * 格式（小端）："RGF1" + uint16 cell_w + uint16 cell_h + uint32 count + uint32 index_off
 *              + uint32 glyph_off；索引为升序码位数组，字形为 cell_h 行 × 每行 2 字节
 *              （高位在左）。
 */

bool rg_cjk_init(void);          /* 幂等；找不到字库也不致命（只是没有汉字） */
bool rg_cjk_ready(void);

/* 把码位 c 的字形填进 output，返回该字形的**原生宽度**。
 *
 * 宽度约定：返回的是"一个汉字占一个 em"，即与拉丁字体的 font->height 同量级，
 * 这样 GUI 现有的等比缩放（points / font->height）会自动把汉字放到正确大小，
 * 宽度测量、居中、右对齐、截断（不劈字）全部无需改动。
 *
 * output 为 NULL 时只量宽度。字库里没有这个码位返回 0（调用方回退显示方块）。
 *
 * output 的位图约定与内置字体完全一致（rg_gui.c 绘制循环读取方式 (row >> sx) & 1）：
 *   * points 行，每行一个 uint32；
 *   * **bit 0 = 最左像素**（内置字体解码即 `row |= (1 << (xOffset + x))`）；
 *   * 行内只放**原生宽度**（12 列）的像素，**横向缩放由调用方按 points/font_height 完成**。
 * 早先两处都写错过：输出 MSB 对齐 → 汉字左右镜像；铺满 points 列 → 39 列塞进 32 位溢出。 */
size_t rg_cjk_glyph(uint32_t *output, int points, int font_height, uint32_t c);
