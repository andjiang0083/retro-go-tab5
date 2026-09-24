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

/* 上游把 dynarec_enable 定义在 libretro/libretro.c，但 retro-go 的入口不编译那个文件
 * （初始化与主循环由我们接管）→ 必须在 app 层定义。
 * savestate.c 靠它判断是否需要保存/恢复 JIT 翻译缓存，开着 dynarec 就必须为 1。 */
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

void app_main(void)
{
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
    if (load_gamepak(NULL, app->romPath, FEAT_DISABLE, FEAT_DISABLE, SERIAL_MODE_DISABLED) != 0)
    {
        RG_PANIC("Could not load the game file.");
    }

    RG_LOGI("reset_gba");
    reset_gba();

    RG_LOGI("emulation loop");

    while (true)
    {
        // RG_TIMER_INIT();

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
        gba_execute_frame(execute_cycles);   // dynarec 可用时走 JIT，否则走解释器
        // RG_TIMER_LAP("execute_arm");

        if (!skip_next_frame)
            rg_display_submit(currentUpdate, 0);

        size_t frames_count = sound_read_samples((s16 *)mixbuffer, AUDIO_BUFFER_LENGTH);
        // RG_TIMER_LAP("sound_read_samples");

        rg_system_tick(rg_system_timer() - start_time);

        rg_audio_submit(mixbuffer, frames_count);
        // RG_TIMER_LAP("rg_audio_submit");

        if (skip_next_frame == 0)
            skip_next_frame = app->frameskip;
        else if (skip_next_frame > 0)
            skip_next_frame--;
    }

    RG_PANIC("GBsP Ended");
}
