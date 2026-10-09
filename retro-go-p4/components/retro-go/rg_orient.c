/* 屏幕方向选择（S2）—— 数据层实现。设计见 docs/SPEC-S2-ORIENTATION-SWITCH.md、rg_orient.h 顶部说明。
 *
 * 为什么是"切引导槽"而不是"运行期切驱动"：两份显示驱动差 427 行（842 vs 975），合并=重写，会动到
 * 已认可的竖屏基线。改成"三件套 × 两方向"的 app 分区（竖屏原名 / 横屏加 `_l`）+ 按标签切引导槽，
 * 则两份驱动一行都不用改，切换发生在**引导之前**。
 *
 * 为什么用标签后缀而不是设置里的"当前方向"：本工程 app 本就是**按标签查找**的
 * （rg_system.c:1133 esp_partition_find_first(APP, ANY, label)），标签天然带方向信息；
 * 而设置层只存"用户的选择"（经 rg_settings_*；本目标落盘在 SD 的 config/ui.json），两者分开
 * ⇒ 不会出现"记录与事实不一致"。 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "rg_orient.h"
#include "rg_settings.h"
#include "rg_gui.h"      /* 重启过渡屏 rg_gui_draw_restart_notice() */
#include "rg_system.h"

#define ORIENT_SECTION  "ui"
#define ORIENT_KEY      "orient"
#define ORIENT_VER_KEY  "orient_ver"
#define ORIENT_VER      1

static int orient_cached = -2;   /* -2 = 还没读过（RG_ORIENT_UNSET 是 -1，不能拿它当"未读"哨兵） */

static bool orient_valid(int value)
{
    return value == RG_ORIENT_PORTRAIT || value == RG_ORIENT_LANDSCAPE;
}

/* 标签是否带横屏后缀 */
static bool label_is_landscape(const char *label)
{
    size_t len = label ? strlen(label) : 0;
    return len > strlen(RG_ORIENT_SUFFIX) && strcmp(label + len - strlen(RG_ORIENT_SUFFIX), RG_ORIENT_SUFFIX) == 0;
}

/* 去掉横屏后缀，得到基名（不改原串）。out 至少 RG_ORIENT_BASE_MAX+1 字节（上限写死见 rg_orient.h）。 */
static void label_to_base(const char *label, char *out)
{
    size_t len = label ? strlen(label) : 0;
    size_t suf = strlen(RG_ORIENT_SUFFIX);
    if (len > suf && strcmp(label + len - suf, RG_ORIENT_SUFFIX) == 0)
        len -= suf;
    if (len > RG_ORIENT_BASE_MAX)
        len = RG_ORIENT_BASE_MAX;
    memcpy(out, label, len);
    out[len] = 0;
}

int rg_orient_running(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running)
        return RG_ORIENT_UNSET;
    return label_is_landscape(running->label) ? RG_ORIENT_LANDSCAPE : RG_ORIENT_PORTRAIT;
}

bool rg_orient_running_is(int value)
{
    return orient_valid(value) && rg_orient_running() == value;
}

int rg_orient_get(void)
{
    if (orient_cached != -2)
        return orient_cached;

    /* 版本不符 = 丢弃旧值当"未选择"，避免将来语义变更时误读老数据 */
    int ver = (int)rg_settings_get_number(ORIENT_SECTION, ORIENT_VER_KEY, -1);
    int value = RG_ORIENT_UNSET;
    if (ver == ORIENT_VER)
    {
        value = (int)rg_settings_get_number(ORIENT_SECTION, ORIENT_KEY, RG_ORIENT_UNSET);
        if (!orient_valid(value))
            value = RG_ORIENT_UNSET;
    }
    return (orient_cached = value);
}

void rg_orient_set(int value)
{
    if (!orient_valid(value))
        return;
    rg_settings_set_number(ORIENT_SECTION, ORIENT_VER_KEY, ORIENT_VER);
    rg_settings_set_number(ORIENT_SECTION, ORIENT_KEY, value);
    rg_settings_commit();
    orient_cached = value;
    RG_LOGI("orient: saved choice %s\n", rg_orient_name(value));
}

bool rg_orient_is_first_boot(void)
{
    return rg_orient_get() == RG_ORIENT_UNSET;
}

const char *rg_orient_name(int value)
{
    switch (value)
    {
    case RG_ORIENT_PORTRAIT:  return "Portrait";
    case RG_ORIENT_LANDSCAPE: return "Landscape";
    default:                  return "Unset";
    }
}

void rg_orient_label_for(const char *base, int orient, char *out)
{
    /* 万一传进来的已经带后缀，先去干净（幂等，避免 `launcher_l_l`） */
    char clean[RG_ORIENT_BASE_MAX + 1];
    label_to_base(base, clean);
    snprintf(out, RG_ORIENT_LABEL_MAX, "%s%s", clean,
             orient == RG_ORIENT_LANDSCAPE ? RG_ORIENT_SUFFIX : "");
}

void rg_orient_app_label(const char *base, char *out)
{
    rg_orient_label_for(base, rg_orient_running(), out);
}

bool rg_orient_restart_into(int value)
{
    if (!orient_valid(value))
        return false;

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running)
        return false;

    /* 由"当前 app"推出"目标方向的同名 app"：launcher <-> launcher_l、gbsp <-> gbsp_l … */
    char base[RG_ORIENT_BASE_MAX + 1], target_label[RG_ORIENT_LABEL_MAX];
    label_to_base(running->label, base);
    rg_orient_label_for(base, value, target_label);

    if (strcmp(target_label, running->label) == 0)
    {
        /* 已经在目标方向的同名 app：只记录选择，不重启 */
        rg_orient_set(value);
        return true;
    }

    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, target_label);
    if (!target)
    {
        RG_LOGE("orient: slot '%s' not found —— 双方向分区表还没烧进设备？(见 SPEC §3)\n", target_label);
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK)
    {
        RG_LOGE("orient: set_boot_partition(%s) failed: 0x%x\n", target_label, err);
        return false;   /* 槽没切成 ⇒ 不写 NVS，避免"记录了但没生效"的不一致 */
    }

    rg_orient_set(value);   /* 槽切成功后才记录 */

    RG_LOGI("orient: restart into %s (slot %s)\n", rg_orient_name(value), target_label);
    /* 过渡屏：把"正在重启"这件事画出来（黑底+提示）。否则重启前最后一帧往往是主题底色
     * （深蓝）⇒ 用户以为崩溃了（实测无 panic；见 rg_gui_draw_restart_notice 注释）。 */
    rg_gui_draw_restart_notice("Restarting...");
    vTaskDelay(pdMS_TO_TICKS(450));   /* 让过渡屏可见约 0.45 秒，也把日志送出串口 */
    esp_restart();
    return true;            /* 正常路径不会到达 */
}
