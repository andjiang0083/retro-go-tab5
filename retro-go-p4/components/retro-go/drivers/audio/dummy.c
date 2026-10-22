#include "rg_system.h"
#include "rg_audio.h"

static int64_t busyUntil = 0;

static bool driver_init(int device, int sampleRate)
{
    busyUntil = 0;
    return true;
}

static bool driver_deinit(void)
{
    return true;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    /* 本 sink 靠 sleep 模拟真实播放节奏。一旦采样率不可用（0/负），
     * 1000000.f/rate = inf ⇒ busyUntil 变无穷 ⇒ 下一次提交 rg_usleep(巨大值) ⇒
     * 核心永久睡死（实测症状：游戏首次产出音频的瞬间画面定格、BUSY 变 0%、
     * 无崩溃无复位）。这里三重护栏：rate 无效不等待、单次等待夹 20ms、时长夹 20ms。 */
    const int rate = rg_audio_get_sample_rate();
    if (rate <= 0)
    {
        static bool warned;
        if (!warned) { warned = true; RG_LOGW("audio/dummy: 采样率无效(%d)，本次不做节奏等待\n", rate); }
        return true;
    }

    int64_t now = rg_system_timer();
    if (busyUntil > now)
    {
        int64_t wait = busyUntil - now;
        if (wait > 20000) wait = 20000;
        rg_usleep((uint32_t)wait);
    }

    int64_t dur = (int64_t)count * 1000000 / rate;
    if (dur < 0 || dur > 20000) dur = 20000;
    busyUntil = rg_system_timer() + dur;
    return true;
}

const rg_audio_driver_t rg_audio_driver_dummy = {
    .name = "dummy",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
};
