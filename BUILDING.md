# Building

Everything here was learned the hard way on a real Tab5. Follow it in order and you will not have to rediscover any of it.

## Requirements

- **ESP-IDF v5.5.x** (tested: v5.5.2). Other versions are untested; 5.3+ is required by the component layout.
- Python 3.10+, `git`, and the usual ESP-IDF toolchain (installed by `install.sh`).
- ~2 GB free disk for the build directory.

> **Make sure your ESP-IDF install is complete.** A common failure mode is having a second, partially-installed IDF on the
> machine whose `export.sh` silently picks a different (broken) Python environment. If the build fails in ways that make no sense,
> verify `IDF_PATH` and `which python3` after sourcing.

## 1. Export ESP-IDF

```bash
. /path/to/esp-idf-v5.5/export.sh
echo $IDF_PATH          # must point at your v5.5 install
```

## 2. Build

```bash
cd retro-go-p4
python3 rg_tool.py --target tab5 --no-networking build-img launcher gbsp
```

**⚠ The `--no-networking` flag is mandatory.**

`rg_tool.py` defaults to `RG_ENABLE_NETWORKING=1`. The ESP32-P4 has no radio, so the Tab5 `sdkconfig` contains no
`CONFIG_ESP_WIFI_*` symbols at all — and `rg_network.c` then fails to compile with:

```
esp_wifi.h:311: error: 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM' undeclared
```

Worse: **an incremental build hides this.** If `rg_network.c.obj` is still up to date, the build succeeds, and the error only
appears later when something forces a full rebuild (a fresh clone, a different toolchain path, a clean). If you see a big
`[19/39] ... [33/39]`-style recompile in the log, you are doing a full build and any latent config error will surface.

Expected output — a merged image plus both apps:

```
retro-go-p4/build/tab5-retro-go.img        # flash this
retro-go-p4/launcher/build/launcher.bin
retro-go-p4/gbsp/build/gbsp.bin
```

## 3. Flash

```bash
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 921600 \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  --force 0x0 retro-go-p4/build/tab5-retro-go.img
```

- `--flash-mode dio` and the merged image at `0x0` are both required; a wrong flash mode produces a board that boots to a blank screen.
- `--force` silences the chip-revision mismatch warning (this board reports a rev the esptool build does not know).
- The merged image **resets `otadata`**, so after flashing, the device boots the **launcher** — not the game you were in.

App-only update (faster, after the first full flash):

```bash
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 921600 write-flash 0x100000 gbsp/build/gbsp.bin
```

## 4. Verifying a build actually worked

Do not trust the absence of errors. Check:

1. the real exit code of the build command,
2. the artifact **mtime and size** (`ls -l launcher/build/launcher.bin gbsp/build/gbsp.bin`),
3. that a symbol you added is present: `riscv32-esp-elf-nm gbsp/build/gbsp.elf | grep <your_symbol>`.

## Partition layout constraint

The apps are flashed into fixed-size OTA partitions: **launcher 960 KB, gbsp 704 KB** (gbsp has roughly 47 KB of headroom).
Adding a large emulator core without touching the partition table will fail at link time. If you are porting another core,
expect to rework `partitions.csv` as part of the job.

## Serial monitor caveat

**Opening the USB-CDC serial port resets the device.** There is no way around it, and it means:

- You cannot attach a monitor to catch a crash that already happened — the reset wipes it.
- Do not use the serial console as a debug loop. Verify visually (what is on the screen), or read the crash log off the SD card.

retro-go writes `/crash.log` to the SD-card root when an app dies. Fields that matter: `Panic configNs` (which app died),
`Panic message`, `Panic context`, and the tail of the log output. Pull the card and read it.

**A note on interpreting it**: `Application terminated!` with an empty PANIC TRACE is not an exception — it is retro-go's
unresponsive-app watchdog killing an app whose main loop stopped. And the `Application:` / `Version:` header lines are the
*writer's* context, so they can name a different app or an older firmware. **Check the file's mtime first** to know whether
the log is even from the crash you are looking at. If there is **no new** `crash.log` at all, the failure is below the software
layer (bus/PSRAM arbitration) — power cycle and treat it as a hardware-level hang.

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM' undeclared` | networking enabled on a radio-less SoC | pass `--no-networking` |
| `IDF_PATH is not defined` | ESP-IDF not exported in this shell | `. /path/to/esp-idf/export.sh` |
| Build works, board boots blank | wrong flash mode / image not merged | `--flash-mode dio`, flash the merged image at `0x0` |
| `i2c.h` legacy/new driver conflict | `esp_lcd` pulls the new I2C driver, `driver` provides the old one | already handled by `CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK=y` in `targets/tab5/sdkconfig` |
| `MALLOC_CAP_EXEC` not available | P4 does not expose it | the port maps it to `MALLOC_CAP_8BIT` |
| `driver/i2s.h` / `driver/adc.h` missing | removed in IDF 5.3+ | port uses the new driver paths |
| Display freezes mid-game, no crash log, only a power cycle helps | PSRAM/DSI bandwidth arbitration starvation | see the performance section in README.md — do not add display-path concurrency |

## Where things live

| Path | What |
|---|---|
| `retro-go-p4/components/retro-go/targets/tab5/` | board target: pin map, `config.h`, `env.py`, `sdkconfig` |
| `retro-go-p4/components/retro-go/drivers/display/` | DSI panel init + the transpose/push path |
| `retro-go-p4/components/retro-go/drivers/audio/tab5_es8388.c` | ES8388 audio driver (via the M5Stack BSP) |
| `retro-go-p4/gbsp/` | GBA app: main loop, savestates, the RISC-V dynarec backend |
| `vendor/m5stack_tab5/` | vendored Tab5 BSP (required; not fetched from the registry) |
| `tools/` | helper scripts: build, flash, serial log, backup |
