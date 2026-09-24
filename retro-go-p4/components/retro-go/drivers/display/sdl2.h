#include <SDL2/SDL.h>

#include "rg_touch_overlay.h"

static SDL_Window *window;
static SDL_Surface *surface, *canvas;
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
static SDL_Surface *composed;   /* 画布副本：可视层合成在它上面，避免反复叠加累积 */
#endif
static int win_left, win_top, win_width, win_height, cursor;
static uint16_t lcd_buffer[LCD_BUFFER_LENGTH];

static void lcd_init(void)
{
    window = SDL_CreateWindow("Retro-Go", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, 0);
    surface = SDL_GetWindowSurface(window);
    canvas = SDL_CreateRGBSurfaceWithFormat(0, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, 16, SDL_PIXELFORMAT_RGB565);
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* 宿主预览：按 tab5 的键位表/分辨率把可视层建起来（与真机同一套渲染代码） */
    composed = SDL_CreateRGBSurfaceWithFormat(0, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, 16, SDL_PIXELFORMAT_RGB565);
    rg_overlay_init();
#endif
}

static void lcd_deinit(void)
{
}

static void lcd_set_window(int left, int top, int width, int height)
{
    int right = left + width - 1;
    int bottom = top + height - 1;
    if (left < 0 || top < 0 || right >= RG_SCREEN_WIDTH || bottom >= RG_SCREEN_HEIGHT)
        RG_LOGW("Bad lcd window (x0=%d, y0=%d, x1=%d, y1=%d)\n", left, top, right, bottom);
    win_left = left;
    win_top = top;
    win_width = width;
    win_height = height;
    cursor = 0;
}

static void lcd_set_backlight(float percent)
{
}

static inline uint16_t *lcd_get_buffer(size_t length)
{
    return lcd_buffer;
}

static inline void lcd_send_buffer(uint16_t *buffer, size_t length)
{
    int bpp = canvas->format->BytesPerPixel;
    int pitch = canvas->pitch;
    void *pixels = canvas->pixels;
    for (size_t i = 0; i < length; ++i) {
        int real_top = win_top + (cursor / win_width);
        int real_left = win_left + (cursor % win_width);
        if (real_top >= RG_SCREEN_HEIGHT || real_left >= RG_SCREEN_WIDTH)
            return;
        uint16_t *dst = (void*)pixels + (real_top * pitch) + (real_left * bpp);
        uint16_t pixel = buffer[i];
        *dst = ((pixel & 0xFF) << 8) | ((pixel & 0xFF00) >> 8);;
        cursor++;
    }
}

#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
#include "lodepng.h"

/* ── 宿主截图钩子（仅预览用，真机不编）───────────────────────────────────────
 * RG_SDL2_SHOT=<path.png>[:秒数][:透明度][:按键]  例：
 *   RG_SDL2_SHOT=/tmp/shot.png:3:40:LEFT|A
 * 到点后：套用状态 → 存 PNG → 退出。给"PC 重渲染 + 视觉评审"用，省掉真机往返。 */
static void shot_hook(void)
{
    static bool parsed, pending = true;
    static const char *path; static int delay = 3, alpha = -1;
    static uint32_t keys; static uint32_t t0; static bool hide;

    if (!parsed)
    {
        parsed = true;
        const char *env = getenv("RG_SDL2_SHOT");
        if (!env || !*env)
        {
            pending = false;
            return;
        }
        static char buf[512];
        snprintf(buf, sizeof(buf), "%s", env);
        char *spec = strchr(buf, ':');
        if (spec)
        {
            *spec++ = 0;
            char *tok = strtok(spec, ":");
            if (tok) delay = atoi(tok);
            if ((tok = strtok(NULL, ":"))) alpha = atoi(tok);
            if ((tok = strtok(NULL, ":")))
            {
                char *k = strtok(tok, "|");
                while (k)
                {
                    if (!strcmp(k, "UP")) keys |= RG_KEY_UP;
                    else if (!strcmp(k, "DOWN")) keys |= RG_KEY_DOWN;
                    else if (!strcmp(k, "LEFT")) keys |= RG_KEY_LEFT;
                    else if (!strcmp(k, "RIGHT")) keys |= RG_KEY_RIGHT;
                    else if (!strcmp(k, "A")) keys |= RG_KEY_A;
                    else if (!strcmp(k, "B")) keys |= RG_KEY_B;
                    else if (!strcmp(k, "X")) keys |= RG_KEY_X;
                    else if (!strcmp(k, "Y")) keys |= RG_KEY_Y;
                    else if (!strcmp(k, "L")) keys |= RG_KEY_L;
                    else if (!strcmp(k, "R")) keys |= RG_KEY_R;
                    else if (!strcmp(k, "START")) keys |= RG_KEY_START;
                    else if (!strcmp(k, "SELECT")) keys |= RG_KEY_SELECT;
                    else if (!strcmp(k, "MENU")) keys |= RG_KEY_MENU;
                    else if (!strcmp(k, "HIDE")) hide = true;   /* 预览"按键隐藏"状态（看左上角开关） */
                    k = strtok(NULL, "|");
                }
            }
        }
        path = strdup(buf);
        t0 = SDL_GetTicks();
        RG_LOGI("shot hook: '%s' in %ds alpha=%d keys=0x%X\n", path, delay, alpha, (unsigned)keys);
    }

    if (!pending || !path || SDL_GetTicks() - t0 < (uint32_t)delay * 1000)
        return;

    pending = false;
    if (alpha >= 0)
        rg_overlay_set_alpha(alpha);
    if (hide)
        rg_overlay_set_visible(false);
    if (keys)
        rg_overlay_debug_set_pressed(keys);
    rg_settings_commit();   /* 立刻落盘：这样"重启后是否沿用档位"也能在同一套钩子里验 */

    /* 合成 → 转 RGB888 → PNG */
    SDL_BlitSurface(canvas, NULL, composed, NULL);
    rg_overlay_blit((uint16_t *)composed->pixels, composed->pitch / 2, 0, 0, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT);
    SDL_BlitSurface(composed, NULL, surface, NULL);
    SDL_UpdateWindowSurface(window);

    size_t n = (size_t)RG_SCREEN_WIDTH * RG_SCREEN_HEIGHT;
    unsigned char *rgb = malloc(n * 3);
    const uint16_t *px = (const uint16_t *)composed->pixels;
    for (size_t i = 0; i < n; ++i)
    {
        uint16_t v = px[i];
        rgb[i * 3 + 0] = (unsigned char)(((v >> 11) & 0x1F) * 255 / 31);
        rgb[i * 3 + 1] = (unsigned char)(((v >> 5) & 0x3F) * 255 / 63);
        rgb[i * 3 + 2] = (unsigned char)((v & 0x1F) * 255 / 31);
    }
    unsigned err = lodepng_encode_file(path, rgb, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT, LCT_RGB, 8);
    free(rgb);
    RG_LOGI("shot hook: saved '%s' (err=%u)\n", path, err);
    exit(err ? 2 : 0);
}
#endif

static void lcd_sync(void)
{
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    /* 可视层合成到画布副本上再呈现 —— 合成到 canvas 会在下一帧被重复叠加（越叠越实） */
    SDL_BlitSurface(canvas, NULL, composed, NULL);
    rg_overlay_blit((uint16_t *)composed->pixels, composed->pitch / 2, 0, 0, RG_SCREEN_WIDTH, RG_SCREEN_HEIGHT);
    SDL_BlitSurface(composed, NULL, surface, NULL);
#else
    SDL_BlitSurface(canvas, NULL, surface, NULL);
#endif
    SDL_UpdateWindowSurface(window);
#if defined(RG_GAMEPAD_TOUCH_MAP) && RG_TOUCH_OVERLAY
    shot_hook();
#endif
}

const rg_display_driver_t rg_display_driver_sdl2 = {
    .name = "sdl2",
};
