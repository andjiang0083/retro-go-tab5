#include "shared.h"

#include <snes9x.h>
#include <math.h>

/* ── [AUDIO-SYNC-FIX] 音频节流按 60fps 算，不是 50fps ────────────────────────
 * shared.h 里是 AUDIO_SAMPLE_RATE / 50 + 1 = 641 样本 = **20.0ms 的音频**。
 * audio_task 每帧固定提交这么多，而 I2S 按 32000Hz 硬件节奏消费 641 个要 20ms，
 * 于是帧时间被节流钉死：max(核心耗时, 20ms) → **帧率天花板 50fps**。
 *
 * 2026-10-07 真机分层计时实测：
 *   Off 档   T19.9 = C11.0(核心) + A8.9(音频提交) + D0.0 + S0.0  → 50fps
 *   关音频   T11.0 = C10.0(核心)                                 → 90fps
 * 那 8.9ms 不是浪费，是模拟器在等音频硬件；病根是它按 50fps 的量提交。
 *
 * SNES 是 60Hz 机器：按 60fps 提交（534 样本 = 16.7ms），帧率天花板变 60fps，
 * 而核心只要 11ms，还剩 5.6ms 余量 —— 正好落在 SNES 原生满帧上。
 * 调分母不是"提速"，是让节流点对齐机器本身的刷新率。
 *
 * ⚠ 只覆盖 SNES：shared.h 那个 /50 是所有核心共用的（PCE 跑 50Hz 有它的道理），
 *   不要直接改 shared.h。 */
#undef  AUDIO_BUFFER_LENGTH
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)

/* ══════════════════════════════════════════════════════════════════════════════
 * [PERF-PROBE 2026-10-07] SNES 性能测量探针 —— **测量专用，发行前必须撤掉**
 *
 * 目的：把一帧的时间拆成"CPU 模拟 / PPU 渲染 / APU(SPC700+DSP)"三块，
 *       回答"dynarec 值多少分"这个问题。
 *
 * 原理：**关掉一块，看核心每帧耗时掉多少**。不做逐指令插桩（那种计时代码本身
 *       的开销会污染读数），也不走串口 —— 读数直接画在屏幕上。
 *
 * 读数：屏幕顶部两行（视口内，靠脏区刷新）
 *       第一行 = 显示帧率（真正发送出去的帧/秒）
 *       第二行 = "C xx.x ms" = S9xMainLoop() 的每帧平均耗时（60 帧滑窗）
 *       ⚠ **主指标是第二行**。frameskip 已钉死为 0（rg_system.c 的 RG_PERF_PIN_FRAMESKIP），
 *         每帧都渲染，所以 C 值 = "完整一帧的全部工作量"，与跳帧策略无关。
 *
 * 操作：游戏内 MENU → Options → "Perf probe"，循环切换四档：
 *         Off → -PPU → -APU → -PPU/APU
 *       每档停 5~10 秒读一次 C 值。四个读数即可算出三块占比：
 *         PPU 占比 = (C_off − C_noPPU) / C_off
 *         APU 占比 = (C_off − C_noAPU) / C_off
 *         CPU 占比 ≈ 剩下的（再扣掉显示 ~1ms，有驱动 PERF 日志可查）
 *
 * ⚠ 测量期正常现象：跳过 PPU 后画面会花（HDMA 依赖渲染时序）、关 APU 后没声音、
 *   游戏逻辑会跑歪 —— 都不影响 C 值的可信度（CPU 周期推进不受影响）。
 *
 * 撤销清单（发行前逐条还原）：
 *   1. 本文件：perf_probe / PERF_PROBE_NAMES / perf_probe_apply / perf_probe_cb /
 *      options_handler 里那一项 / 主循环里的 perf_probe_tick / 顶部 include <apu.h>
 *   2. rg_touch_overlay.h：RG_OVERLAY_SHOW_FPS → 0（并撤 rg_overlay_set_probe_line）
 *   3. rg_system.c：RG_PERF_PIN_FRAMESKIP → 0；invalidate_lines 行号按新位置还原
 *   4. cpuexec.c：snes_probe_skip_ppu / snes_probe_skip_apu 两个定义 + RenderLine 那处 if
 *   5. snes9x/src/spc700.h：APU_EXECUTE 宏里的 && !snes_probe_skip_apu 和那行 extern
 * ══════════════════════════════════════════════════════════════════════════════ */

/* 两个探针开关都定义在 snes9x/src/cpuexec.c —— 不走头文件，直接 extern，
 * 免得动 snes9x 组件的 include 路径。 */
extern int snes_probe_skip_ppu;
extern int snes_probe_skip_apu;
/* 叠加层读数与置脏（实现在 rg_touch_overlay.c / rg_display.c，这里直接声明避免动包含顺序） */
extern void rg_overlay_set_probe_line(int line, const char *text);
extern void rg_display_invalidate_lines(int top, int count);

/* [PERF-PROBE] core1 上两个任务的实测耗时（微秒）—— 回答"core1 到底被谁吃掉"。
 * ⚠ 主循环那侧的 D 段恒为 0（提交是异步的），真实工作量必须从任务内部量。 */
extern volatile int32_t rg_display_task_us;        /* 由 rg_display.c 的 display_task 写 */
static volatile int32_t snes_audio_task_us = 0;    /* 由本文件 audio_task 写 */
extern void rg_display_set_partial_updates(bool on);

// #define FRAME_DOUBLE_BUFFERING
// #define AUDIO_DOUBLE_BUFFERING
#define USE_AUDIO_TASK

typedef struct
{
	char name[16];
	struct {
		uint16_t snes9x_mask;
		uint16_t local_mask;
		uint16_t mod_mask;
	} keys[16];
} keymap_t;

static const keymap_t KEYMAPS[] = {
	/* "Full" —— 触摸屏（Tab5）专用：**每个 SNES 键都用它自己的键**。
	 * 上游那三套（Type A/B/C）是给"物理按键不够"的小设备做的重排：X/Y 借给 START/SELECT、
	 * L/R 变成 B/A+菜单 的组合键、Type C 干脆把 X/Y/L/R 置 0 禁用。
	 * 我们的触摸屏有 START/SELECT，也有独立的 L/R 行，所以直接用真键（用户 2026-10-07 定）。
	 * ⚠ 必须是**第一套**：keymap_id 默认 0，否则新用户默认仍落在重排版上。 */
	{"Full", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_X, 0},
		{SNES_Y_MASK, RG_KEY_Y, 0},
		{SNES_TL_MASK, RG_KEY_L, 0},
		{SNES_TR_MASK, RG_KEY_R, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	{"Type A", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, RG_KEY_START, 0},
		{SNES_Y_MASK, RG_KEY_SELECT, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	{"Type B", {
		{SNES_A_MASK, RG_KEY_START, 0},
		{SNES_B_MASK, RG_KEY_A, 0},
		{SNES_X_MASK, RG_KEY_SELECT, 0},
		{SNES_Y_MASK, RG_KEY_B, 0},
		{SNES_TL_MASK, RG_KEY_B, RG_KEY_MENU},
		{SNES_TR_MASK, RG_KEY_A, RG_KEY_MENU},
		{SNES_START_MASK, RG_KEY_START, RG_KEY_MENU},
		{SNES_SELECT_MASK, RG_KEY_SELECT, RG_KEY_MENU},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
	{"Type C", {
		{SNES_A_MASK, RG_KEY_A, 0},
		{SNES_B_MASK, RG_KEY_B, 0},
		{SNES_X_MASK, 0, 0},
		{SNES_Y_MASK, 0, 0},
		{SNES_TL_MASK, 0, 0},
		{SNES_TR_MASK, 0, 0},
		{SNES_START_MASK, RG_KEY_START, 0},
		{SNES_SELECT_MASK, RG_KEY_SELECT, 0},
		{SNES_UP_MASK, RG_KEY_UP, 0},
		{SNES_DOWN_MASK, RG_KEY_DOWN, 0},
		{SNES_LEFT_MASK, RG_KEY_LEFT, 0},
		{SNES_RIGHT_MASK, RG_KEY_RIGHT, 0},
	}},
};

static const size_t KEYMAPS_COUNT = (sizeof(KEYMAPS) / sizeof(keymap_t));

static const char *SNES_BUTTONS[] = {
	"None", "None", "None", "None", "R", "L", "X", "A", "Right", "Left", "Down", "Up", "Start", "Select", "Y", "B"
};

#define AUDIO_LOW_PASS_RANGE ((60 * 65536) / 100)

static rg_app_t *app;
static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_audio_sample_t *audioBuffers[2];
static rg_audio_sample_t *currentAudioBuffer;

#ifdef USE_AUDIO_TASK
static rg_task_t *audio_task_handle;
#endif

static bool apu_enabled = true;
static bool lowpass_filter = false;

/* [PERF-PROBE] 探针档位：位 0 = 跳过 PPU 渲染，位 1 = 暂停 APU 模拟。见文件顶部说明。 */
static int perf_probe = 0;
static const char *PERF_PROBE_NAMES[] = {"Off", "-PPU", "-APU", "-PPU/APU"};

static void perf_probe_apply(void)
{
    snes_probe_skip_ppu = (perf_probe & 1) ? 1 : 0;
    snes_probe_skip_apu = (perf_probe & 2) ? 1 : 0;
}

/* [PERF-PROBE] 分层计时：把一帧切成四段，60 帧滑窗后组装三行文本推到屏幕。
 *
 * 动机：首轮测量（真机）发现帧时间 40ms 里只有 11ms 落在 S9xMainLoop() 内 ——
 * 剩下约 28ms 在核心之外，而且四档探针下几乎不变（29.0 / 28.1 / 29.1 / 27.2），
 * 比整个模拟器都大。这一轮专门定位它在哪一段。 */
static int64_t perf_acc_core = 0, perf_acc_audio = 0, perf_acc_disp = 0, perf_acc_sys = 0;
static int perf_acc_n = 0;

static void perf_probe_fmt(char *out, size_t n, int64_t us)
{
    int ms10 = (int)(us / 100);          /* 微秒 → 毫秒×10（整数，避免浮点格式化） */
    if (ms10 < 0)
        ms10 = 0;
    if (ms10 > 99999)
        ms10 = 99999;
    snprintf(out, n, "%d.%d", ms10 / 10, ms10 % 10);
}

static void perf_probe_tick(int64_t core_us, int64_t audio_us, int64_t disp_us, int64_t sys_us)
{
    perf_acc_core += core_us;
    perf_acc_audio += audio_us;
    perf_acc_disp += disp_us;
    perf_acc_sys += sys_us;
    if (++perf_acc_n < 60)
        return;

    const int64_t n = perf_acc_n;
    const int64_t avg_core = perf_acc_core / n;
    const int64_t avg_audio = perf_acc_audio / n;
    const int64_t avg_disp = perf_acc_disp / n;
    const int64_t avg_sys = perf_acc_sys / n;
    const int64_t total_us = avg_core + avg_audio + avg_disp + avg_sys;
    char s_core[8], s_audio[8], s_disp[8], s_sys[8], s_total[8];
    perf_probe_fmt(s_core, sizeof(s_core), avg_core);
    perf_probe_fmt(s_audio, sizeof(s_audio), avg_audio);
    perf_probe_fmt(s_disp, sizeof(s_disp), avg_disp);
    perf_probe_fmt(s_sys, sizeof(s_sys), avg_sys);
    perf_probe_fmt(s_total, sizeof(s_total), total_us);

    int fps = (total_us > 0) ? (int)(1000000 / total_us) : 0;
    if (fps > 99)
        fps = 99;

    /* [PERF-PROBE] 同一批数字同时打一行日志。
     * 动机（用户提出）：屏幕上的读数是"瞬时值 + 每 4 秒才刷新一次 + 还在跳"，
     * 肉眼读容易抓错；串口能连续抓几百行，PC 侧算均值/最大/抖动才靠得住。
     * ⚠ 全部整数微秒：esp_log 不支持 %f / %lld，用了会让后续参数错位
     *   （会打出 "ld us" 之类乱码，见 ~/esp32/ESP32-经验沉淀.md）。 */
    RG_LOGI("PERF-PROBE fps=%d total_us=%d core_us=%d audio_us=%d disp_us=%d sys_us=%d dtask_us=%d atask_us=%d",
            fps, (int)total_us, (int)avg_core, (int)avg_audio, (int)avg_disp, (int)avg_sys,
            (int)rg_display_task_us, (int)snes_audio_task_us);

    char s_dtask[8], s_atask[8];
    perf_probe_fmt(s_dtask, sizeof(s_dtask), rg_display_task_us);
    perf_probe_fmt(s_atask, sizeof(s_atask), snes_audio_task_us);

    char l0[24], l1[24], l2[24], l3[24];
    snprintf(l0, sizeof(l0), "F%2d T%s", fps, s_total);   /* 帧率 / 帧总时间 */
    snprintf(l1, sizeof(l1), "C%s D%s", s_core, s_disp);  /* 核心 / 显示段(提交) */
    snprintf(l2, sizeof(l2), "A%s S%s", s_audio, s_sys);  /* 音频提交 / 系统段 */
    snprintf(l3, sizeof(l3), "G%s X%s", s_dtask, s_atask); /* core1 实测：显示任务 / 音频任务 */

    perf_acc_core = perf_acc_audio = perf_acc_disp = perf_acc_sys = 0;
    perf_acc_n = 0;

    rg_overlay_set_probe_line(0, l0);
    rg_overlay_set_probe_line(1, l1);
    rg_overlay_set_probe_line(2, l2);
    rg_overlay_set_probe_line(3, l3);
    /* 四行共 128px，落在视口内 y 0..128；必须置脏才会重画（叠加层是"块被推送时才合成"的） */
    rg_display_invalidate_lines(0, 144);
}

static int keymap_id = 0;
static keymap_t keymap;

static const char *SETTING_KEYMAP = "keymap";
static const char *SETTING_APU_EMULATION = "apu";
static const char *SETTING_APU_FILTER = "filter";
// --- MAIN

static void update_keymap(int id)
{
    keymap_id = id % KEYMAPS_COUNT;
    keymap = KEYMAPS[keymap_id];
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    return S9xSaveState(filename);
}

static bool load_state_handler(const char *filename)
{
    return S9xLoadState(filename);
}

static bool reset_handler(bool hard)
{
    S9xReset();
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

static rg_gui_event_t apu_toggle_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        apu_enabled = !apu_enabled;
        rg_settings_set_number(NS_APP, SETTING_APU_EMULATION, apu_enabled);
    }

    strcpy(option->value, apu_enabled ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

static rg_gui_event_t lowpass_filter_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        lowpass_filter = !lowpass_filter;
        rg_settings_set_number(NS_APP, SETTING_APU_FILTER, lowpass_filter);
    }

    strcpy(option->value, lowpass_filter ? _("On") : _("Off"));

    return RG_DIALOG_VOID;
}

/* [PERF-PROBE] 性能测量档位（测量专用，见文件顶部说明）。 */
static rg_gui_event_t perf_probe_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        perf_probe = (perf_probe + 1) & 3;
        perf_probe_apply();
        /* 换成"关掉某一块"后耗时会立刻变，把滑窗清零立刻重新收敛，别让旧值拖尾 */
        perf_acc_core = perf_acc_audio = perf_acc_disp = perf_acc_sys = 0;
        perf_acc_n = 0;
    }

    strcpy(option->value, PERF_PROBE_NAMES[perf_probe]);

    return RG_DIALOG_VOID;
}

static rg_gui_event_t change_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_PREV || event == RG_DIALOG_NEXT)
    {
        if (event == RG_DIALOG_PREV && --keymap_id < 0)
            keymap_id = KEYMAPS_COUNT - 1;
        if (event == RG_DIALOG_NEXT && ++keymap_id > KEYMAPS_COUNT - 1)
            keymap_id = 0;
        update_keymap(keymap_id);
        rg_settings_set_number(NS_APP, SETTING_KEYMAP, keymap_id);
        return RG_DIALOG_REDRAW;
    }

    if (event == RG_DIALOG_ENTER)
    {
        return RG_DIALOG_CANCEL;
    }

    if (option->arg == -1)
    {
        strcat(strcat(strcpy(option->value, "< "), keymap.name), " >");
    }
    else if (option->arg >= 0)
    {
        int local_button = keymap.keys[option->arg].local_mask;
        int mod_button = keymap.keys[option->arg].mod_mask;
        int snes9x_button = log2(keymap.keys[option->arg].snes9x_mask); // convert bitmask to bit number

        if (snes9x_button < 4 || (local_button & (RG_KEY_UP|RG_KEY_DOWN|RG_KEY_LEFT|RG_KEY_RIGHT)))
        {
            option->flags = RG_DIALOG_FLAG_HIDDEN;
            return RG_DIALOG_VOID;
        }

        if (keymap.keys[option->arg].mod_mask)
            sprintf(option->value, "%s + %s", rg_input_get_key_name(mod_button), rg_input_get_key_name(local_button));
        else
            sprintf(option->value, "%s", rg_input_get_key_name(local_button));

        option->label = SNES_BUTTONS[snes9x_button];
        option->flags = RG_DIALOG_FLAG_NORMAL;
    }

    return RG_DIALOG_VOID;
}

static rg_gui_event_t menu_keymap_cb(rg_gui_option_t *option, rg_gui_event_t event)
{
    if (event == RG_DIALOG_ENTER)
    {
        const rg_gui_option_t options[] = {
            {-1, _("Profile"), "-", RG_DIALOG_FLAG_NORMAL, &change_keymap_cb},
            {-2, "", NULL, RG_DIALOG_FLAG_MESSAGE, NULL},
            {-3, "snes9x  ", "handheld", RG_DIALOG_FLAG_MESSAGE, NULL},
            {0, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {1, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {2, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {3, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {4, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {5, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {6, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {7, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {8, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {9, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {10, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {11, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {12, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {13, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {14, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            {15, "-", "-", RG_DIALOG_FLAG_HIDDEN, &change_keymap_cb},
            RG_DIALOG_END,
        };
        rg_gui_dialog(option->label, options, 0);
        return RG_DIALOG_REDRAW;
    }

    strcpy(option->value, keymap.name);
    return RG_DIALOG_VOID;
}

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_WIDTH * 2;
    GFX.ZPitch = SNES_WIDTH;
    GFX.Screen = currentUpdate->data;
    GFX.SubScreen = malloc(GFX.Pitch * SNES_HEIGHT_EXTENDED);
    GFX.ZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    GFX.SubZBuffer = malloc(GFX.ZPitch * SNES_HEIGHT_EXTENDED);
    return GFX.Screen && GFX.SubScreen && GFX.ZBuffer && GFX.SubZBuffer;
}

void S9xDeinitDisplay(void)
{
}

uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0)
        return 0;

    uint32_t joystick = rg_input_read_gamepad();
    uint32_t joypad = 0;

    for (int i = 0; i < RG_COUNT(keymap.keys); ++i)
    {
        uint32_t bitmask = keymap.keys[i].local_mask | keymap.keys[i].mod_mask;
        if (bitmask && bitmask == (joystick & bitmask))
        {
            joypad |= keymap.keys[i].snes9x_mask;
        }
    }

    return joypad;
}

bool S9xReadMousePosition(int32_t which1, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool JustifierOffscreen(void)
{
    return true;
}

void JustifierButtons(uint32_t *justifiers)
{
    (void)justifiers;
}

static inline void mix_samples(int32_t count)
{
    currentAudioBuffer = audioBuffers[currentAudioBuffer == audioBuffers[0]];
    if (lowpass_filter)
        S9xMixSamplesLowPass((int16_t *)currentAudioBuffer, count, AUDIO_LOW_PASS_RANGE);
    else
        S9xMixSamples((int16_t *)currentAudioBuffer, count);
}

#ifdef USE_AUDIO_TASK
static void audio_task(void *arg)
{
    rg_task_msg_t msg;
    while (rg_task_receive(&msg))
    {
        if (msg.type == RG_TASK_MSG_STOP)
            break;
        const int64_t t_start = rg_system_timer();   /* [PERF-PROBE] */
        mix_samples(AUDIO_BUFFER_LENGTH << 1);
        rg_audio_submit(currentAudioBuffer, AUDIO_BUFFER_LENGTH);
        snes_audio_task_us = (int32_t)(rg_system_timer() - t_start);   /* [PERF-PROBE] */
    }
}
#endif

static void options_handler(rg_gui_option_t *dest)
{
    *dest++ = (rg_gui_option_t){0, _("Audio enable"), "-", RG_DIALOG_FLAG_NORMAL, &apu_toggle_cb};
    *dest++ = (rg_gui_option_t){0, _("Audio filter"), "-", RG_DIALOG_FLAG_NORMAL, &lowpass_filter_cb};
    *dest++ = (rg_gui_option_t){0, _("Controls"),     "-", RG_DIALOG_FLAG_NORMAL, &menu_keymap_cb};
    *dest++ = (rg_gui_option_t){0, _("Perf probe"),   "-", RG_DIALOG_FLAG_NORMAL, &perf_probe_cb};  /* [PERF-PROBE] */
    *dest++ = (rg_gui_option_t)RG_DIALOG_END;
}

void snes_main(void)
{
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
        .options = &options_handler,
    };
    app = rg_system_reinit(AUDIO_SAMPLE_RATE, &handlers, NULL);

    // Load settings
    apu_enabled = rg_settings_get_number(NS_APP, SETTING_APU_EMULATION, 1);
    lowpass_filter = rg_settings_get_number(NS_APP, SETTING_APU_FILTER, 0);
    update_keymap(rg_settings_get_number(NS_APP, SETTING_KEYMAP, 0));

    // Allocate surfaces and audio buffers
    updates[0] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED, RG_PIXEL_565_LE, 0);
    updates[0]->height = SNES_HEIGHT;
#ifdef FRAME_DOUBLE_BUFFERING
    updates[1] = rg_surface_create(SNES_WIDTH, SNES_HEIGHT_EXTENDED, RG_PIXEL_565_LE, 0);
    updates[1]->height = SNES_HEIGHT;
#else
    updates[1] = updates[0];
#endif
    currentUpdate = updates[0];

#ifdef AUDIO_DOUBLE_BUFFERING
    audioBuffers[0] = (rg_audio_sample_t *)malloc(AUDIO_BUFFER_LENGTH * 4);
    audioBuffers[1] = (rg_audio_sample_t *)malloc(AUDIO_BUFFER_LENGTH * 4);
#else
    audioBuffers[0] = (rg_audio_sample_t *)malloc(AUDIO_BUFFER_LENGTH * 4);
    audioBuffers[1] = audioBuffers[0];
#endif
    currentAudioBuffer = audioBuffers[0];

    if (!updates[0] || !updates[1] || !audioBuffers[0] || !audioBuffers[1])
        RG_PANIC("Failed to allocate buffers!");

#ifdef USE_AUDIO_TASK
    // Set up multicore audio
    /* [PERF-PROBE 关联改动 · 注意别丢] 优先级 6 → 2。
     * 原因：core1 上 rg_display / rg_input / snes_audio 三个任务原本**同为优先级 6**，
     * 同优先级是时间片轮转 —— 一开声音，audio_task 每帧的 DSP 混音就从 display 手里
     * 分走时间片，display 追不上 16.67ms，双缓冲 updates[] 被覆写 → 花屏（真机实测：
     * 无声不花、有声就花，核心耗时 12.2→14.4）。
     * 降到 2 = 让位于显示（PCE 核心的 pce_sound 一直就是 PRIORITY_2，照它抄）。
     * 副作用：core1 若被显示吃满，音频会欠载（声音断续/拖慢 core0 的提交等待）。 */
    /* [PERF-PROBE 关联改动 · 注意别丢] 队列深度 1 → 3（改用 rg_task_create_ex）。
     * 原因：rg_task_create() 把队列深度**硬编码为 1**。深度 1 意味着主循环塞进一条
     * 消息队列就满，必须等 audio_task 把它收走才能发下一条；而 audio_task 一轮要
     * 19.4ms（mix 2.7ms + submit 阻塞 ~16.7ms 等 I2S 腾地方），几乎总卡在 submit 里
     * → 主循环每帧干等。实测 A 段（音频提交等待）均值 5.8ms / 最大 13.5ms，
     * 白吃掉 core0 的时间，把帧时间顶到 17.6ms > 16.67ms，fps 卡在 56。
     * 加深到 3 = 主循环可以提前塞 1~2 条，不必等 audio_task 回到 receive。
     * 稳态下生产速率（帧率限制器锁 60Hz）与消费速率（I2S 60Hz）相等，队列只吸收
     * 抖动、不会持续积压；万一主循环跑快了，队列填满后 send 照样阻塞 —— 行为退化成
     * 原来的样子，安全。PCE 核没用这个（它核心轻、等得起），是本核心独有的改动。 */
    audio_task_handle = rg_task_create_ex("snes_audio", &audio_task, NULL, 2048, RG_TASK_PRIORITY_2, 1, 3);
    RG_ASSERT(audio_task_handle, "Failed to create audio task!");
#endif

    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
    Settings.SoundInputRate = AUDIO_SAMPLE_RATE;
    Settings.DisableSoundEcho = false;
    Settings.InterpolatedSound = true;

    if (!S9xInitDisplay())
        RG_PANIC("Display init failed!");

    /* [PERF-PROBE] SNES 每帧画面几乎全变，逐行哈希的结果永远是"变了" —— 491KB/帧
     * 的校验和纯属白算（实测约 3.4ms/帧），而且它跑在 core1 的 display_task 上，
     * 直接和 snes_audio 抢时间。关掉 = 每行都推，画面逐像素不变。 */
    rg_display_set_partial_updates(false);

    if (!S9xInitMemory())
        RG_PANIC("Memory init failed!");

    if (!S9xInitAPU())
        RG_PANIC("APU init failed!");

    if (!S9xInitSound(0, 0))
        RG_PANIC("Sound init failed!");

    if (!S9xInitGFX())
        RG_PANIC("Graphics init failed!");

    S9xSetPlaybackRate(Settings.SoundPlaybackRate);

    const char *filename = app->romPath;

    if (rg_extension_match(filename, "zip"))
    {
        if (!rg_storage_unzip_file(filename, NULL, (void **)&Memory.ROM, &Memory.ROM_AllocSize, RG_FILE_USER_BUFFER))
            RG_PANIC("ROM file unzipping failed!");
        filename = NULL;
    }

    if (!LoadROM(filename))
        RG_PANIC("ROM loading failed!");

    if (app->bootFlags & RG_BOOT_RESUME)
    {
        rg_emu_load_state(app->saveSlot);
    }

    rg_system_set_tick_rate(Memory.ROMFramesPerSecond);
    app->frameskip = 3;

    bool menuCancelled = false;
    bool menuPressed = false;
    int skipFrames = 0;

    while (1)
    {
        uint32_t joystick = rg_input_read_gamepad();

        if (menuPressed && !(joystick & RG_KEY_MENU))
        {
            if (!menuCancelled)
            {
                rg_task_delay(50);
                rg_gui_game_menu();
            }
            menuCancelled = false;
        }
        else if (joystick & RG_KEY_OPTION)
        {
            rg_gui_options_menu();
        }

        menuPressed = joystick & RG_KEY_MENU;

        if (menuPressed && joystick & ~RG_KEY_MENU)
        {
            menuCancelled = true;
        }

        int64_t startTime = rg_system_timer();
        bool drawFrame = (skipFrames == 0);
        bool slowFrame = false;

        IPPU.RenderThisFrame = drawFrame;
        GFX.Screen = currentUpdate->data;

        S9xMainLoop();
        const int64_t t_core = rg_system_timer();      /* [PERF-PROBE] 段界：核心模拟结束 */

    #ifdef USE_AUDIO_TASK
        if (apu_enabled)
        {
            rg_task_msg_t msg = {0};
            rg_task_send(audio_task_handle, &msg);
        }
    #endif
        const int64_t t_audio = rg_system_timer();     /* [PERF-PROBE] 段界：音频提交结束 */

        if (drawFrame)
        {
            slowFrame = !rg_display_sync(false);
            rg_display_submit(currentUpdate, 0);
            currentUpdate = updates[currentUpdate == updates[0]];
        }
        const int64_t t_disp = rg_system_timer();      /* [PERF-PROBE] 段界：显示段结束 */

    #ifndef USE_AUDIO_TASK
        if (apu_enabled)
        {
            mix_samples(AUDIO_BUFFER_LENGTH << 1);
            rg_system_tick(rg_system_timer() - startTime);
            rg_audio_submit(currentAudioBuffer, AUDIO_BUFFER_LENGTH);
        }
        else
        {
            rg_system_tick(rg_system_timer() - startTime);
        }
    #else
        rg_system_tick(rg_system_timer() - startTime);
    #endif
        const int64_t t_sys = rg_system_timer();       /* [PERF-PROBE] 段界：系统段结束 */

        /* [PERF-PROBE] 四段分层计时：core / audio / display / system */
        perf_probe_tick(t_core - startTime, t_audio - t_core, t_disp - t_audio, t_sys - t_disp);

        if (skipFrames == 0)
        {
            int elapsed = rg_system_timer() - startTime;
            if (app->frameskip > 0)
                skipFrames = app->frameskip;
            else if (elapsed > app->frameTime + 1500) // Allow some jitter
                skipFrames = 1; // (elapsed / frameTime)
            else if (drawFrame && slowFrame)
                skipFrames = 1;
        }
        else if (skipFrames > 0)
        {
            skipFrames--;
        }

        /* ── [FRAME-LIMIT] 显式把帧率锁到 app->frameTime（SNES：60Hz / 16667µs）────
         * 主循环原本没有帧率限制，靠"等硬件"隐式限速。但两个节流源都被拆了：
         *   ① rg_display_sync(false) 是非阻塞的（做性能仪表时改的）；
         *   ② 音频提交阻塞在 AUDIO-SYNC-FIX 后消失（A 段 8.9ms → 0.0ms）。
         * 结果核心能跑多快跑多快（真机实测 81~99fps）：游戏以 1.35~1.65 倍速快进，
         * 而音频按真实 32000Hz 播放 → 音画不同步；同时主循环比显示任务跑得还快，
         * 双缓冲 updates[] 被覆盖 → 花屏。
         * 这里补上缺失的节流：超时就睡掉余量（睡眠不烧 CPU）。
         * 核心比 60fps 慢时 remain<0，自动不睡 —— 慢的场景照旧自然掉帧。 */
        {
            const int64_t elapsed = rg_system_timer() - startTime;
            const int64_t remain = app->frameTime - elapsed;
            if (remain > 1000)   /* 不足 1ms 就不睡（delay(0) 只是 yield，没意义） */
                rg_task_delay((int)((remain + 500) / 1000));
        }
    }
}
