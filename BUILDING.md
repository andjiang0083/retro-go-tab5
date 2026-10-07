# Building

**English** · [中文](BUILDING_CN.md)

Everything here was learned the hard way on a real Tab5. Follow it in order and you will not have to rediscover any of it.

## Requirements

- **ESP-IDF v5.5.x** (tested: v5.5.2). Other versions are untested; 5.3+ is required by the component layout.
- Python 3.10+, `git`, and the usual ESP-IDF toolchain (installed by `install.sh`).
- ~2 GB free disk for the build directory.

> **Make sure your ESP-IDF install is complete.** A common failure mode is having a second, partially-installed IDF on the
> machine whose `export.sh` silently picks a different (broken) Python environment. If the build fails in ways that make no sense,
> verify `IDF_PATH` and `which python3` after sourcing.
>
> Two concrete forms observed in practice:
> - An IDF installed by eim (ESP-IDF Installation Manager) defines `idf.py` only as a shell **alias** in its activation
>   script, so it is not on `PATH` and `rg_tool.py` fails with `No such file or directory: 'idf.py'`. Use
>   `. $IDF_PATH/export.sh` instead.
> - If that venv's `python3.x` is a **dangling symlink** (pointing at an interpreter that has since been removed),
>   `idf.py` fails with `No module named 'click'`. Use the complete IDF install, or recreate that venv.

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

Expected output — a merged image plus both apps (both live under `retro-go-p4/`):

```
retro-go-p4/retro-go_<version>_tab5.img    # flash this; <version> looks like v0.4.7-4-g086bd
retro-go-p4/launcher/build/launcher.bin
retro-go-p4/gbsp/build/gbsp.bin
```

- `rg_tool.py` composes the image name itself: `<project>_<version>_<target>.img` (lowercased); with `--single-app`
  it becomes `..._tab5-single.img`. **Do not copy a fixed filename** — it changes with the version; find the newest with
  `ls -1t retro-go-p4/retro-go_*_tab5.img`.
- `<version>` comes from `git describe --tags --abbrev=5 --dirty --always` (override via the `PROJECT_VER` env var).
  ⚠ In a shallow clone (`git clone --depth 1`) there are no tags and the version degrades to a bare commit hash.

## 3. Flash

```bash
IMG=$(ls -1t retro-go-p4/retro-go_*_tab5.img | head -1)   # newest merged image
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 921600 \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  --force 0x0 "$IMG"
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
3. that a symbol you added is present: `riscv32-esp-elf-nm gbsp/build/gbsp.elf | grep <your_symbol>`;
4. the merged image's **structure anchors** — a successful build does not mean a correct image; the file must be
   bootloader + partition table + app in one:

   ```bash
   IMG=$(ls -1t retro-go-p4/retro-go_*_tab5.img | head -1)
   od -An -tx1 -j8192  -N1 "$IMG"   # expect e9          -> bootloader image magic
   od -An -tx1 -j32768 -N2 "$IMG"   # expect aa 50       -> partition table magic 0x50AA
   od -An -tx1 -j65568 -N4 "$IMG"   # expect 32 54 cd ab -> app descriptor magic
   ```

   (CI runs the same assertions — see below.)

## CI (GitHub Actions)

`.github/workflows/build.yml` runs a **full build from a clean clone** on every push / PR to `main`, and answers
exactly three questions:

1. does it build (`--no-networking`, full build — an incremental build hides config errors; **both the dual-app and
   the single-app form are built**);
2. is the artifact a **real merged image**: the three structure anchors plus a 512 KB floor;
3. does the repository contain any path that is **only valid on one machine**, and is the IDF version 5.5.2.

On point 3 (this is exactly how CI first failed): `*/dependencies.lock` is generated by the component manager and
records the **absolute path of the vendored components on the machine that generated it**
(`/Users/<someone>/.../vendor/managed_components/...`). On that machine the build works fine — the path really is
there, which is why nothing showed up locally — but on any other machine it fails with
`CMake Error: The "path" field in the manifest file ... does not point to a directory`.
Those three lock files are therefore **not tracked** (covered by `.gitignore`; the build regenerates them), and two
CI assertions keep anyone from committing them again.

It does **not** verify frame rate, display path, crashes or timing — none of that is verifiable without hardware
(watch the screen / read `/crash.log` off the SD card). It also does not need network to fetch components (`vendor/` is vendored).

**To confirm the gate really goes red**: on a branch, break one `.c` on purpose (e.g. reference an undeclared identifier)
and open a PR — CI must fail on the compile error.

**Real timings (run #6, no cache at all, both forms built)**: **7 min 49 s** end to end — checkout 51 s (needs the full
history and tags), ESP-IDF install **226 s** (the official action pulls 5.5.2 plus toolchains via EIM — the biggest
chunk), dual-app build 121 s, single-app build 54 s, gates and upload <5 s. The artifact is 4,180,865 B: both merged
images (2293760 / 2097152 B), both app bins, `SHA256SUMS.txt` and both build logs; disk on the runner is a non-issue
(78 GB free).

> `SHA256SUMS.txt` lists **only the two merged images**: the single-app build rebuilds `launcher.bin`/`gbsp.bin`, so by
> upload time those have been overwritten (this run caught exactly that mismatch). The bins are still uploaded for
> debugging, but the manifest only describes files no later step rewrites.

⚠ One environment trap (this is what killed run #2; the workflow's step 2 handles it): the official
`install-esp-idf-action` exports `IDF_PATH` and the toolchains but does **not** put IDF's own script directories on
`PATH` (`$IDF_PATH/tools`, `components/partition_table`, …). And `rg_tool.py` calls `idf.py` / `gen_esp32part.py` /
`esptool.py` / `parttool.py` by **bare name** on non-Windows (see `rg_tool.py:52-57`) ⇒ the symptom is "everything
compiles, then Packing dies with `No such file or directory: 'gen_esp32part.py'`". The local dev tree is fine only
because `tools/idf-env.sh` sources IDF's `export.sh`.

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
| `.github/workflows/build.yml` | CI gate: clean-clone full build + merged-image structure anchors + IDF version consistency |
