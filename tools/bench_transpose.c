/* 转置访存模式微基准 —— 验证"跨步写导致 cache line 放大"假设
 *
 * 背景：Tab5 的 90° 旋转把"若干逻辑行"转成物理竖条，驱动里的实现是
 *     for i: for j: dst[j*rows + (rows-1-i)] = swap16(src[i*w + j]);
 * 目标地址步长 = rows 个 uint16，16 行时 = 32 字节 = 恰好一个 cache line
 * => 每次写只脏一条新 line，写放大接近 rows 倍。
 *
 * 本程序对比三种实现，并做逐字节正确性校验：
 *   A. strided  —— 当前驱动实现
 *   B. tiled    —— 分块转置（16x16 块，读写都留在 cache 内）
 *   C. noswap   —— 与 B 相同但不做字节序交换（量化 swap 本身的成本）
 *
 * 用法：cc -O2 -o bench_transpose bench_transpose.c && ./bench_transpose
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#define W_DEF    720     /* 逻辑列数（GBA 视口宽） */
#define ROWS_DEF 16      /* 每次推送的逻辑行数 */
#define REP      200000  /* 迭代次数：模拟一整局游戏的推送总量 */

static uint16_t *src, *ref, *dst;

static inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

/* A. 当前驱动实现：跨步写 */
static void transpose_strided(const uint16_t *s, uint16_t *d, int rows, int w)
{
    for (int i = 0; i < rows; ++i) {
        const uint16_t *sr = s + (size_t)i * w;
        const int a = rows - 1 - i;
        for (int j = 0; j < w; ++j)
            d[(size_t)j * rows + a] = swap16(sr[j]);
    }
}

/* B. 分块转置：T×T 块，源行在块内被多次复用，目标写连续 */
#define T 16
static void transpose_tiled(const uint16_t *s, uint16_t *d, int rows, int w)
{
    for (int i0 = 0; i0 < rows; i0 += T) {
        int i1 = (i0 + T < rows) ? i0 + T : rows;
        for (int j0 = 0; j0 < w; j0 += T) {
            int j1 = (j0 + T < w) ? j0 + T : w;
            for (int j = j0; j < j1; ++j) {
                uint16_t *dcol = d + (size_t)j * rows;
                const uint16_t *scol = s + j;
                for (int i = i0; i < i1; ++i)
                    dcol[rows - 1 - i] = swap16(scol[(size_t)i * w]);
            }
        }
    }
}

/* C. 分块但省略字节序交换 */
static void transpose_tiled_noswap(const uint16_t *s, uint16_t *d, int rows, int w)
{
    for (int i0 = 0; i0 < rows; i0 += T) {
        int i1 = (i0 + T < rows) ? i0 + T : rows;
        for (int j0 = 0; j0 < w; j0 += T) {
            int j1 = (j0 + T < w) ? j0 + T : w;
            for (int j = j0; j < j1; ++j) {
                uint16_t *dcol = d + (size_t)j * rows;
                const uint16_t *scol = s + j;
                for (int i = i0; i < i1; ++i)
                    dcol[rows - 1 - i] = scol[(size_t)i * w];
            }
        }
    }
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double bench(void (*fn)(const uint16_t *, uint16_t *, int, int), int rows, int w)
{
    double t0 = now_ms();
    for (int r = 0; r < REP; ++r)
        fn(src, dst, rows, w);
    return now_ms() - t0;
}

int main(int argc, char **argv)
{
    int rows = (argc > 1) ? atoi(argv[1]) : ROWS_DEF;
    int w    = (argc > 2) ? atoi(argv[2]) : W_DEF;

    src = aligned_alloc(64, (size_t)rows * w * 2);
    ref = aligned_alloc(64, (size_t)rows * w * 2);
    dst = aligned_alloc(64, (size_t)rows * w * 2);
    for (int i = 0; i < rows * w; ++i)
        src[i] = (uint16_t)(i * 2654435761u >> 13);   /* 伪随机，避免全零被优化 */

    transpose_strided(src, ref, rows, w);

    printf("块: %d 行 x %d 列 = %d 像素 (%.1f KB)   迭代 %d 次\n",
           rows, w, rows * w, rows * w * 2 / 1024.0, REP);

    /* 正确性：三种实现必须逐字节一致 */
    memset(dst, 0, (size_t)rows * w * 2);
    transpose_tiled(src, dst, rows, w);
    printf("正确性 tiled  vs strided: %s\n", memcmp(ref, dst, (size_t)rows * w * 2) == 0 ? "一致 ✓" : "不一致 ✗");
    memset(dst, 0, (size_t)rows * w * 2);
    transpose_tiled_noswap(src, dst, rows, w);
    printf("正确性 noswap vs strided: %s (预期不一致，仅用于计时)\n",
           memcmp(ref, dst, (size_t)rows * w * 2) == 0 ? "一致" : "不一致");

    /* 计时 */
    double a = bench(transpose_strided, rows, w);
    double b = bench(transpose_tiled, rows, w);
    double c = bench(transpose_tiled_noswap, rows, w);
    double px = (double)REP * rows * w;

    printf("\n%-22s %9.1f ms   每像素 %6.2f ns   相对 %5.2fx\n", "A strided (当前)", a, a * 1e6 / px, 1.0);
    printf("%-22s %9.1f ms   每像素 %6.2f ns   相对 %5.2fx\n", "B tiled (分块)", b, b * 1e6 / px, a / b);
    printf("%-22s %9.1f ms   每像素 %6.2f ns   相对 %5.2fx\n", "C tiled 无swap", c, c * 1e6 / px, a / c);

    printf("\n推算到真机：每块 %d 行，每秒推 ~%d 块 => 当前实现每秒耗时约 %.0f ms\n",
           rows, 313, 313 * (a * 1e6 / (double)REP) / 1000.0);
    printf("            分块实现则约 %.0f ms\n", 313 * (b * 1e6 / (double)REP) / 1000.0);

    free(src); free(ref); free(dst);
    return 0;
}
