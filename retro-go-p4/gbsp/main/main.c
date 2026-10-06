#include <rg_system.h>
#include <stdio.h>
#include <stdlib.h>

#include "../components/gbsp-libretro/common.h"
#include "../components/gbsp-libretro/memmap.h"
#include "../components/gbsp-libretro/gba_memory.h"
#include "../components/gbsp-libretro/gba_cc_lut.h"
#include "../components/gbsp-libretro/savestate.h"

#define AUDIO_SAMPLE_RATE (GBA_SOUND_FREQUENCY)
#define AUDIO_BUFFER_LENGTH (AUDIO_SAMPLE_RATE / 60 + 1)

#include "bios.h"

u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
u32 translation_gate_targets = 0;
boot_mode selected_boot_mode = boot_game;

u32 skip_next_frame = 0;
int sprite_limit = 1;

/* 诊断计数器（2026-10-06 "横条闪烁"排查用，仅 RG_GBA_DIAG 编译） */
static uint32_t display_busy_frames;     /* 提交时显示任务仍占着上一帧的次数 */
static uint32_t display_submit_frames;   /* 提交总次数 */

/* 上游把 dynarec_enable 定义在 libretro/libretro.c，但 retro-go 的入口不编译那个文件
 * （初始化与主循环由我们接管）→ 必须在 app 层定义。
 * savestate.c 靠它判断是否需要保存/恢复 JIT 翻译缓存，开着 dynarec 就必须为 1。 */
/* 2026-10-03 临时 A/B：关掉 dynarec 走解释器，用来判定"取名界面方向键错乱"
 * 是否是 P4 移植版 dynarec 的问题（Gemini 第 4 条假设 + 宿主/真机行为不一致）。
 * 判定完要改回 1（解释器慢很多）。 */
int dynarec_enable = 1;

static rg_surface_t *updates[2];
static rg_surface_t *currentUpdate;
static rg_app_t *app;

void netpacket_poll_receive()
{
}
void netpacket_send(uint16_t client_id, const void *buf, size_t len)
{
}

static bool screenshot_handler(const char *filename, int width, int height)
{
    return rg_surface_save_image_file(currentUpdate, filename, width, height);
}

static bool save_state_handler(const char *filename)
{
    /* 416KB 状态缓冲：宿主用普通 malloc，真机上这尺寸会落到 PSRAM 堆。 */
    void *buffer = malloc(GBA_STATE_MEM_SIZE);
    if (!buffer)
    {
        RG_LOGE("Save state: out of memory (%d bytes).\n", (int)GBA_STATE_MEM_SIZE);
        return false;
    }
    gba_save_state(buffer);
    /* flags=0：原子性由框架负责 —— rg_system.c 先建好目录，再让我们写到一个 .new 临时文件，
     * 最后才替换正式存档。（rg_storage 自己的 RG_FILE_ATOMIC_WRITE 目前还是个 TODO，传了没用。） */
    bool ok = rg_storage_write_file(filename, buffer, GBA_STATE_MEM_SIZE, 0);
    free(buffer);
    if (!ok)
        RG_LOGE("Save state failed: %s\n", filename);
    return ok;
}

static bool load_state_handler(const char *filename)
{
    void *buffer = NULL;
    size_t size = 0;

    if (!rg_storage_read_file(filename, &buffer, &size, 0))
        return false;

    /* gba_load_state() 没有长度参数，文件短了会被越界读 —— 我们自己的存档正好等于
     * GBA_STATE_MEM_SIZE（核心内部再校验 MAGIC/VERSION），比它小的一律拒绝。 */
    bool ok = (size >= GBA_STATE_MEM_SIZE) && gba_load_state(buffer);
    free(buffer);
    if (!ok)
        RG_LOGE("Load state failed: %s (%u bytes)\n", filename, (unsigned)size);
    return ok;
}

static bool reset_handler(bool hard)
{
    return true;
}

static void event_handler(int event, void *arg)
{
    if (event == RG_EVENT_REDRAW)
    {
        rg_display_submit(currentUpdate, 0);
    }
}

int16_t input_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    // RG_LOGI("%u, %u, %u, %u", port, device, index, id);
    uint32_t joystick = rg_input_read_gamepad();
    int16_t val = 0;
    if (joystick & RG_KEY_DOWN) val |= (1 << RETRO_DEVICE_ID_JOYPAD_DOWN);
    if (joystick & RG_KEY_UP) val |= (1 << RETRO_DEVICE_ID_JOYPAD_UP);
    if (joystick & RG_KEY_LEFT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_LEFT);
    if (joystick & RG_KEY_RIGHT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT);
    if (joystick & RG_KEY_START) val |= (1 << RETRO_DEVICE_ID_JOYPAD_START);
    if (joystick & RG_KEY_SELECT) val |= (1 << RETRO_DEVICE_ID_JOYPAD_SELECT);
    if (joystick & RG_KEY_B) val |= (1 << RETRO_DEVICE_ID_JOYPAD_B);
    if (joystick & RG_KEY_A) val |= (1 << RETRO_DEVICE_ID_JOYPAD_A);
    /* GBA 的肩键：之前这里漏了，导致触摸虚拟手柄上的 L/R 画得出来、按了没反应
     * （RG_KEY_L/R 在 rg_input.h 有定义、touch_layout.h 也画了，但没人往核心里传）。 */
    if (joystick & RG_KEY_L) val |= (1 << RETRO_DEVICE_ID_JOYPAD_L);
    if (joystick & RG_KEY_R) val |= (1 << RETRO_DEVICE_ID_JOYPAD_R);
    /* 核心把 X/Y 定义为 Turbo A / Turbo B（gpsp_turbo_period 真的实现了），
     * 触摸手柄上画了这两颗键，同样需要在这里传下去。 */
    if (joystick & RG_KEY_X) val |= (1 << RETRO_DEVICE_ID_JOYPAD_X);
    if (joystick & RG_KEY_Y) val |= (1 << RETRO_DEVICE_ID_JOYPAD_Y);
    return val;
}

void set_fastforward_override(bool fastforward)
{
}

/* ===== 电池存档（SRAM）持久化 —— 分三步做，每步单独验证 =================
 * 背景：此前 gamepak_backup 从没落过盘，每次开机还被 memset 成 0xFF，
 *       游戏内存档点存的东西一重启就丢。
 * 上次一步到位（算路径+建目录+读文件）导致「无法进入游戏」，已回退到
 * commit 9f45df5 之前。这次拆开：
 *   step1 只算路径（纯字符串，零 I/O）      ← 本步
 *   step2 开机读回 .srm（要建目录、要读 SD）
 *   step3 运行中检测变化写回
 * ========================================================================= */
static char sram_path[256];   /* ROM 主名（stem） */
static char sram_file[320];   /* 完整存档路径 */
static uint32_t sram_last_hash;

static void sram_setup_path(void)
{
    const char *rom = app->romPath ? app->romPath : "";
    const char *rel = strstr(rom, "/roms/");
    rel = rel ? rel + 6 : (strrchr(rom, '/') ? strrchr(rom, '/') + 1 : rom);
    snprintf(sram_path, sizeof sram_path, "%s", rel);
    char *dot = strrchr(sram_path, '.');
    if (dot) *dot = 0;
    snprintf(sram_file, sizeof sram_file, RG_BASE_PATH_SAVES "/gba/%s.srm", sram_path);
    RG_LOGI("SRAM: save file = %s\n", sram_file);
}

static void sram_load(void)
{
    if (!sram_path[0]) return;
    void *buf = NULL;
    size_t size = 0;
    if (!rg_storage_read_file(sram_file, &buf, &size, 0) || !buf)
    {
        RG_LOGI("SRAM step2: no save file yet (%s)\n", sram_file);
        return;
    }
    size_t n = size < sizeof(gamepak_backup) ? size : sizeof(gamepak_backup);
    memcpy(gamepak_backup, buf, n);
    free(buf);
    RG_LOGI("SRAM step2: loaded %u bytes <- %s\n", (unsigned)n, sram_file);
}

/* step3：运行中检测变化就写回。主循环是 while(1)、App 没有干净退出路径，
 * 所以只能走"定期检测"，不能挂"退出时保存"。
 * 建目录放在这里（第一次真要写时才建）——开机阶段一律不碰 SD。 */
static void sram_autosave(void)
{
#if defined(RG_TEST_NO_AUTOSAVE) && RG_TEST_NO_AUTOSAVE
    return;   /* 测试期禁用自动存档：防止把测试 SRAM 写回用户存档 */
#endif
    static int64_t next_check = 0;
    int64_t now = rg_system_timer();
    if (now < next_check) return;
    next_check = now + 2000000;            /* 每 2 秒看一次 */

    if (!sram_file[0]) return;

    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(gamepak_backup); ++i)
        h = (h ^ gamepak_backup[i]) * 16777619u;
    if (h == sram_last_hash) return;       /* 没变就不写 */

    char dir[320];
    snprintf(dir, sizeof dir, "%s", sram_file);
    for (char *p = dir + 1; *p; ++p)
        if (*p == '/') { *p = 0; rg_storage_mkdir(dir); *p = '/'; }

    if (rg_storage_write_file(sram_file, gamepak_backup, sizeof(gamepak_backup), 0))
    {
        sram_last_hash = h;
        RG_LOGI("SRAM step3: saved %u bytes -> %s\n", (unsigned)sizeof(gamepak_backup), sram_file);
    }
    else
        RG_LOGE("SRAM step3: save FAILED -> %s\n", sram_file);
}

#if defined(RG_TARGET_SDL2)
#include <sys/stat.h>
#include <string.h>
/* ── 宿主自动化测试钩子（**只编进 SDL2 宿主构建**，真机一行都不编）───────────────
 * 动机：恶魔城这类游戏的"片头→标题→存档选择→名字输入"要跑 1~2 分钟才到目标界面，
 *       每次改一点代码都重放一遍太慢、还容易错过窗口。这里的做法是**一次长跑 + 切片存档**：
 *         RG_TEST_SAVEAT="t1,t2,...:<目录>"   在这些墙钟秒各存一份 state_t##.gs0
 *         RG_TEST_RAMAT="t1,t2,...:<目录>"    在这些秒把 EWRAM+IWRAM 落盘 ram_t##.bin
 *         RG_TEST_LOADSTATE=<文件>            启动时（reset 之后）载入切片，秒级回到目标界面
 *      之后配合 rg_input.c 的 RG_TEST_KEYS（时间轴注入按键）就能做无人值守 A/B。
 * 注意：这些钩子写的是**绝对路径**（绕过 rg_storage 的路径约束），只在宿主上用。 */
static void rg_test_dump_ram(const char *path)
{
    const size_t ew = 256 * 1024, iw = 32 * 1024;
    u8 *buf = malloc(ew + iw);
    if (!buf)
        return;
    for (size_t i = 0; i < ew; i++)
        buf[i] = memory_map_read[(0x02000000 + i) >> 15][(0x02000000 + i) & 0x7FFF];
    for (size_t i = 0; i < iw; i++)
        buf[ew + i] = memory_map_read[(0x03000000 + i) >> 15][(0x03000000 + i) & 0x7FFF];
    FILE *f = fopen(path, "wb");
    if (f)
    {
        fwrite(buf, 1, ew + iw, f);
        fclose(f);
        RG_LOGI("RG_TEST_RAMAT: -> %s (%u bytes)\n", path, (unsigned)(ew + iw));
    }
    else
        RG_LOGE("RG_TEST_RAMAT: cannot write %s\n", path);
    free(buf);
}

static void rg_test_hooks(void)
{
    static bool init = false, on = false;
    static float ts[256], tr[256];
    static int ns, nr, is, ir;
    static char ds[400], dr[400];
    static float t0;

    if (!init)
    {
        init = true;
        t0 = rg_system_timer() / 1000000.0f;
        const char *evs = getenv("RG_TEST_SAVEAT");
        const char *evr = getenv("RG_TEST_RAMAT");
        if (evs)
        {
            char buf[800];
            snprintf(buf, sizeof(buf), "%s", evs);
            char *colon = strrchr(buf, ':');
            if (colon)
            {
                *colon = 0;
                snprintf(ds, sizeof(ds), "%s", colon + 1);
                char *tok = strtok(buf, ",");
                while (tok && ns < 256) { ts[ns++] = atof(tok); tok = strtok(NULL, ","); }
            }
        }
        if (evr)
        {
            char buf[800];
            snprintf(buf, sizeof(buf), "%s", evr);
            char *colon = strrchr(buf, ':');
            if (colon)
            {
                *colon = 0;
                snprintf(dr, sizeof(dr), "%s", colon + 1);
                char *tok = strtok(buf, ",");
                while (tok && nr < 256) { tr[nr++] = atof(tok); tok = strtok(NULL, ","); }
            }
        }
        on = (ns > 0 || nr > 0);
        if (on)
            RG_LOGI("RG_TEST: saveat n=%d dir='%s' ramat n=%d dir='%s'\n", ns, ds, nr, dr);
    }
    if (!on)
        return;

    const float now = rg_system_timer() / 1000000.0f - t0;
    while (is < ns && now >= ts[is])
    {
        char path[512];
        void *buffer = malloc(GBA_STATE_MEM_SIZE);
        if (buffer)
        {
            gba_save_state(buffer);
            snprintf(path, sizeof(path), "%s/state_t%03d.gs0", ds, (int)ts[is]);
            FILE *f = fopen(path, "wb");
            if (f)
            {
                fwrite(buffer, 1, GBA_STATE_MEM_SIZE, f);
                fclose(f);
                RG_LOGI("RG_TEST_SAVEAT: t=%.1fs -> %s\n", now, path);
            }
            free(buffer);
        }
        is++;
    }
    while (ir < nr && now >= tr[ir])
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/ram_t%03d.bin", dr, (int)tr[ir]);
        rg_test_dump_ram(path);
        ir++;
    }
}
#endif /* RG_TARGET_SDL2 */

#if defined(RG_GBA_DIAG) && RG_GBA_DIAG && !defined(RG_TARGET_SDL2)
/* 2026-10-06 临时：无人值守真机测试 —— 启动器没给 ROM 路径时自己挑一个 ROM 载入。
 * 为什么需要：真机上没人点屏幕，必须让游戏自己跑起来，才能验证"静态菜单里按键没反应"。
 * 优先 恶魔城系列 目录里的第一个 .gba（晓月/月下都在里面），挑不到再退回整个 gba 目录。 */
static char rg_test_auto_rom[RG_PATH_MAX + 1];
static char rg_test_auto_rom_pref[RG_PATH_MAX + 1];   /* 首选：名字含"晓月"的那个 */
static int rg_test_rom_cb(const rg_scandir_t *entry, void *arg)
{
    if (!entry->is_file || !rg_extension_match(entry->basename, "gba"))
        return RG_SCANDIR_SKIP;
    if (!rg_test_auto_rom[0])
        snprintf(rg_test_auto_rom, sizeof(rg_test_auto_rom), "%s", entry->path);
    if (strstr(entry->basename, "晓月") && !rg_test_auto_rom_pref[0])
        snprintf(rg_test_auto_rom_pref, sizeof(rg_test_auto_rom_pref), "%s", entry->path);
    return RG_SCANDIR_SKIP;
}
static void rg_test_auto_rom_pick(void)
{
    const char *cands[] = { RG_BASE_PATH_ROMS "/gba/恶魔城系列（3作）", RG_BASE_PATH_ROMS "/gba" };
    for (size_t i = 0; i < RG_COUNT(cands) && !rg_test_auto_rom_pref[0]; ++i)
        rg_storage_scandir(cands[i], rg_test_rom_cb, NULL, 0);
    if (rg_test_auto_rom_pref[0])
        snprintf(rg_test_auto_rom, sizeof(rg_test_auto_rom), "%s", rg_test_auto_rom_pref);
    RG_LOGW("RG_TEST_AUTOROM: %s\n", rg_test_auto_rom[0] ? rg_test_auto_rom : "(none)");
}
#endif

void app_main(void)
{
#if defined(RG_TARGET_SDL2)
    /* 宿主：日志直接落盘不丢 —— 被 alarm 杀掉时块缓冲会整段丢失（吃过这个亏）。 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
#endif
    const rg_handlers_t handlers = {
        .loadState = &load_state_handler,
        .saveState = &save_state_handler,
        .reset = &reset_handler,
        .screenshot = &screenshot_handler,
        .event = &event_handler,
    };

    app = rg_system_init(AUDIO_SAMPLE_RATE, &handlers, NULL);
    // app = rg_system_init(AUDIO_SAMPLE_RATE * 0.7, &handlers, NULL);
    // rg_system_set_overclock(2);

    updates[0] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_FAST);
    updates[0]->height = GBA_SCREEN_HEIGHT;
    /* 第二块画面缓冲（2026-10-06 修"横条闪烁"）：上游 NES/GBC/SNES/gwenesis 都是两块 +
     * 提交后轮换。GBA 移植只建了 updates[0] 且永不轮换 ⇒ 显示任务（另一个线程）还在异步读
     * 这块缓冲时，核心已把下一帧渲染进同一块内存 ⇒ 显示任务读到"上半屏旧帧 + 下半屏新帧"
     * 拼起来的画面 = 一条条横条；快速卷轴/换房间时最严重（变化行最多 ⇒ 显示耗时最长 ⇒
     * 落后核心最多）。建不出来就退回原单缓冲行为（只是仍有横条，不会崩）。 */
    updates[1] = rg_surface_create(GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT + 1, RG_PIXEL_565_LE, MEM_FAST);
    if (updates[1])
        updates[1]->height = GBA_SCREEN_HEIGHT;
    currentUpdate = updates[0];

    gba_screen_pixels = currentUpdate->data;

    RG_LOGI("GBA");

    libretro_supports_bitmasks = true;
    retro_set_input_state(input_cb);
    init_gamepak_buffer();
    init_sound();

    if (load_bios(RG_BASE_PATH_BIOS "/gba_bios.bin") != 0)
        memcpy(bios_rom, open_gba_bios_rom, sizeof(bios_rom));

    memset(gamepak_backup, 0xff, sizeof(gamepak_backup));
#if defined(RG_GBA_DIAG) && RG_GBA_DIAG && !defined(RG_TARGET_SDL2)
    /* 强制用晓月（用户复现对象）：忽略启动器/boot.json 给的路径，保证每次复现的是同一个 ROM */
    rg_test_auto_rom_pick();
    if (rg_test_auto_rom[0])
        app->romPath = rg_test_auto_rom;
#endif
    sram_setup_path();   /* step1：只算路径，不碰 SD */
#if defined(RG_TEST_BOOT_RESUME) && RG_TEST_BOOT_RESUME
    /* 测试钩子（2026-10-06）：真机上没人点启动器，这里强制走"启动器带了存档位"的那条路，
     * 用来验证 step2b 的自动读档。语义与启动器等价：RG_BOOT_RESUME + slot<<4。
     * slot < 0 表示"用最后用过的那个存档位"（与启动器 Resume 菜单的默认高亮同一口径）。用完撤掉。 */
    {
        int slot = RG_TEST_BOOT_RESUME_SLOT;
        if (slot < 0)
        {
            rg_emu_states_t *st = rg_emu_get_states(app->romPath, 4);
            slot = st->lastused ? (int)st->lastused->id : (st->latest ? (int)st->latest->id : 0);
            RG_LOGW("RG_TEST_RESUME: 自动挑存档位 slot=%d（存在 %d 个）\n", slot, (int)st->used);
            free(st);
        }
        app->bootFlags |= RG_BOOT_RESUME;
        app->saveSlot = slot;
        RG_LOGW("RG_TEST_RESUME: forced bootFlags=%02X slot=%d rom=%s\n",
                (unsigned)app->bootFlags, (int)app->saveSlot, app->romPath);
    }
#endif

    if (load_gamepak(NULL, app->romPath, FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        RG_PANIC("Could not load the game file.");
    }

    RG_LOGI("reset_gba");
    reset_gba();

    RG_LOGI("emulation loop");

    sram_load();   /* step2：开机读回 .srm（只读，文件不存在就跳过）*/

    /* step2b（2026-10-06 修）：启动器带了"从存档位起"的意图就恢复它。
     * 上游其它核心（NES / SNES / GBC / SMS / PCE / GW / fmsx / gwenesis）都有这一段，
     * GBA 移植漏了 ⇒ 从启动器选"继续游戏 + 存档位"进游戏后仍是新开局（只回了 .srm），
     * 必须再从 menu 手动读档；顺带"menu 里保存并退出 → 下次开机自动续上"也失效。
     * 位置与上游一致：reset 之后、主循环之前（app->saveSlot 由 rg_system.c 从 bootFlags 解出）。 */
    if (app->bootFlags & RG_BOOT_RESUME)
        rg_emu_load_state(app->saveSlot);

#if defined(RG_TARGET_SDL2)
    /* 宿主：载入切片存档，秒级回到目标界面（配合 RG_TEST_SAVEAT） */
    {
        const char *p = getenv("RG_TEST_LOADSTATE");
        if (p && *p)
        {
            void *buffer = malloc(GBA_STATE_MEM_SIZE);
            FILE *f = buffer ? fopen(p, "rb") : NULL;
            size_t n = f ? fread(buffer, 1, GBA_STATE_MEM_SIZE, f) : 0;
            if (f)
                fclose(f);
            bool ok = (n == GBA_STATE_MEM_SIZE) && gba_load_state(buffer);
            RG_LOGI("RG_TEST_LOADSTATE: %s -> %s (%u bytes)\n", p, ok ? "OK" : "FAILED", (unsigned)n);
            free(buffer);
        }
    }
#endif

    while (true)
    {
        // RG_TIMER_INIT();

#if defined(RG_TARGET_SDL2)
        rg_test_hooks();
#endif

        rg_audio_sample_t mixbuffer[AUDIO_BUFFER_LENGTH];
        uint32_t joystick = rg_input_read_gamepad();

        if (joystick & (RG_KEY_MENU | RG_KEY_OPTION))
        {
            if (joystick & RG_KEY_MENU)
                rg_gui_game_menu();
            else
                rg_gui_options_menu();
        }

        int64_t start_time = rg_system_timer();

        update_input();
        rumble_frame_reset();

        clear_gamepak_stickybits();
#if defined(RG_TEST_FLUSH_CACHE) && RG_TEST_FLUSH_CACHE
        /* TEST 2026-10-06：每帧强制刷新翻译缓存（限 f<600 控制开销），
         * 验证「游戏把解压出来的代码写进 RAM 后 dynarec 仍用陈旧翻译」这一假设。 */
        if (frame_counter < 600)
            flush_translation_cache_ram();
#endif
        gba_execute_frame(execute_cycles);
        sram_autosave();          /* step3：电池存档变了就落盘 */   // dynarec 可用时走 JIT，否则走解释器
        // RG_TIMER_LAP("execute_arm");

#if defined(RG_GBA_DIAG) && RG_GBA_DIAG && RG_GBA_DIAG_SPAM
        /* ── 真机"输入 vs 显示"分辨探针（2026-10-06）────────────────────────────
         * 只有一个问题要回答：菜单里按方向键没反应，是**游戏没收到键**还是**面板没收到像素**。
         * 三条独立证据：
         *   DIAG_CURSOR 游戏自己的菜单光标字节（EWRAM 0x020004F9；宿主已证明按左 -1）
         *   DIAG_DIR    游戏自己的方向状态（EWRAM 0x0200001A；宿主实测 按左=02 按下=06）
         *   DIAG_FB     游戏帧缓冲哈希 vs 累计"推给面板的像素"哈希/次数/丢块数
         * 判据：按键后 CURSOR/DIR 变了、推像素哈希没变 ⇒ 显示层丢了；两者都没变 ⇒ 游戏侧没收到键。 */
        {
            static unsigned diag_tick;
            static u8 last_cur = 0xFF;
            static u16 last_dir = 0xFFFF;
            extern uint32_t rg_display_push_hash, rg_display_push_blocks;
            extern uint32_t rg_display_dirty_lines;
            extern uint32_t rg_display_push_drops, rg_display_push_redirty;
            const u8 *ew = (const u8 *)memory_map_read[0x02000000 >> 15];
            u8 cur = ew[0x04F9];
            u16 dir = (u16)(ew[0x001A] | (ew[0x001B] << 8));
            if (cur != last_cur)
            {
                last_cur = cur;
#if defined(RG_TARGET_SDL2)
                { FILE *df = fopen("/tmp/diag_host.log", "a");
                  if (df) { fprintf(df, "DIAG_CURSOR f=%u cur=%u\n", (unsigned)frame_counter, (unsigned)cur); fclose(df); } }
#else
                RG_LOGW("DIAG_CURSOR f=%u cur=%u\n", (unsigned)frame_counter, (unsigned)cur);
#endif
            }
            if (dir != last_dir)
            {
                last_dir = dir;
#if defined(RG_TARGET_SDL2)
                { FILE *df = fopen("/tmp/diag_host.log", "a");
                  if (df) { fprintf(df, "DIAG_DIR f=%u dir=%04X\n", (unsigned)frame_counter, (unsigned)dir); fclose(df); } }
#else
                RG_LOGW("DIAG_DIR f=%u dir=%04X\n", (unsigned)frame_counter, (unsigned)dir);
#endif
            }
            if ((diag_tick++ % 60) == 0)
            {
                u32 hg = rg_hash((const char *)gba_screen_pixels, GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * 2);
#if defined(RG_TARGET_SDL2)
                /* 宿主日志被吞（实测 stdout 无输出），诊断直接落盘，便于和真机逐帧比对 */
                {
                    FILE *df = fopen("/tmp/diag_host.log", "a");
                    if (df)
                    {
                        fprintf(df, "DIAG_FB f=%u game=%08X push=%08X n=%u drops=%u dirty=%u\n",
                                (unsigned)frame_counter, (unsigned)hg,
                                (unsigned)rg_display_push_hash, (unsigned)rg_display_push_blocks,
                                (unsigned)rg_display_push_drops, (unsigned)rg_display_dirty_lines);
                        fclose(df);
                    }
                }
#else
                RG_LOGW("DIAG_FB f=%u game=%08X push=%08X n=%u drops=%u dirty=%u\n",
                        (unsigned)frame_counter, (unsigned)hg,
                        (unsigned)rg_display_push_hash, (unsigned)rg_display_push_blocks,
                        (unsigned)rg_display_push_drops, (unsigned)rg_display_dirty_lines);
#endif
            }
            /* ── 区域哈希探针（2026-10-06）────────────────────────────────────────
             * mh 只说"两构建内存不同"，不说"哪里不同"。这里把 EWRAM 切 8 块(32KB)、
             * IWRAM 切 8 块(4KB)，每 10 帧打一次，按块定位分岔位置。
             * 同时跑两次解释器做运次间对照：若解释器两次自身就不一致，说明是未初始化内存。 */
            if (frame_counter <= 600 && (frame_counter % 10) == 0)
            {
                const char *ew = (const char *)memory_map_read[0x02000000 >> 15];
                const char *iw = (const char *)memory_map_read[0x03000000 >> 15];
                char b[320]; int o = 0;
                o += snprintf(b + o, sizeof(b) - o, "MHDIFF f=%u E", (unsigned)frame_counter);
                for (int sl = 0; sl < 8; ++sl)
                    o += snprintf(b + o, sizeof(b) - o, " %08X", (unsigned)rg_hash(ew + sl * 32768, 32768));
                o += snprintf(b + o, sizeof(b) - o, " I");
                for (int sl = 0; sl < 8; ++sl)
                    o += snprintf(b + o, sizeof(b) - o, " %08X", (unsigned)rg_hash(iw + sl * 4096, 4096));
                RG_LOGW("%s\n", b);
            }

            /* ── 状态级探针（2026-10-06）：定位 dynarec 从哪一帧开始算错 ─────────────
             * rh = 整个 GBA 寄存器文件 reg[0..63] 的哈希（CPSR 也在这段里），每帧打；
             *      mh = EWRAM(256K)+IWRAM(32K) 哈希，每 60 帧打一次。
             * 判据：宿主(解释器) 与 真机(dynarec) 的 rh 序列**第一处分歧** = dynarec 算错的那一帧，
             *       该帧的 pc 就是出错代码的位置。 */
            {
                extern u32 reg[64];
                u32 rh = rg_hash((const char *)reg, 18 * sizeof(u32));  /* 只比 r0-r15+CPSR+模式：reg[16..63] 是 JIT 暂存/缓存槽，两边布局本就不同 */
                unsigned freq = (frame_counter <= 420) ? 1u : 60u;
                if ((frame_counter % freq) == 0)
                {
                    u32 mh = 0;
                    if ((frame_counter % 60) == 0)
                    {
                        const char *ew = (const char *)memory_map_read[0x02000000 >> 15];
                        const char *iw = (const char *)memory_map_read[0x03000000 >> 15];
                        mh = rg_hash(ew, 256 * 1024) ^ rg_hash(iw, 32 * 1024);
                    }
#if defined(RG_TARGET_SDL2)
                    {
                        FILE *df = fopen("/tmp/diag_host.log", "a");
                        if (df)
                        {
                            fprintf(df, "DIAG_ST f=%u rh=%08X pc=%08X mh=%08X\n",
                                    (unsigned)frame_counter, (unsigned)rh, (unsigned)reg[15], (unsigned)mh);
                            fclose(df);
                        }
                    }
#else
                    RG_LOGW("DIAG_ST f=%u rh=%08X pc=%08X mh=%08X\n",
                            (unsigned)frame_counter, (unsigned)rh, (unsigned)reg[15], (unsigned)mh);
#endif
                }
            }
        }
#endif
        if (!skip_next_frame)
        {
            /* 仪表（2026-10-06，仅诊断版编译）：提交前先问显示任务是否还占着上一帧。
             * 占着 = 我们马上要覆盖它可能还在读的缓冲。轮换生效后正常应恒为"空"。 */
#if defined(RG_GBA_DIAG) && RG_GBA_DIAG
            display_submit_frames++;
            if (!rg_display_sync(false))
                display_busy_frames++;
#endif
            rg_display_submit(currentUpdate, 0);
            /* 轮换：交给显示任务的那块不再被核心改写，核心画到另一块（gba_screen_pixels
             * 是核心每帧现读的全局，见 components/gbsp-libretro/video.cpp get_screen_pixels()）。 */
            if (updates[1] && !RG_TEST_NO_SURFACE_ROTATION)
            {
                currentUpdate = updates[currentUpdate == updates[0]];
                gba_screen_pixels = currentUpdate->data;
            }
        }

        size_t frames_count = sound_read_samples((s16 *)mixbuffer, AUDIO_BUFFER_LENGTH);
        // RG_TIMER_LAP("sound_read_samples");

        rg_system_tick(rg_system_timer() - start_time);

        rg_audio_submit(mixbuffer, frames_count);
        // RG_TIMER_LAP("rg_audio_submit");

#if defined(RG_GBA_DIAG) && RG_GBA_DIAG
        {
            static int64_t tear_last_us;
            static uint32_t tear_last_busy;
            const int64_t tear_now = rg_system_timer();
            if (tear_now - tear_last_us >= 1000000)
            {
                RG_LOGW("DIAG_TEAR: 每秒提交时显示仍忙 = %u 帧（累计忙 %u / 提交 %u）\n",
                        (unsigned)(display_busy_frames - tear_last_busy),
                        (unsigned)display_busy_frames, (unsigned)display_submit_frames);
                tear_last_busy = display_busy_frames;
                tear_last_us = tear_now;
            }
        }
#endif

        if (skip_next_frame == 0)
            skip_next_frame = app->frameskip;
        else if (skip_next_frame > 0)
            skip_next_frame--;
    }

    RG_PANIC("GBsP Ended");
}
