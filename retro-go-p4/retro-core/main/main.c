#include "shared.h"


void app_main(void)
{
    rg_app_t *app = rg_system_init(AUDIO_SAMPLE_RATE, NULL, NULL);

    RG_LOGI("configNs=%s", app->configNs);

    /* 横屏：按机型把"画面可用区"切对 —— 没肩键行的机型（GB/GBC/NES/GG/SMS/COL/PCE/GW/Lynx）
     * 上边界抬到 0，让 4x 画面吃满顶部；GBA/SNES/菜单 保持 {280,120,280,120}。
     * ⚠ 必须在核心第一帧之前调用（视口是那时算的）。竖屏下本调用恒等。 */
    rg_display_set_visible_area_for_console(app->configNs);

    if (strcmp(app->configNs, "gbc") == 0 || strcmp(app->configNs, "gb") == 0)
        gbc_main();
    else if (strcmp(app->configNs, "nes") == 0)
        nes_main();
    else if (strcmp(app->configNs, "pce") == 0)
        pce_main();
    else if (strcmp(app->configNs, "sms") == 0)
        sms_main();
    else if (strcmp(app->configNs, "gg") == 0)
        sms_main();
    else if (strcmp(app->configNs, "col") == 0)
        sms_main();
    else if (strcmp(app->configNs, "gw") == 0)
        gw_main();
    else if (strcmp(app->configNs, "snes") == 0)
        snes_main();
#ifndef __TINYC__
    else if (strcmp(app->configNs, "lnx") == 0)
        lynx_main();
#endif
    else
        launcher_main();

    RG_PANIC("Never reached");
}
