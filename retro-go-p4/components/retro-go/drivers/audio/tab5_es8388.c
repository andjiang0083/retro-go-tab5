/*
 * Tab5 (ESP32-P4) audio driver — ES8388 codec, driven through the official M5Stack BSP.
 *
 * Why a dedicated driver instead of reusing drivers/audio/i2s.c:
 *   - the legacy driver/i2s.h API cannot output MCLK, and this board needs it (GPIO30);
 *   - the BSP already owns the proven chain (I2S std + ES8388 init + amp enable on the
 *     io-expander's SPK_EN), and esp_codec_dev ships Espressif's official ES8388 driver.
 *
 * Pacing: esp_codec_dev_write() blocks on the I2S DMA, and THAT is what paces the app loop
 * here. Do NOT add a busyUntil-style sleep on top of it (the dummy driver uses one, but with a
 * real blocking sink it would halve the frame rate).
 */
#include "rg_system.h"
#include "rg_audio.h"

#if RG_AUDIO_USE_TAB5_CODEC

#include "esp_codec_dev.h"

/* The BSP umbrella header (bsp/m5stack_tab5.h) pulls in lvgl, which retro-go does not build,
 * so declare only what we use — same trick as the display/touch code. */
extern esp_err_t bsp_i2c_init(void);
extern esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);

static esp_codec_dev_handle_t codec = NULL;

static bool driver_init(int device, int sampleRate)
{
    if (bsp_i2c_init() != ESP_OK)
    {
        RG_LOGW("Tab5 audio: bsp_i2c_init failed, staying silent.\n");
        return false;
    }

    codec = bsp_audio_codec_speaker_init(); // I2S std + ES8388 init + SPK_EN, all in the BSP
    if (!codec)
    {
        RG_LOGW("Tab5 audio: ES8388 init failed, staying silent.\n");
        return false;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = (uint32_t)(sampleRate > 0 ? sampleRate : 32768),
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_err_t err = esp_codec_dev_open(codec, &fs);
    if (err != ESP_OK)
    {
        /* Never fatal: a broken audio path must not take the display/emulator down with it. */
        RG_LOGW("Tab5 audio: codec open at %d Hz failed (%s), staying silent.\n", sampleRate, esp_err_to_name(err));
        codec = NULL;
        return false;
    }

    esp_codec_dev_set_out_vol(codec, 70);
    esp_codec_dev_set_out_mute(codec, false);
    RG_LOGI("Tab5 audio: ES8388 ready at %d Hz.\n", sampleRate);
    return true;
}

static bool driver_deinit(void)
{
    if (codec)
    {
        esp_codec_dev_close(codec);
        codec = NULL;
    }
    return true;
}

static bool driver_submit(const rg_audio_frame_t *frames, size_t count)
{
    if (!codec || !count)
        return false;

    /* Blocking write — the I2S DMA paces the emulator loop. */
    esp_err_t err = esp_codec_dev_write(codec, (void *)frames, count * sizeof(rg_audio_frame_t));
    if (err != ESP_OK)
    {
        static int64_t lastWarn = 0;
        if (rg_system_timer() - lastWarn > 1000000)
        {
            lastWarn = rg_system_timer();
            RG_LOGW("Tab5 audio: codec write failed (%s).\n", esp_err_to_name(err));
        }
        return false;
    }
    return true;
}

static bool driver_set_mute(bool mute)
{
    return codec && esp_codec_dev_set_out_mute(codec, mute) == ESP_OK;
}

static bool driver_set_volume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > 100)
        volume = 100;
    return codec && esp_codec_dev_set_out_vol(codec, volume) == ESP_OK;
}

const rg_audio_driver_t rg_audio_driver_tab5_es8388 = {
    .name = "ES8388",
    .init = driver_init,
    .deinit = driver_deinit,
    .submit = driver_submit,
    .set_mute = driver_set_mute,
    .set_volume = driver_set_volume,
};

#endif // RG_AUDIO_USE_TAB5_CODEC
