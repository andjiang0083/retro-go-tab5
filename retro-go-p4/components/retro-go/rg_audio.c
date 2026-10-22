/* Modifications copyright (c) 2026 andjiang0083
 * The upstream file of this name comes from retro-go
 * (https://github.com/ducalex/retro-go, GPLv2); this port keeps it
 * under GPL-2.0-only. See CREDITS.md in the repository root.
 */

#include "rg_system.h"
#include "rg_audio.h"

#include <stdlib.h>
#include <string.h>

/* Only the tab5 target defines this; give every other target a clean 0. */
#ifndef RG_AUDIO_USE_TAB5_CODEC
#define RG_AUDIO_USE_TAB5_CODEC 0
#endif

extern const rg_audio_driver_t rg_audio_driver_dummy;
#if RG_AUDIO_USE_TAB5_CODEC
extern const rg_audio_driver_t rg_audio_driver_tab5_es8388;
#endif
extern const rg_audio_driver_t rg_audio_driver_buzzer;
extern const rg_audio_driver_t rg_audio_driver_i2s;
extern const rg_audio_driver_t rg_audio_driver_sdl2;

// static const rg_audio_driver_t *drivers[] = {
//     NULL,
// };

static const rg_audio_sink_t sinks[] = {
    {&rg_audio_driver_dummy,  0, "Dummy"  },
#if RG_AUDIO_USE_TAB5_CODEC
    {&rg_audio_driver_tab5_es8388, 0, "ES8388"},
#endif
#if RG_AUDIO_USE_INT_DAC
    {&rg_audio_driver_i2s,    0, "Speaker"},
#endif
#if RG_AUDIO_USE_EXT_DAC
    {&rg_audio_driver_i2s,    1, "Ext DAC"},
#endif
#if RG_AUDIO_USE_SDL2
    {&rg_audio_driver_sdl2,   0, "SDL2"   },
#endif
#if RG_AUDIO_USE_BUZZER_PIN
    {&rg_audio_driver_buzzer, 0, "Buzzer" },
#endif
    // {rg_audio_driver_bt_a2dp, 0, "Bluetooth"},
};

#define ACQUIRE_DEVICE(timeout)                         \
    ({                                                  \
        bool lock = rg_mutex_take(audio.lock, timeout); \
        if (!lock)                                      \
            RG_LOGE("Failed to acquire lock!\n");       \
        lock;                                           \
    })
#define RELEASE_DEVICE() rg_mutex_give(audio.lock)

static struct
{
    const rg_audio_sink_t *sink;
    const rg_audio_driver_t *driver;
    rg_mutex_t *lock;
    int sampleRate;
    int filter;
    int volume;
    bool muted;
} audio;
static rg_audio_counters_t counters;

static const char *SETTING_DRIVER = "AudioDriver";
static const char *SETTING_DEVICE = "AudioDevice";
static const char *SETTING_VOLUME = "Volume";
static const char *SETTING_FILTER = "AudioFilter";

static const char *get_last_driver_error(void)
{
    if (audio.driver && audio.driver->get_error)
        return audio.driver->get_error();
    return "Unspecified Error";
}

void rg_audio_init(int sampleRate)
{
    /* 调用方可能传 0（config 未定义音频采样率）⇒ Dummy sink 会除零变成 inf 睡死。
     * 统一兜底，非法值一律按 GBA 原生的 32768Hz 处理（日志可查）。 */
    if (sampleRate <= 0)
    {
        RG_LOGW("rg_audio: 采样率非法(%d)，兜底为 32768Hz\n", sampleRate);
        sampleRate = 32768;
    }
#ifdef RG_AUDIO_FORCE_SAMPLE_RATE
    /* 采样率固定的音频芯片（I2S 从机）要求核心按同一率喂数据，否则会慢放/快放。
     * 这里按 target 声明强制对齐（未定义此宏的 target ⇒ 逻辑与产物逐字节不变）。 */
    if (sampleRate != RG_AUDIO_FORCE_SAMPLE_RATE)
    {
        RG_LOGI("rg_audio: 采样率按 target 强制 %dHz → %dHz\n", sampleRate, RG_AUDIO_FORCE_SAMPLE_RATE);
        sampleRate = RG_AUDIO_FORCE_SAMPLE_RATE;
    }
#endif

    RG_ASSERT(audio.sink == NULL, "Audio sink already initialized!");

    if (!audio.lock)
    {
        audio.lock = rg_mutex_create();
        RELEASE_DEVICE();
    }
    ACQUIRE_DEVICE(1000);

    char *driver_name = rg_settings_get_string(NS_GLOBAL, SETTING_DRIVER, "DEFAULT");
    int device = rg_settings_get_number(NS_GLOBAL, SETTING_DEVICE, 0);
    for (size_t i = 0; i < RG_COUNT(sinks); ++i)
    {
        if (strcmp(sinks[i].driver->name, driver_name) == 0 && sinks[i].device == device)
            audio.sink = &sinks[i];
    }
    free(driver_name);

    if (!audio.sink) // Default to first non-dummy if no match found
        audio.sink = &sinks[1 % RG_COUNT(sinks)];

    audio.filter = (int)rg_settings_get_number(NS_GLOBAL, SETTING_FILTER, 0);
    audio.volume = (int)rg_settings_get_number(NS_GLOBAL, SETTING_VOLUME, 50);
    audio.sampleRate = sampleRate;
#if defined(RG_TEST_MUTE) && RG_TEST_MUTE
    /* 2026-10-10 临时钩子（PPA 非阻塞实验，无人值守测试必须静音）。
     * 只改内存状态、**不调 rg_audio_set_volume/settings 写入** ⇒ 不动用户存的音量。
     * 实验跑完把 config.h 里的 RG_TEST_MUTE 改回 0（保留开关本身，与 RG_TEST_NO_AUTOSAVE 同款）。 */
    audio.muted = true;
#endif
    audio.driver = audio.sink->driver;

    if (audio.driver->init(audio.sink->device, sampleRate))
    {
        if (audio.driver->set_mute)
            audio.driver->set_mute(audio.muted);
        if (audio.driver->set_volume)
            audio.driver->set_volume(audio.volume);

        RG_LOGI("Audio ready. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, audio.volume);
    }
    else
    {
        RG_LOGE("Failed to initialize audio. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, audio.volume);
        RG_LOGE(" - Error: %s\n", get_last_driver_error());
        audio.sink = &sinks[0]; // Switching to dummy might allow us to at least boot
        audio.driver = audio.sink->driver;
    }

    RELEASE_DEVICE();
}

void rg_audio_deinit(void)
{
    if (!audio.sink)
        return;

    // We'll go ahead even if we can't acquire the lock...
    ACQUIRE_DEVICE(1000);

    audio.driver->deinit();

    RG_LOGI("Audio terminated. sink='%s'\n", audio.sink->name);

    audio.driver = NULL;
    audio.sink = NULL;

    RELEASE_DEVICE();
}

void rg_audio_submit(const rg_audio_frame_t *frames, size_t count)
{
    const int64_t time_start = rg_system_timer();

    if (!audio.driver)
        return;

    if (!frames || !count)
        return;

    if (ACQUIRE_DEVICE(0))
    {
        audio.driver->submit(frames, count);
        RELEASE_DEVICE();
    }

    counters.totalSamples += count;
    counters.busyTime += rg_system_timer() - time_start;

    /* 2026-09-28 性能盲区仪表：音频忙时占每秒的比例（此前无任何可见读数）。
     * 用自己的窗口计数，不改 counters 的累计语义（别处可能在读）。 */
    {
        static int64_t pf_win = 0;
        static int64_t pf_busy = 0;
        static uint32_t pf_calls = 0;
        int64_t now = rg_system_timer();
        pf_busy += now - time_start;
        pf_calls++;
        if (pf_win == 0) pf_win = now;
        if (now - pf_win >= 1000000) {
            /* 探针已停用（性能排查期临时加入，疑与游戏内改音量崩溃相关） */
    /* RG_LOGI("AUDIO-PF: busy=%d us/秒  calls=%d  (占每秒 %d.%d%%)\n",
                    (int)pf_busy, (int)pf_calls,
                    (int)(pf_busy / 10000), (int)((pf_busy / 1000) % 10)); */
            pf_win = now; pf_busy = 0; pf_calls = 0;
        }
    }
}

rg_audio_counters_t rg_audio_get_counters(void)
{
    return counters;
}

const char *rg_audio_get_driver(void)
{
    if (!audio.driver)
        return NULL;
    return audio.driver->name;
}

const rg_audio_sink_t *rg_audio_get_sinks(size_t *count)
{
    if (count)
        *count = RG_COUNT(sinks);
    return sinks;
}

const rg_audio_sink_t *rg_audio_get_sink(void)
{
    return audio.sink;
}

void rg_audio_set_sink(const char *driver_name, int device)
{
    RG_LOGI("%s %d", driver_name, device);
    rg_settings_set_string(NS_GLOBAL, SETTING_DRIVER, driver_name);
    rg_settings_set_number(NS_GLOBAL, SETTING_DEVICE, device);
    rg_audio_deinit();
    rg_audio_init(audio.sampleRate);
}

int rg_audio_get_volume(void)
{
    return audio.volume;
}

void rg_audio_set_volume(int percent)
{
    /* 音频尚未接入（sink=Dummy / 已 deinit）时，GUI 对话框（rg_gui.c）和退出到启动器
     * 的路径仍会调这三个函数 ⇒ 断言会把「按菜单键退出」变成 panic→重启→自动进游戏的循环。
     * 未就绪时安静忽略：对已就绪的情形行为完全不变（原本断言恒真）。 */
    if (audio.driver == NULL)
    {
        static bool audio_warned;
        if (!audio_warned)
        {
            audio_warned = true;
            RG_LOGW("rg_audio: 未就绪（音频未实现或已 deinit），忽略本次音量/静音/采样率调用\n");
        }
        return;
    }
    audio.volume = RG_MIN(RG_MAX(percent, 0), 100);
    if (audio.driver->set_volume)
        audio.driver->set_volume(audio.volume);
    rg_settings_set_number(NS_GLOBAL, SETTING_VOLUME, audio.volume);
    RG_LOGI("Volume set to %d%%\n", audio.volume);
}

bool rg_audio_get_mute(void)
{
    return audio.muted;
}

void rg_audio_set_mute(bool mute)
{
    /* 音频尚未接入（sink=Dummy / 已 deinit）时，GUI 对话框（rg_gui.c）和退出到启动器
     * 的路径仍会调这三个函数 ⇒ 断言会把「按菜单键退出」变成 panic→重启→自动进游戏的循环。
     * 未就绪时安静忽略：对已就绪的情形行为完全不变（原本断言恒真）。 */
    if (audio.driver == NULL)
    {
        static bool audio_warned;
        if (!audio_warned)
        {
            audio_warned = true;
            RG_LOGW("rg_audio: 未就绪（音频未实现或已 deinit），忽略本次音量/静音/采样率调用\n");
        }
        return;
    }

    if (!ACQUIRE_DEVICE(1000))
        return;

    if (audio.driver->set_mute)
        audio.driver->set_mute(mute);

    audio.muted = mute;
    RELEASE_DEVICE();
}

int rg_audio_get_sample_rate(void)
{
    return audio.sampleRate;
}

void rg_audio_set_sample_rate(int sampleRate)
{
    /* 音频尚未接入（sink=Dummy / 已 deinit）时，GUI 对话框（rg_gui.c）和退出到启动器
     * 的路径仍会调这三个函数 ⇒ 断言会把「按菜单键退出」变成 panic→重启→自动进游戏的循环。
     * 未就绪时安静忽略：对已就绪的情形行为完全不变（原本断言恒真）。 */
    if (audio.driver == NULL)
    {
        static bool audio_warned;
        if (!audio_warned)
        {
            audio_warned = true;
            RG_LOGW("rg_audio: 未就绪（音频未实现或已 deinit），忽略本次音量/静音/采样率调用\n");
        }
        return;
    }

    if (audio.sampleRate == sampleRate)
        return;

    if (audio.driver->set_sample_rate)
    {
        if (ACQUIRE_DEVICE(1000))
        {
            audio.driver->set_sample_rate(sampleRate);
            audio.sampleRate = sampleRate;
            RELEASE_DEVICE();
        }
    }
    else
    {
        rg_audio_deinit();
        rg_audio_init(sampleRate);
    }
}
