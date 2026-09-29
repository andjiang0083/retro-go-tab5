/* 内置 CJK 点阵字库的读取端（生成端见 tools/gen_cjk_font.py）。
 *
 * 字库**直接编进 app 镜像**（CMake 里 target_add_binary_data 把 assets/cjk12.bin 变成
 * _binary_cjk12_bin_start/_end 两个符号），不再单独占一个 flash 分区。
 * 为什么改：M5Launcher 这类启动器只装"一个 app 镜像"，并且会把不属于 sys/system 的
 * 数据分区按它自己的规则重建 —— 按名字找 flash 分区的老做法在那种安装方式下必然拿不到
 * 字体。编进镜像就跟安装方式无关了。
 * 代价：每个 app 各带一份（Tab5 只有 launcher + gbsp 两个 app，共 103KB×2；少了 cjkfont
 * 分区，总镜像大小基本不变）。数据在 flash 里，直接按指针读，没有运行期拷贝。
 */
#include "rg_system.h"
#include "rg_cjk.h"

#if defined(ESP_PLATFORM)

#include <string.h>

/* 由 CMake 的 target_add_binary_data(assets/cjk12.bin) 生成 */
extern const uint8_t cjk12_bin_start[] asm("_binary_cjk12_bin_start");
extern const uint8_t cjk12_bin_end[] asm("_binary_cjk12_bin_end");

static const uint8_t *cjk_index = NULL;   /* 升序码位数组 */
static const uint8_t *cjk_glyphs = NULL;  /* 字形数据起点 */
static uint32_t cjk_count = 0;
static uint16_t cjk_cell_w = 0, cjk_cell_h = 0;
static uint16_t cjk_row_bytes = 0;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool rg_cjk_init(void)
{
    if (cjk_glyphs)
        return true;

    const uint8_t *b = cjk12_bin_start;
    const size_t blob_size = (size_t)(cjk12_bin_end - cjk12_bin_start);
    if (!b || blob_size < 24)
    {
        RG_LOGW("cjk: embedded font missing (blob %u bytes)\n", (unsigned)blob_size);
        return false;
    }
    if (memcmp(b, "RGF1", 4) != 0)
    {
        RG_LOGE("cjk: bad magic (expected RGF1)\n");
        return false;
    }
    cjk_cell_w = rd16(b + 4);
    cjk_cell_h = rd16(b + 6);
    cjk_count = rd32(b + 8);
    uint32_t index_off = rd32(b + 12);
    uint32_t glyph_off = rd32(b + 16);
    if (!cjk_cell_w || !cjk_cell_h || !cjk_count ||
        index_off + cjk_count * 4 > blob_size || glyph_off > blob_size)
    {
        RG_LOGE("cjk: header out of range (count=%u idx=%u glyph=%u size=%u)\n",
                (unsigned)cjk_count, (unsigned)index_off, (unsigned)glyph_off, (unsigned)blob_size);
        return false;
    }
    /* 每行按整字节存放，向上取整（12 位 → 2 字节） */
    cjk_row_bytes = (uint16_t)((cjk_cell_w + 7) / 8);
    if (glyph_off + (uint32_t)cjk_count * cjk_row_bytes * cjk_cell_h > blob_size)
    {
        RG_LOGE("cjk: glyph data out of range\n");
        return false;
    }

    cjk_index = b + index_off;
    cjk_glyphs = b + glyph_off;
    RG_LOGI("cjk: font ready, %u glyphs, %ux%u cells, %u KB embedded\n",
            (unsigned)cjk_count, cjk_cell_w, cjk_cell_h, (unsigned)(blob_size / 1024));
    return true;
}

bool rg_cjk_ready(void)
{
    return cjk_glyphs != NULL;
}

/* 升序索引二分查找：12 步以内，比建哈希表省内存也够快（每帧几百次查找） */
static const uint8_t *cjk_find(uint32_t c)
{
    uint32_t lo = 0, hi = cjk_count;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t v = rd32(cjk_index + mid * 4);
        if (v == c)
            return cjk_glyphs + (size_t)mid * cjk_row_bytes * cjk_cell_h;
        if (v < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

size_t rg_cjk_glyph(uint32_t *output, int points, int font_height, uint32_t c)
{
    if (!cjk_glyphs || c < 0x80 || points <= 0)
        return 0;

    const uint8_t *g = cjk_find(c);
    if (!g)
        return 0;

    if (!output)
        return font_height;   /* 一个汉字占一个 em，交给调用方按 points/font_height 缩放 */

    /* 把 cell 缩放到 points 行：横纵用同一比例（保持正方形，汉字被拉扁/拉高都很难看）。
     * 纵向比例 points/cell_h 可能不是整数（12 → 39 是 3.25），所以按源行索引取，允许重复行。 */
    memset(output, 0, (size_t)points * 4);
    for (int y = 0; y < points; ++y)
    {
        int sy = y * cjk_cell_h / points;
        if (sy >= cjk_cell_h)
            sy = cjk_cell_h - 1;
        const uint8_t *row = g + (size_t)sy * cjk_row_bytes;
        /* 位图约定必须和内置字体一致（见 rg_gui.c 绘制循环）：
         *   * 每行一个 uint32，**bit 0 = 最左像素**（绘制端是 (row >> sx) & 1）；
         *   * 行内只放**原生宽度**（cell_w 列），横向缩放由调用方按 points/font_height 做。
         * 踩过的坑：最初写成 MSB 对齐 + 铺满 points 列 → ①左右镜像 ②39 列塞进 32 位溢出错位，
         * 屏幕上表现为"汉字被横向拉伸"。 */
        /* 源：文件里每行 row_bytes 字节、**MSB 在左**（12 位放在 16 位值的高位），
         *     先按大端拼成整数 v，第 x 列 = v 的 bit (row_bytes*8-1-x)。
         * 输出：**bit x = 第 x 列（左起）** —— 与内置字体解码完全一致
         *     （rg_gui.c: row |= (1 << (xOffset + x))），绘制端 (row >> sx) & 1。
         * 踩过的坑：①输出写成 MSB 对齐 → 汉字左右镜像；②直接铺满 points 列 →
         * 39 列塞进 32 位溢出错位。两处都会让汉字看起来"被拉伸/变形"。 */
        uint32_t v = 0;
        for (int i = 0; i < cjk_row_bytes; ++i)
            v = (v << 8) | row[i];
        uint32_t out_row = 0;
        for (int x = 0; x < cjk_cell_w; ++x)
            if ((v >> (cjk_row_bytes * 8 - 1 - x)) & 1)
                out_row |= (uint32_t)1 << x;
        output[y] = out_row;
    }
    return font_height;
}

#else /* 宿主（SDL2 预览）：没有 flash 分区，直接从本地文件读，保证预览能验证汉字渲染 */

#include <stdio.h>
#include <stdlib.h>

static uint8_t *host_blob = NULL;
static const uint8_t *cjk_index = NULL, *cjk_glyphs = NULL;
static uint32_t cjk_count = 0;
static uint16_t cjk_cell_w = 0, cjk_cell_h = 0, cjk_row_bytes = 0;

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

bool rg_cjk_init(void)
{
    if (cjk_glyphs)
        return true;
    const char *paths[] = {"retro-go-p4/assets/cjk12.bin", "assets/cjk12.bin", "../assets/cjk12.bin"};
    for (size_t i = 0; i < RG_COUNT(paths) && !cjk_glyphs; ++i)
    {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (len <= 20 || !(host_blob = malloc(len)))
        {
            fclose(f);
            continue;
        }
        if (fread(host_blob, 1, len, f) != (size_t)len)
        {
            free(host_blob);
            host_blob = NULL;
            fclose(f);
            continue;
        }
        fclose(f);
        if (memcmp(host_blob, "RGF1", 4) != 0)
        {
            free(host_blob);
            host_blob = NULL;
            continue;
        }
        cjk_cell_w = rd16(host_blob + 4);
        cjk_cell_h = rd16(host_blob + 6);
        cjk_count = rd32(host_blob + 8);
        cjk_index = host_blob + rd32(host_blob + 12);
        cjk_glyphs = host_blob + rd32(host_blob + 16);
        cjk_row_bytes = (uint16_t)((cjk_cell_w + 7) / 8);
        printf("[info] cjk: host font loaded from %s (%u glyphs, %ux%u)\n", paths[i],
               (unsigned)cjk_count, cjk_cell_w, cjk_cell_h);
    }
    if (!cjk_glyphs)
        printf("[warn] cjk: no font file found, CJK text will show as boxes\n");
    return cjk_glyphs != NULL;
}

bool rg_cjk_ready(void)
{
    return cjk_glyphs != NULL;
}

static const uint8_t *cjk_find(uint32_t c)
{
    uint32_t lo = 0, hi = cjk_count;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t v = rd32(cjk_index + mid * 4);
        if (v == c)
            return cjk_glyphs + (size_t)mid * cjk_row_bytes * cjk_cell_h;
        if (v < c)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

size_t rg_cjk_glyph(uint32_t *output, int points, int font_height, uint32_t c)
{
    if (!cjk_glyphs || c < 0x80 || points <= 0)
        return 0;
    const uint8_t *g = cjk_find(c);
    if (!g)
        return 0;
    if (!output)
        return font_height;
    memset(output, 0, (size_t)points * 4);
    for (int y = 0; y < points; ++y)
    {
        int sy = y * cjk_cell_h / points;
        if (sy >= cjk_cell_h) sy = cjk_cell_h - 1;
        const uint8_t *row = g + (size_t)sy * cjk_row_bytes;
        /* 位图约定必须和内置字体一致（见 rg_gui.c 绘制循环）：
         *   * 每行一个 uint32，**bit 0 = 最左像素**（绘制端是 (row >> sx) & 1）；
         *   * 行内只放**原生宽度**（cell_w 列），横向缩放由调用方按 points/font_height 做。
         * 踩过的坑：最初写成 MSB 对齐 + 铺满 points 列 → ①左右镜像 ②39 列塞进 32 位溢出错位，
         * 屏幕上表现为"汉字被横向拉伸"。 */
        /* 源：文件里每行 row_bytes 字节、**MSB 在左**（12 位放在 16 位值的高位），
         *     先按大端拼成整数 v，第 x 列 = v 的 bit (row_bytes*8-1-x)。
         * 输出：**bit x = 第 x 列（左起）** —— 与内置字体解码完全一致
         *     （rg_gui.c: row |= (1 << (xOffset + x))），绘制端 (row >> sx) & 1。
         * 踩过的坑：①输出写成 MSB 对齐 → 汉字左右镜像；②直接铺满 points 列 →
         * 39 列塞进 32 位溢出错位。两处都会让汉字看起来"被拉伸/变形"。 */
        uint32_t v = 0;
        for (int i = 0; i < cjk_row_bytes; ++i)
            v = (v << 8) | row[i];
        uint32_t out_row = 0;
        for (int x = 0; x < cjk_cell_w; ++x)
            if ((v >> (cjk_row_bytes * 8 - 1 - x)) & 1)
                out_row |= (uint32_t)1 << x;
        output[y] = out_row;
    }
    return font_height;
}

#endif
