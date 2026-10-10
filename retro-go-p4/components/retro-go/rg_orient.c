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

/* ── 单 app 形态（0.4.9）：方向真值改放 NVS ────────────────────────────────────
 * 为什么不用 rg_settings：显示初始化**早于**存储层 —— 那时 SD 还没挂载、ui.json 读不到，
 * 而显示必须用方向来选后端与几何。nvs_flash_init() 幂等（与 rg_system.c 的 RG_SINGLE_APP
 * 待续标志同一套做法），任何时机调用都安全。 */
#if defined(ESP_PLATFORM)
#include "nvs.h"
#endif
#define ORIENT_NVS_NS   "rgapp"
#define ORIENT_NVS_KEY  "orient"

static int orient_active_cache = -2;   /* -2 = 未读 */

bool rg_orient_is_single_app(void)
{
#if defined(RG_SINGLE_APP)
    return true;
#else
    return false;
#endif
}

static bool orient_nvs_read(int *out)
{
#if defined(RG_SINGLE_APP) && defined(ESP_PLATFORM)
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(ORIENT_NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    esp_err_t err = nvs_get_u8(h, ORIENT_NVS_KEY, &v);
    nvs_close(h);
    if (err != ESP_OK)
        return false;
    *out = (int)v;
    return true;
#else
    (void)out;
    return false;
#endif
}

static void orient_nvs_write(int value)
{
#if defined(RG_SINGLE_APP) && defined(ESP_PLATFORM)
    nvs_handle_t h;
    if (nvs_open(ORIENT_NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, ORIENT_NVS_KEY, (uint8_t)value);
    nvs_commit(h);
    nvs_close(h);
#else
    (void)value;
#endif
}

int rg_orient_active(void)
{
    if (orient_active_cache != -2)
        return orient_active_cache;
#if defined(RG_SINGLE_APP)
    /* 单 app：NVS 权威。没有记录 ⇒ 默认**竖屏**（用户 2026-10-09 定："默认是竖屏"；
     * 首次开机的弹窗会调 rg_orient_set() 把它落进 NVS）。 */
    int v;
    if (orient_nvs_read(&v) && orient_valid(v))
        return (orient_active_cache = v);
    return (orient_active_cache = RG_ORIENT_PORTRAIT);
#else
    /* 双 app：运行分区标签就是真值（零依赖、不可能与事实不一致） */
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running)
        return RG_ORIENT_UNSET;           /* 不缓存异常态 */
    return (orient_active_cache = label_is_landscape(running->label) ? RG_ORIENT_LANDSCAPE : RG_ORIENT_PORTRAIT);
#endif
}

void rg_orient_active_set(int value)
{
    if (!orient_valid(value))
        return;
    orient_active_cache = value;
    orient_nvs_write(value);
}

/* 单 app 一次性迁移：老版本把用户选择存在 SD（ui/orient）上，NVS 里还没有值 ⇒ 搬过来。
 * ⚠ 只能在**存储层起来之后**调用（rg_system_init 内），因为它要读 SD。 */
void rg_orient_sync_from_settings(void)
{
#if defined(RG_SINGLE_APP)
    int v;
    if (orient_nvs_read(&v) && orient_valid(v))
        return;                                  /* 已有权威值，什么都不做 */
    int sd = rg_orient_get();                    /* 读 SD 的 ui/orient（-1 = 未选过） */
    if (!orient_valid(sd))
    {
        orient_nvs_write(rg_orient_active());    /* 落一次默认值，以后不再进本分支 */
        return;
    }
    orient_nvs_write(sd);
    if (sd != rg_orient_active())
    {
        /* 迁来的选择与本次启动实际用的不一致 ⇒ 重启一次让显示跟上。
         * NVS 已写好 ⇒ 只可能发生这一次（不会重启循环）。 */
        RG_LOGI("orient: migrated choice %s from settings -> restart to apply\n", rg_orient_name(sd));
        rg_gui_draw_restart_notice("Applying display orientation...");
        vTaskDelay(pdMS_TO_TICKS(450));
        esp_restart();
    }
    RG_LOGI("orient: migrated choice %s from settings (already active)\n", rg_orient_name(sd));
#endif
}

int rg_orient_running(void)
{
    /* 语义统一：双 app = 运行分区标签；单 app = NVS 权威值（没有第二个分区可"运行在"）。 */
    return rg_orient_active();
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
#if defined(RG_SINGLE_APP)
    /* 单 app：NVS 才是显示层读的权威值 ⇒ 必须一起写，否则会出现"记录说横屏、显示还是竖屏"。
     * ⚠ 双 app 形态**不能**碰 orient_active_cache：那里的"实际方向"是运行分区标签，
     *   与"用户选择"是两个不同的东西（选择了横屏但还没重启时，实际仍跑竖屏）。 */
    orient_nvs_write(value);
    orient_active_cache = value;
#endif
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
#if defined(RG_SINGLE_APP)
    /* 单 app 形态（0.4.9）：**没有**第二个分区、也没有 `_l` 后缀分区 —— 两个方向共用同一个 app。
     * 这里必须原样返回基名：加 `_l` 是找不到分区的（调用点虽有回退，但会刷 "not present" 告警，
     * 也与"标签必须是真实存在的分区"这个前提相悖；见 rg_system.c 两处调用点的注释）。 */
    snprintf(out, RG_ORIENT_LABEL_MAX, "%s", base);
#else
    rg_orient_label_for(base, rg_orient_running(), out);
#endif
}

bool rg_orient_restart_into(int value)
{
    if (!orient_valid(value))
        return false;

#if defined(RG_SINGLE_APP)
    /* 单 app 形态（0.4.9）：**没有第二个分区可切** —— 把方向写进 NVS + 重启自己，
     * 分发器（launcher/main/main.c 的 app_main）会在启动时按新方向初始化显示后端。
     * 已经在目标方向时只记录、不重启（与双 app 形态返回值语义一致，调用方据此跳过重启确认）。 */
    if (rg_orient_active() == value)
    {
        rg_orient_set(value);
        return true;
    }
    rg_orient_active_set(value);   /* 先落权威值：重启后显示才会按新方向起 */
    rg_orient_set(value);          /* 同时写设置（设置页/首启弹窗读它） */
    RG_LOGI("orient: restart into %s (single app)\n", rg_orient_name(value));
    rg_gui_draw_restart_notice("Restarting...");
    vTaskDelay(pdMS_TO_TICKS(450));
    esp_restart();
    return true;                   /* 正常路径不会到达 */
#else
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
#endif  /* RG_SINGLE_APP */
}
