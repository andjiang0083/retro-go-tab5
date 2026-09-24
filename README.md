# retro-go-tab5

**A port of [retro-go](https://github.com/ducalex/retro-go) to the M5Stack Tab5 (ESP32-P4), with a RISC-V dynamic recompiler for Game Boy Advance.**

The Tab5 is a 1280x720 MIPI-DSI handheld built on the ESP32-P4 — a dual-core RISC-V SoC with 32 MB of PSRAM and no wireless radio at all.
retro-go is a lightweight multi-system emulator frontend. This repository is the port: board bring-up, display path, audio, input,
and a GBA core that JIT-compiles ARM/Thumb into native RISC-V so the CPU-emulation cost stops being the bottleneck.

Built and tested on real hardware. Contributions very welcome — see [Contributing](#contributing).

---

## Status

Honest status, measured on the device (not aspirational):

| Component | State | Notes |
|---|---|---|
| Launcher (ROM browser, menus) | ✅ Working | Touch-driven, 1:1 rendering (no scaling artifacts) |
| GBA core (gpSP) | ✅ Working | Interpreter + **RISC-V dynarec** (JIT), ~2x faster than the interpreter |
| Audio | ✅ Working | ES8388 codec over I2S, 32 kHz, no frame-rate impact |
| Savestates | ✅ Working | Core-level state (~416 KB) written to the SD card |
| Touch virtual gamepad | ✅ Working | Diamond ABXY layout, per-key colors, drawn only in the screen margins |
| Display path | ⚠️ CPU-transpose | ~15 fully-rendered frames/sec at 60 fps logical — see [Performance](#performance) |
| Battery gauge | ❌ Not implemented | `BATT:0` in the status log; the Tab5 has an INA226 |
| Other cores (NES/SNES/MD/PCE/...) | ❌ Not ported | The retro-go tree carries them; only the launcher + GBA are wired for this target |

**Logical speed is full speed**: GBA titles run at 59-60 fps of emulated time with audio in sync.
What is *not* yet at 60 is the number of frames the display path actually pushes to the panel (see below).

---

## Hardware

| | |
|---|---|
| Board | M5Stack Tab5 |
| SoC | ESP32-P4, dual-core RISC-V @ 360 MHz |
| RAM | 32 MB PSRAM + 736 KB internal SRAM |
| Flash | 16 MB |
| Display | 1280x720 MIPI-DSI, ST7123 integrated panel (native portrait 720x1280; the landscape mode is a software rotation) |
| Touch | ST7123, integrated with the panel, I2C 0x55 |
| Audio | ES8388 codec (I2S) |
| Storage | microSD (SDMMC) |

Note: the ESP32-P4 has **no Wi-Fi and no Bluetooth**. Anything network-related in upstream retro-go is compiled out for this target.

---

## Quick start

```bash
# 1. Get ESP-IDF v5.5 and export it (see BUILDING.md for the exact pitfalls)
. ~/esp/esp-idf-v5.5/export.sh

# 2. Build both apps (launcher + GBA) into one flashable image
cd retro-go-p4
python3 rg_tool.py --target tab5 --no-networking build-img launcher gbsp

# 3. Flash
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 921600 \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 build/tab5-retro-go.img
```

Full details, the `--no-networking` trap, and serial-monitor caveats: **[BUILDING.md](BUILDING.md)**.

---

## Controls

The Tab5 has almost no physical buttons, so the gamepad is drawn on the touch screen inside the letterbox margins
(the game viewport is 720x480, leaving room on both sides):

- **D-pad** — left margin
- **A / B / X / Y** — right margin, diamond layout, each key its own color
- **MENU** — open the in-game menu (savestates, options, reset)
- **OPTION** — options menu

Layout reference: [docs/touch-layout-p2.png](docs/touch-layout-p2.png)

---

## Performance

Measured on hardware with the dynarec enabled, Pokémon Emerald:

```
FPS:61 (46+0+15)     BUSY:32-56%
 |    |  |  |
 |    |  |  +-- frames fully drawn
 |    |  +----- partially drawn
 |    +-------- skipped
 +------------- logical fps
```

- **Logical fps: 59-60** — the emulated console runs at full speed, audio in sync, no pitch drift.
- **Drawn fps: ~15** — the display path (CPU transpose + PSRAM write-back) is the ceiling, not the CPU emulation
  (BUSY peaks around 56%, so there is headroom).

The obvious fix — decoupling the emulator from the display with a deeper queue and triple buffering — **was implemented and measured**:
drawn frames doubled from 15 to 30/sec and the picture got visibly smoother. **It was then reverted**: on this board it periodically
wedges the display path (screen freezes, no panic log, power cycle required) because the DPI scan-out (~106 MB/s of continuous PSRAM traffic
at 1280x720@60 Hz) leaves almost no arbitration headroom for a second concurrent writer. That is a property of this panel size, not of the queue design.

**The remaining safe direction is to push *fewer bytes per frame*** — e.g. only submitting the rows the core actually changed.
See [ROADMAP.md](ROADMAP.md).

---

## Repository layout

```
retro-go-p4/            the retro-go tree (upstream source + this port)
  components/retro-go/  framework core: system, display, input, audio, storage, targets/
    targets/tab5/       board target: config.h, env.py, sdkconfig
  launcher/             ROM browser app
  gbsp/                 GBA emulator app (gpSP + RISC-V dynarec)
  rg_tool.py            upstream build/flash driver
vendor/                 vendored third-party components required to build
  m5stack_tab5/         M5Stack Tab5 BSP (vendored copy)
  esp_lcd_st7121/       panel driver
tools/                  build / flash / serial-log helper scripts
docs/                   porting notes (Chinese) + touch layout diagram
```

---

## Porting notes

The development journal for this port — board bring-up findings, hardware facts that were verified the hard way
("do not retry these"), display-architecture notes, and the debugging order — lives in
[docs/TAB5-PORT-STATUS.md](docs/TAB5-PORT-STATUS.md) (written in Chinese).

## Contributing

This project exists because a device with no upstream support got one anyway. If you have a Tab5, an ESP32-P4 board, or just
an interest in RISC-V JIT work, there is plenty to do:

- **[ROADMAP.md](ROADMAP.md)** — the concrete open items, roughly ordered
- **[CONTRIBUTING.md](CONTRIBUTING.md)** — how to build, test, and submit changes
- **Good first issues**: battery gauge (INA226), push-only-changed-rows display path, porting another core, docs and screenshots

A few house rules that come from doing this on real hardware:

1. **One visible change per flash.** Flash, look at the screen, then continue.
2. **No serial-console debugging loops.** On this board, opening the USB-CDC port resets the device — so verification is visual
   (or by reading `/crash.log` off the SD card).
3. **Anything touching the display path or PSRAM bandwidth must ship behind a runtime switch** (an opt-in file on the SD card),
   so a bad experiment can be rolled back without reflashing.

---

## Credits

This port stands on other people's work:

- **[retro-go](https://github.com/ducalex/retro-go)** by Alex Duchesne (ducalex) — the emulator frontend this is a port of. GPLv2.
- **[gpSP](https://github.com/libretro/gpsp)** — the Game Boy Advance core. GPLv2.
- **[HowBoyAdvance](https://github.com/Irak4t0n/HowBoyAdvance)** — the ESP32-P4 GBA project whose RISC-V dynarec this port's
  JIT backend is derived from. GPLv2. Credit for the dynarec approach belongs there.
- **M5Stack** — the Tab5 BSP and hardware documentation.
- **Espressif** — ESP-IDF, and the PPA/DMA2D and MIPI-DSI drivers the display path experiments were built on.

## License

**GPLv2** — see [LICENSE](LICENSE). This is a derivative work of retro-go (GPLv2) and of the HowBoyAdvance dynarec (GPLv2),
so it must remain GPLv2. If you distribute a firmware image built from this repository, you must make the corresponding source available.

ROMs are **not** included and never will be. Bring your own legally obtained dumps.
