# Changelog

All notable changes to this port. Everything listed here was verified on real hardware.
See [CHANGELOG_CN.md](CHANGELOG_CN.md) for the Chinese version.

---

## v0.4.9 — Portrait or landscape: one image, both orientations

### New: orientation switching

- **One image supports both portrait and landscape**: the first boot asks which one you want, and you can change
  it any time in `Settings → Screen orientation`. The choice is stored in NVS and the device reboots into it.
- **Orientation is a runtime value, not two separately built firmwares**: the geometry table
  (`targets/tab5/geom.h`), the touch layout tables and the X/Y ↔ L/R swap key coordinates are all resolved at
  runtime for the active orientation. The whole tree was swept — no third place is left that picks a variant at
  compile time.
- **Landscape has its own pad layout**: game centred, D-pad on the left, A/B/X/Y diamond on the right, L/R in the
  top corners, and the **X/Y ↔ L/R swap key centred between L and R**; hit areas are re-laid out for landscape
  and never cover the game.

### Improved

- **Back to a single-app release** (v0.4.8 shipped two apps): the menu and all 11 cores compile into one app, so
  no "single app image only" installer can produce a menu whose games never start. Same trade-off as v0.4.7: the
  in-menu `Check for updates` is gone (it needs a second app partition).
- **Image-shape gate before packaging**: a stale partition layout or incomplete app data is caught before the
  package is produced (`tools/check-single-app-image.py`).
- The flash script falls back to a lower baud / `--no-compress` when the USB-Serial/JTAG handshake fails.

### Docs

- README and the screenshot section now carry a **real-hardware landscape photo** (every label and position in it
  was checked against the firmware's own landscape layout table).
- New `docs/SPEC-0.4.9-SINGLE-APP.md` (design) and `docs/HANDOFF-0.4.9-SINGLE-APP.md` (handoff + on-device
  acceptance record).

## v0.4.7 — multi-core expansion, four touch skins, and per-console controls

The big one: the port stops being "a GBA machine" and becomes an 11-console retro handheld.

### Multi-core, shipped as a single app

- **Eleven consoles wired up and verified on device**: GBA, GB, GBC, NES, SNES, SMS, Game Gear,
  ColecoVision, PC Engine, Lynx, Game & Watch — each with its own core and its own
  per-console window size / zoom.
- **Shipped as one merged single-app image (2 MB)**. The menu and all cores live in the same app;
  switching a core is now a NVS flag + restart rather than jumping to a second app partition.
  This also removes the class of bug where third-party installers (which only extract the *first*
  app partition) left you with a menu and no cores.

### Touch skins — four palettes

- Four switchable control-panel colour palettes (`TouchSkin` in NVS), built on a shared
  "panel geometry" path so the letterbox area is never themed, only the control area.
- **The game screen itself is never themed.** Earlier iterations drew grooves, dividers and
  bezel decoration around the screen; all of that is gone for every console — the screen is the
  screen, the black bars stay black.

### Per-console controls, matched to the real hardware

Controls are decided by **what the real controller has**, not by what the upstream core happened
to map — these are different things, and conflating them produced wrong layouts.

- **GBA**: X/Y/A/B + L/R, plus a swap-XY/LR button (GBA only).
- **SNES**: X/Y/A/B as real buttons + L/R. Upstream's SNES core had borrowed X/Y for START/SELECT
  and used L/R as combinations (a workaround for small handhelds) — a new **"Full" keymap preset**
  is now the default and maps every SNES button to its own key. Existing presets are kept.
- **GB / GBC / NES / SMS / Game Gear**: X/Y act as **turbo** (Y = turbo A, X = turbo B), driven
  from a single source of truth (`rg_input_apply_turbo()`, ~12 Hz).
- **ColecoVision**: no turbo (it has a numeric keypad; the feel is different).
- **SNES never gets turbo** — its X/Y are real buttons.

### Fixed

- **Blue-screen crash when loading a SNES savestate** (`Interrupt wdt timeout on CPU0`, reboot loop).
  The crash was *not* in the emulator core: `update_memory_statistics()` in `rg_system.c` called
  `heap_caps_get_info(SPIRAM)`, which walks the entire 27 MB TLSF pool with interrupts disabled —
  long enough to trip the watchdog. Switched to the O(1) `heap_caps_get_free_size` /
  `heap_caps_get_total_size` APIs. Internal-RAM statistics are unchanged.

### Added

- **First-run guide card on the launcher** (GBA page only): bilingual hint telling a new user that
  **A opens the game list** and which directory ROMs belong in. The path is read from the app's own
  ROM path (not hard-coded), so it follows the selected console.

---

## v0.4.6 — M5Launcher install fix + SD mount fallback

- **"Installed with M5Launcher, then games won't start"** (issue #7). This firmware ships as two
  apps (menu + core in separate partitions), and "is a core installed?" is answered at runtime by
  looking for the core's partition. M5Launcher's installer **only extracts the first app in the
  embedded partition table**, so a full-image install produced a menu with zero cores, zero tabs,
  and a null-pointer crash. Fixes: a zero-tab guard so it can no longer crash, and a self-explaining
  dialog that states the cause and the action instead of showing a blank screen.
  (Upstream note: M5Launcher 2.9.1's Tab5 install path itself occasionally browns out mid-transfer;
  its changelog says 2.10.0 fixes this. Retry, or use M5Burner / esptool.)
- **`SD Card Error / Storage mount failed` (0x107)** (issue #8). The card slot's I/O rail is powered
  from an on-chip LDO channel that this firmware historically never claims; another firmware
  (M5Launcher configures it) can change that hardware state, and it **survives a soft reset**. The
  card ends up in a state where it never answers CMD1 — software cannot revive it.
  Fix: retry once with the vendor's LDO recipe on first mount failure (normal boot path untouched).
  **User-side workaround: reseat the card and do a full power cycle** (a reset is not enough).

---

## v0.4.5 — two GBA porting gaps (tearing, and launcher save slots)

- **Horizontal tearing during fast scrolling** (issue #5). The GBA main loop only ever created
  `updates[0]` and never rotated `currentUpdate`, while the display task reads the buffer
  **asynchronously from another thread** — so the core could start drawing frame N+1 while the
  display task was still reading frame N: old frame on top, new frame below. Fixed by double
  buffering and rotating after submit (matching every other core upstream). v0.4.4 raising the
  frame rate 34.7 → 59.7 made this pre-existing defect visible.
- **Launcher's "continue game" / save slot did nothing** (issue #6). `gbsp` never consumed
  `RG_BOOT_RESUME` (all eight other cores do), so the slot the launcher handed over was dropped.
  Fixed by restoring the three lines after `sram_load()`.

---

## v0.4.4 — GBA dynarec: input not responding

- **Root cause**: the interpreter and the dynarec accounted for memory/fetch cycles with **different
  rules** — the interpreter looked up wait states dynamically (`ws_cyc_nseq` / `ws_cyc_seq`), while
  the dynarec used hard-coded constants. The effect was not "a wrong number" but a **permanent
  one-frame phase offset in the game's own timing** for identical workloads (LZ77 decompression,
  level loading), which shifted input and animation windows. That is why every previous "timing
  patch" had failed.
- Fixed by aligning the dynarec to the interpreter's accounting (runtime wait-state lookup for
  8/16/32-bit accesses, fetch cycles, and removing 13 estimated MUL/MLA charges).
- Measured on device, same ROM and same scripted input over the same 185 s window:
  old dynarec 37.5 fps / broken cursor sequence; **fixed 59.7 fps / correct sequence**;
  interpreter baseline 34.7 fps. Counter-intuitively, charging cycles *more* accurately means
  *fewer* guest instructions per frame, so the frame rate went **up**.

---

## v0.4.3 — "installed via M5Launcher, can't play"

- The CJK font lived in its own flash partition; installers that recreate the data partition lose
  it (Chinese turns into boxes). **Fixed by compiling the 3773-glyph font into the app image**
  (also shrank the total image by 64 KB).
- This firmware is two apps, and that installer only takes the first one — so the core was never
  installed. **Fixed by adding a single-app build form** (`-DRG_SINGLE_APP=1`), where menu and core
  are compiled into one app.
- Repository README added a "where are you installing from → which file" table.

---

## v0.4.2 — display performance

- Display-path work with the three options ranked by cost/benefit, and the display-path
  rework (bounded retry instead of silently dropped frames).

---

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
