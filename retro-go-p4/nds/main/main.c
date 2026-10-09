/* nds app —— NDS core 入口。
 * 模式沿用 gbsp/main.c：rg_system_init → surface 双缓冲轮换 → rg_display_submit / rg_system_tick。
 * M0：占位棋盘格（验证显示通路）。M1：接 nds_core_step()，双屏上下拼接（256×384）。
 * ⚠ house rules：一次 flash 一个可见变更 —— 不改动 gbsp/launcher。
 */
#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nds_core.h"   /* 核心是 IDF 组件（EXTRA_COMPONENT_DIRS 指向 tab5-nds/core），按组件头名引入 */

#define NDS_AUDIO_RATE 32000 /* ES8388 I2S，与 gbsp 同管线（M4） */

/* 双屏上下拼接：上屏 0..191，下屏 192..383（NDS 物理布局） */
#define NDS_DUAL_H (NDS_SCREEN_H * 2)
#define NDS_FB_BYTES (NDS_SCREEN_W * NDS_SCREEN_H * 2)

static rg_app_t *app;
static rg_surface_t *updates[2]; /* 双缓冲轮换（gbsp 修"横条闪烁"的同款做法） */
static rg_surface_t *currentUpdate;
static nds_core_cfg_t nds_cfg; /* 生命周期内常驻；fb 指针每帧指向后台缓冲 */

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    (void)filename; /* M5：nds_core savestate */
    return false;
}

static bool load_state_handler(const char *filename)
{
    (void)filename; /* M5 */
    return false;
}

static bool reset_handler(bool hard)
{
    nds_core_reset(hard);
    /* hard reset 会清 VRAM（nds_bus_reset）→ 自检图案没了，必须重画，否则复位后黑屏 */
    nds_core_selftest();
    return true;
}

static void event_handler(int event, void *arg)
{
    (void)arg;
    if (event == RG_EVENT_REDRAW)
        rg_display_submit(currentUpdate, 0);
}

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    /* M3：rg_input_read_gamepad() → NDS_KEY_* 位图 + 下屏触摸映射（nds_core_set_input）。 */
    (void)port; (void)device; (void)index; (void)id;
    return 0;
}

/* rg_system 要求的网络桩（本移植 --no-networking，见 gbsp） */
void netpacket_poll_receive(void) {}
void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
    (void)client_id; (void)buf; (void)len;
}

/* 把 cfg 的 fb 指针指向指定表面的双屏区域（上=+0，下=+NDS_FB_BYTES） */
static void nds_bind_surface(rg_surface_t *s)
{
    nds_cfg.fb_top = (uint8_t *)s->data;
    nds_cfg.fb_bottom = (uint8_t *)s->data + NDS_FB_BYTES;
}

void app_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
    };

    app = rg_system_init(NDS_AUDIO_RATE, &handlers, NULL);

    /* 双屏：256×384（+1 行余量，沿用 gbsp 的 surface 分配习惯） */
    updates[0] = rg_surface_create(NDS_SCREEN_W, NDS_DUAL_H + 1, RG_PIXEL_565_LE, MEM_FAST);
    if (!updates[0]) RG_PANIC("nds: surface alloc failed");
    updates[0]->height = NDS_DUAL_H;
    updates[1] = rg_surface_create(NDS_SCREEN_W, NDS_DUAL_H + 1, RG_PIXEL_565_LE, MEM_FAST);
    if (updates[1])
        updates[1]->height = NDS_DUAL_H; /* 建不出就退回单缓冲（gbsp 同款降级） */
    currentUpdate = updates[0];

    /* 核心接线。ROM：M5 起经 rg_storage_read_file 载入并交 nds_core（当前不消费）。 */
    memset(&nds_cfg, 0, sizeof(nds_cfg));
    nds_cfg.rom = NULL;
    nds_cfg.rom_size = 0;
    nds_bind_surface(currentUpdate);
    if (!nds_core_init(&nds_cfg))
        RG_PANIC("nds: core init failed");

    /* M1 自检（上机诊断）：
     *  a) CPU 自检：跑内嵌 Thumb 程序经真实 bus 写主屏 fb[0] —— 在目标芯片（RISC-V）上
     *     验证 arm9_step + bus 译码/字节序/非对齐访问（host x86 通过不代表目标通过）。
     *  b) 显示自检：双屏写彩条/棋盘格 —— 验证双屏布局 + 面板字节序 + 缩放。 */
    RG_LOGI("nds: rom=%s", app && app->romPath ? app->romPath : "(none)");
    RG_LOGI("nds: cpu selftest %s", nds_core_selftest_cpu() ? "PASS" : "FAIL");
    nds_core_selftest();

    /* M1：无卡带代码可跑，CPU 不占帧预算（M2 scheduler 接管；届时由事件驱动 arm9_step） */
    nds_core_set_tick_budget(0);

    RG_LOGI("NDS core M1 (dual screen %dx%d)", NDS_SCREEN_W, NDS_DUAL_H);

    while (1)
    {
        int64_t start_time = rg_system_timer();

        nds_bind_surface(currentUpdate); /* 指向后台缓冲（轮换后可能变化） */
        nds_core_step();                 /* vblank + 双屏 VRAM→fb 拷贝 */

        rg_display_submit(currentUpdate, 0);
        if (updates[1])
            currentUpdate = updates[currentUpdate == updates[0]];

        rg_system_tick(rg_system_timer() - start_time);
        /* M4: 混音缓冲 → rg_audio_submit(mixbuffer, n)（Core-1，ES8388） */
    }

    RG_PANIC("NDS core ended"); /* 不可达（gbsp 同款） */
}
