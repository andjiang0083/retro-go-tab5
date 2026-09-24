# Credits and attribution

This project is a derivative work. Here is exactly whose work it builds on, and under what terms.

## Upstream projects

| Project | Author / origin | License | How it is used here |
|---|---|---|---|
| [retro-go](https://github.com/ducalex/retro-go) | Alex Duchesne (ducalex) | GPLv2 | The emulator frontend this repository is a port of. The `retro-go-p4/` tree is derived from it. |
| [gpSP](https://github.com/libretro/gpsp) | Exophase, maintained by libretro | GPLv2 | The Game Boy Advance CPU/PPU core in `gbsp/`. |
| [HowBoyAdvance](https://github.com/Irak4t0n/HowBoyAdvance) | Irak4t0n | GPLv2 | The RISC-V dynamic-recompiler backend for gpSP in this port is derived from this ESP32-P4 GBA project. |
| [M5Stack Tab5 BSP](https://github.com/m5stack/M5Stack-IDF) | M5Stack | Apache-2.0 / MIT (see `vendor/m5stack_tab5/LICENSE`) | Board support: display, touch, audio codec, IO expander. Vendored because the port patches its init order. |
| esp_lcd_st7121 | Espressif / M5Stack | see `vendor/esp_lcd_st7121/` | MIPI-DSI panel driver for the Tab5's ST7121 controller. |
| [ESP-IDF](https://github.com/espressif/esp-idf) | Espressif Systems | Apache-2.0 | The SDK, including the MIPI-DSI, PPA/DMA2D, I2S and SDMMC drivers. |

## Bundled assets (from upstream retro-go)

These are carried over unchanged from upstream and keep their original terms:

- `retro-go-p4/retro-core/components/gnuboy/tests/blargg.zip` and
  `retro-go-p4/retro-core/components/nofrendo/docs/nes-test-roms-master.zip` — **homebrew test ROMs**, used for core self-tests.
- `retro-go-p4/prboom-go/components/prboom/data/doom1.wad` — the **shareware** DOOM episode, redistributable under id Software's
  shareware terms. It is not the full game.
- `retro-go-p4/themes/default/source/es-theme-gbz35-master.zip` — a theme source archive.

- `retro-go-p4/gbsp/components/gbsp-libretro/bios/open_gba_bios.bin` — an **open-source GBA BIOS replacement**
  (VBA-M / Normmatt lineage, as used by ReGBA and TempGBA). It is not Nintendo's BIOS: the complete sources it is built from
  ship alongside it in `bios/source/` with a `Makefile`, so it can be rebuilt from scratch. Credit to the original authors.
- `retro-go-p4/components/retro-go/targets/t-deck-plus/docs/t-deck-plus_keyboard_raw_mode.bin` — a small keyboard
  raw-mode capture used as documentation for the T-Deck target. Not a firmware image.

No commercial ROMs are included in this repository, and none should ever be added.

## Vendored registry components

`vendor/managed_components/` contains Espressif Component Registry components vendored into the tree because the project's CMake
lists that directory in `EXTRA_COMPONENT_DIRS` — a clean clone must contain them to build. Each carries its own license file:

| Component | Used for |
|---|---|
| `espressif__esp_codec_dev` | Audio codec abstraction (ES8388 path) |
| `espressif__esp_lcd_touch` | Touch controller abstraction |
| `espressif__esp_lcd_touch_st7123` | Tab5 touch controller driver |
| `espressif__esp_lcd_touch_gt911` | Other touch controllers (shared dependency) |
| `espressif__esp_lcd_st7703` | MIPI-DSI panel driver (shared dependency) |
| `espressif__cmake_utilities` | CMake helpers |
| `espressif__usb_host_hid` | USB HID host support |

## Port-specific work in this repository

Board bring-up (display, touch, IO-expander reset ordering, SD, LEDC backlight), the Tab5 target definition, the ES8388 audio
driver, savestate wiring, the touch virtual gamepad, the RISC-V dynarec integration and stack sizing, and the display-path
measurements documented in the README are the work of this repository's contributors.

If you believe your work is used here without proper attribution, please open an issue — it will be fixed promptly.
