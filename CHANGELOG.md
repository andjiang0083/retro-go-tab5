# Changelog

## v0.1.0 — first public release

First public snapshot of the Tab5 (ESP32-P4) port. Everything below was verified on real hardware.

### Emulation

- **GBA (gpSP) runs at full logical speed** — 59-60 fps of emulated time, audio in sync.
- **RISC-V dynamic recompiler** integrated into gpSP (JIT compiling ARM/Thumb to native RISC-V), derived from HowBoyAdvance.
  Roughly 2x faster than the interpreter path; CPU busy time drops to 32-56%.
- Savestates: core-level state (~416 KB) written to and restored from the SD card.

### Platform

- **Board bring-up**: 1280x720 MIPI-DSI panel (ST7121), ST7123 touch, microSD over SDMMC, LEDC backlight.
- **Touch virtual gamepad**: diamond ABXY layout, per-key colors, drawn only inside the letterbox margins so it never covers the game.
- **Audio**: ES8388 codec over I2S at 32 kHz, driven through the M5Stack BSP. No frame-rate cost, no double-throttling.
- **Launcher** renders 1:1 (no scaling artifacts); font scaling applied to the in-game menus.
- Fixed a bring-up hang caused by a touch-init retry storm, and the IO-expander reset ordering that the vendor BSP expects.

### Display path — measured, and one experiment reverted

- Established the baseline: logical 60 fps, **~15 fully-drawn frames/sec**, `BUSY` peaking around 56%.
- The bottleneck is the display task's CPU transpose + PSRAM write-back, not emulation.
- **Queue depth 1 -> 2 plus triple buffering was implemented and measured**: drawn frames doubled (15 -> 30/sec), visibly
  smoother. **Reverted**: on this board it periodically wedges the panel (freeze, no panic log, power cycle required) because
  DPI scan-out at 1280x720@60 Hz already consumes ~106 MB/s of PSRAM bandwidth. See README for the full write-up.
- **PPA/DMA2D hardware scaling was evaluated and is not used**: direct full-screen writes to the DSI framebuffer wedge the panel.
  Any future attempt must be throttled and behind a runtime switch.

### Developer tooling

- SDL2 host build for iterating on application logic without flashing (`tools/build_sdl2_mac.sh`).
- Build/flash/serial-log helper scripts in `tools/`.
- Fixed a host-side timer precision bug (float32 multiplication of a nanosecond counter quantised the frame clock to ~70 ms steps
  after ~7 days of uptime) that made the SDL2 host appear to run at 15 fps.
