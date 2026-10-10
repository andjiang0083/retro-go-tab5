#pragma once

#include <stdbool.h>

/* 屏幕方向选择（S2）—— 设置项契约 + 引导槽切换（**标签后缀制**）。
 *
 * 用户要求（2026-10-09，逐字）："第一次使用，弹窗，让用户选择横还是竖，要说明设置哪里可以重选。
 * 默认是竖屏。"  完整设计见 docs/SPEC-S2-ORIENTATION-SWITCH.md（§0 用户要求、§3 分区与引导）。
 *
 * 两种"方向"必须分清（本模块最容易搞错的地方）：
 *   - **实际方向** = 当前**正在运行的分区标签**决定：带 `_l` 后缀 ⇒ 横屏，不带 ⇒ 竖屏。
 *     不依赖 NVS ⇒ 不可能出现"记录说横屏、实际跑竖屏"。UI 显示与标签拼装都用它。
 *   - **用户的选择** = 设置层的 `ui` 段 `orient` 键（Namespace `ui` / key `orient`）：
 *     经 `rg_settings_*` 读写；**本目标上它落盘在 SD 卡**（`/sd/retro-go/config/ui.json`，实机日志实证），
 *     不是 NVS —— 换句话说拔卡/换卡后这个选择会"忘记"（首次弹窗会再出现一次，属可接受行为）。
 *     值域 `-1` = 未选择 ⇒ 首次开机要弹窗。
 *     只在两处用：① 判断是否首次使用 ② 用户点切换后记下来。
 *
 * 分区方案（SPEC §3）：三件套 × 两方向 —— 竖屏用原名（`launcher`/`retro-core`/`gbsp`，零回归），
 * 横屏同名加 `_l`。本工程 app 是**按标签查找**的（`rg_system.c:1133`），所以横屏下凡是要起
 * 某个 app 的地方都必须过一遍 `rg_orient_app_label()`。 */

#define RG_ORIENT_UNSET     (-1)  /* NVS 无记录（或版本不符）= 从未选择过 ⇒ 首次开机要弹窗 */
#define RG_ORIENT_PORTRAIT   0
#define RG_ORIENT_LANDSCAPE  1

#define RG_ORIENT_SUFFIX     "_l"   /* 横屏分区标签后缀 */
#define RG_ORIENT_LABEL_MAX  17     /* esp_partition_t.label 是 char[17]（含结尾 NUL） */
#define RG_ORIENT_BASE_MAX   14     /* 基名上限：14 + "_l" + NUL = 17 ⇒ 加后缀后仍在 label[17] 内。
                                     * 定死这个上限是为了让编译器能证明 snprintf 不会截断
                                     * （-Werror=format-truncation，2026-10-09 实证）。 */

/* 当前**实际**方向（读运行分区标签，不读 NVS）。 */
int rg_orient_running(void);

/* ── 单 app 形态（0.4.9）的方向真值 ─────────────────────────────────────────────
 * 双 app 形态："实际方向"= 运行分区标签（上面那套）。
 * 单 app 形态：**没有第二个分区可切**，方向改成运行时属性，权威值放 NVS（`rgapp/orient`）。
 *   关键约束：它必须在**显示初始化之前**可读（显示要用它选后端与几何），
 *   而 SD 上的 `ui/orient` 那时还没挂载 ⇒ 所以不能拿 SD 当真值。
 *   于是分工：NVS = 权威；SD `ui/orient` = 设置界面的载体 + 老版本迁移源；
 *   `rg_orient_set()` 两个都写，不会分叉。惰性初始化 + 缓存，任何时机调用都安全。 */
int  rg_orient_active(void);             /* 当前生效方向（显示层/几何/driver 分发专用） */
void rg_orient_active_set(int value);    /* 写 NVS（单 app）+ 缓存；不切分区、不重启 */
void rg_orient_sync_from_settings(void); /* 单 app：把老版本的 SD 选择迁进 NVS（一次性，可能重启一次） */
bool rg_orient_is_single_app(void);      /* 本二进制是不是单 app 形态（供日志/UI 判断） */

/* 当前运行的 app 是不是这个方向。 */
bool rg_orient_running_is(int value);

/* 用户的选择（NVS，带缓存）：RG_ORIENT_UNSET / PORTRAIT / LANDSCAPE。 */
int rg_orient_get(void);

/* 写入用户的选择（不重启、不切槽）。 */
void rg_orient_set(int value);

/* 是否"从未选择过"（首次开机 ⇒ 出选择页）。 */
bool rg_orient_is_first_boot(void);

/* 文案："Portrait"/"Landscape"/"Unset"（运行期字符串，不进 _() 翻译宏）。 */
const char *rg_orient_name(int value);

/* 把 app **基名**按指定方向拼成完整分区标签。base 必须是基名（不能已带后缀）。
 * out 至少 RG_ORIENT_LABEL_MAX 字节。例：("retro-core", LANDSCAPE) -> "retro-core_l" */
void rg_orient_label_for(const char *base, int orient, char *out);

/* 把 app 基名按**当前实际方向**拼成完整标签 —— 凡是要起某个 app 的地方都用这个。 */
void rg_orient_app_label(const char *base, char *out);

/* 写 NVS + 切到目标方向的**同名 app** 引导槽 + 重启。
 * 已在目标方向时只记录选择、不重启（调用方可据此跳过重启确认）。
 * 任一步失败都不写 NVS（不留"记录了但没生效"的不一致状态）。成功时不会返回。 */
bool rg_orient_restart_into(int value);
