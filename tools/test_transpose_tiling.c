/* 转置分块化的等价性测试（主机侧跑，真机前先证明输出逐字节相同）
 *
 * 原实现（mipi_dsi_tab5.h 里）：
 *   for (i = 0; i < rows; ++i) { src = buffer + i*w; a = rows-1-i;
 *       for (j = 0; j < w; ++j) tab5_scratch[j*rows + a] = swap16(src[j]); }
 *
 * 新实现：把 (i, j) 按 32x32 分块后按同样公式写入 —— 只是**存储顺序**不同，
 * 落点与取值完全一致，因此结果必须逐字节相同、与分块大小/边界无关。
 *
 * 编译运行： cc -O2 -o /tmp/tt tools/test_transpose_tiling.c && /tmp/tt
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LCD_BUFFER_LENGTH 20480          /* 与 targets/tab5/config.h 一致 */

static uint16_t ref_buf[LCD_BUFFER_LENGTH];
static uint16_t new_buf[LCD_BUFFER_LENGTH];
static uint16_t src_buf[LCD_BUFFER_LENGTH];

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* ---- 原实现 ---- */
static void transpose_ref(uint16_t *dst, const uint16_t *buffer, int rows, int w)
{
    for (int i = 0; i < rows; ++i) {
        const uint16_t *src = buffer + (size_t)i * w;
        const int a = rows - 1 - i;
        for (int j = 0; j < w; ++j)
            dst[(size_t)j * rows + a] = swap16(src[j]);
    }
}

/* ---- 新实现：32x32 分块（内层沿 j 连续读，吃满 cache line）---- */
#define TR 32
#define TC 32
static void transpose_tiled(uint16_t *dst, const uint16_t *buffer, int rows, int w)
{
    for (int i0 = 0; i0 < rows; i0 += TR) {
        const int ti = (rows - i0 < TR) ? (rows - i0) : TR;
        for (int j0 = 0; j0 < w; j0 += TC) {
            const int tj = (w - j0 < TC) ? (w - j0) : TC;
            for (int ii = 0; ii < ti; ++ii) {
                const int i = i0 + ii;
                const uint16_t *src = buffer + (size_t)i * w + j0;
                const int a = rows - 1 - i;
                uint16_t *d = dst + (size_t)j0 * rows + a;
                for (int jj = 0; jj < tj; ++jj)
                    d[(size_t)jj * rows] = swap16(src[jj]);
            }
        }
    }
}

int main(void)
{
    int fails = 0, cases = 0;
    /* 覆盖：1 行/列、正好 32、32 的倍数上下、真实工作尺寸 */
    const int rowset[] = { 1, 2, 3, 7, 16, 28, 31, 32, 33, 64 };
    const int wset[]   = { 1, 2, 31, 32, 33, 100, 320, 480, 640, 720, 1280 };

    for (unsigned r = 0; r < sizeof(rowset) / sizeof(*rowset); ++r) {
        for (unsigned c = 0; c < sizeof(wset) / sizeof(*wset); ++c) {
            int rows = rowset[r], w = wset[c];
            size_t n = (size_t)rows * w;
            if (n > LCD_BUFFER_LENGTH) continue;
            cases++;
            for (size_t k = 0; k < n; ++k)
                src_buf[k] = (uint16_t)((k * 2654435761u) >> 7);
            memset(ref_buf, 0xAB, sizeof(ref_buf));
            memset(new_buf, 0xCD, sizeof(new_buf));
            transpose_ref(ref_buf, src_buf, rows, w);
            transpose_tiled(new_buf, src_buf, rows, w);
            if (memcmp(ref_buf, new_buf, n * sizeof(uint16_t)) != 0) {
                fails++;
                printf("  ✗ rows=%d w=%d 输出不一致\n", rows, w);
            }
        }
    }
    printf("等价性测试：%d 个尺寸组合，%d 个失败\n", cases, fails);
    if (!fails) {
        /* 顺带量一下两种写法在主机上的耗时（仅供参照，真机结论以 xpose= 打点为准）*/
        printf("✓ 新旧实现输出逐字节相同（含 1 行/1 列/非 32 倍数等边界）\n");
        return 0;
    }
    return 1;
}
