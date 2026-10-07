# 移植面清单：本仓 vs 上游 retro-go

> **本文件由 `tools/port-diff.py` 生成，不要手改** —— 复跑一次就回来了。（手写说明只允许出现在最下面「人工说明」一节，那节不会被覆盖。）

- 生成时间：2026-10-07 18:33 CST
- 上游基准：[`ducalex/retro-go@master`](https://github.com/ducalex/retro-go/tree/master) = `4ced120669750ca7228fd0414211430c1d923166`
- 本仓基准：`retro-go-p4` 子树对象 `793aed6f820c7f9ad40d14322cc3e23240c45fe3`（`git rev-parse HEAD:retro-go-p4`），共 989 个文件
- 这条记录的是**子树对象 hash** 而不是 commit hash：写 commit hash 会在提交那一刻就过期。
- 比对方式：git blob SHA（内容相同 ⇒ SHA 相同），**不下载上游整树**

## 一句话

`retro-go-p4/` 是上游的整树拷贝，其中 **746 个文件逐字节相同**、**70 个被就地修改**、**173 个是移植新增**，另有 **25 个上游文件没有带上**。要 review 这个移植，只需要看后三张表；第一张表之外都是上游代码。

| 类别 | 数量 |
|---|---|
| 与上游逐字节相同（上游代码，勿改） | 746 |
| 上游文件被就地修改 | 70 |
| 本仓新增（上游没有） | 173 |
| 上游有、本仓未带上 | 25 |
| 上游文件总数 | 841 |

## 一、被修改的上游文件（70 个）

这些是**移植真正改到上游代码的地方**，也是将来跟进上游更新时唯一需要看的部分。

### 框架核心（rg_*）（23）

- `components/retro-go/CMakeLists.txt`
- `components/retro-go/README.md`
- `components/retro-go/config.h`
- `components/retro-go/rg_audio.c`
- `components/retro-go/rg_display.c`
- `components/retro-go/rg_display.h`
- `components/retro-go/rg_gui.c`
- `components/retro-go/rg_gui.h`
- `components/retro-go/rg_i2c.c`
- `components/retro-go/rg_input.c`
- `components/retro-go/rg_input.h`
- `components/retro-go/rg_localization.h`
- `components/retro-go/rg_network.c`
- `components/retro-go/rg_network.h`
- `components/retro-go/rg_storage.c`
- `components/retro-go/rg_storage.h`
- `components/retro-go/rg_surface.c`
- `components/retro-go/rg_surface.h`
- `components/retro-go/rg_system.c`
- `components/retro-go/rg_system.h`
- `components/retro-go/rg_utils.c`
- `components/retro-go/rg_utils.h`
- `components/retro-go/translations.h`

### targets / 生成物（sdkconfig，多为噪声）（13）

- `components/retro-go/targets/byteboi-rev1/sdkconfig`
- `components/retro-go/targets/crokpocket/sdkconfig`
- `components/retro-go/targets/esplay-micro/sdkconfig`
- `components/retro-go/targets/fri3d-2024/sdkconfig`
- `components/retro-go/targets/mrgc-g32/sdkconfig`
- `components/retro-go/targets/mrgc-gbm/sdkconfig`
- `components/retro-go/targets/nullnano/sdkconfig`
- `components/retro-go/targets/odroid-go/sdkconfig`
- `components/retro-go/targets/rachel-esp32/sdkconfig`
- `components/retro-go/targets/retro-esp32/sdkconfig`
- `components/retro-go/targets/retro-ruler-V1/sdkconfig`
- `components/retro-go/targets/t-deck-plus/sdkconfig`
- `components/retro-go/targets/vmu/sdkconfig`

### launcher（ROM 前端）（8）

- `launcher/CMakeLists.txt`
- `launcher/main/CMakeLists.txt`
- `launcher/main/applications.c`
- `launcher/main/applications.h`
- `launcher/main/browser.c`
- `launcher/main/gui.c`
- `launcher/main/images.c`
- `launcher/main/main.c`

### 构建 / 工具 / 文档（8）

- `.gitignore`
- `CHANGELOG.md`
- `PORTING.md`
- `base.cmake`
- `retro-go.code-workspace`
- `rg_tool.py`
- `tools/font_converter.py`
- `tools/mkfw.py`

### 其它机种 / 主题（7）

- `prboom-go/components/prboom/CMakeLists.txt`
- `prboom-go/main/main.c`
- `retro-core/main/main_gbc.c`
- `retro-core/main/main_nes.c`
- `retro-core/main/main_sms.c`
- `retro-core/main/main_snes.c`
- `themes/default/background_msx.png`

### targets / 板级配置（config.h / env.py）（5）

- `components/retro-go/targets/byteboi-rev1/config.h`
- `components/retro-go/targets/crokpocket/env.py`
- `components/retro-go/targets/odroid-go/config.h`
- `components/retro-go/targets/retro-ruler-V1/env.py`
- `components/retro-go/targets/sdl2/config.h`

### 框架 drivers（显示/音频/输入驱动）（5）

- `components/retro-go/drivers/audio/buzzer.c`
- `components/retro-go/drivers/audio/i2s.c`
- `components/retro-go/drivers/audio/sdl2.c`
- `components/retro-go/drivers/display/ili9341.h`
- `components/retro-go/drivers/display/sdl2.h`

### 其它（1）

- `.github/workflows/ci.yml`

## 二、新增文件（173 个）

### gbsp/（GBA 核心，本移植独立加入）（121）

- `gbsp/CMakeLists.txt`
- `gbsp/components/gbsp-libretro/.gitignore`
- `gbsp/components/gbsp-libretro/.gitlab-ci.yml`
- `gbsp/components/gbsp-libretro/.travis.yml`
- `gbsp/components/gbsp-libretro/3ds/3ds_cache_utils.S`
- `gbsp/components/gbsp-libretro/3ds/3ds_utils.c`
- `gbsp/components/gbsp-libretro/3ds/3ds_utils.h`
- `gbsp/components/gbsp-libretro/CMakeLists.txt`
- `gbsp/components/gbsp-libretro/COPYING`
- `gbsp/components/gbsp-libretro/Makefile`
- `gbsp/components/gbsp-libretro/Makefile.common`
- `gbsp/components/gbsp-libretro/README.md`
- `gbsp/components/gbsp-libretro/arm/arm64_codegen.h`
- `gbsp/components/gbsp-libretro/arm/arm64_emit.h`
- `gbsp/components/gbsp-libretro/arm/arm64_stub.S`
- `gbsp/components/gbsp-libretro/arm/arm_codegen.h`
- `gbsp/components/gbsp-libretro/arm/arm_dpimacros.h`
- `gbsp/components/gbsp-libretro/arm/arm_emit.h`
- `gbsp/components/gbsp-libretro/arm/arm_stub.S`
- `gbsp/components/gbsp-libretro/bios/Makefile`
- `gbsp/components/gbsp-libretro/bios/README.md`
- `gbsp/components/gbsp-libretro/bios/open_gba_bios.bin`
- `gbsp/components/gbsp-libretro/bios/source/core.s`
- `gbsp/components/gbsp-libretro/bios/source/divide.s`
- `gbsp/components/gbsp-libretro/bios/source/helper.arm.c`
- `gbsp/components/gbsp-libretro/bios/source/logo.c`
- `gbsp/components/gbsp-libretro/bios/source/logo_data.c`
- `gbsp/components/gbsp-libretro/bios/source/logo_data.h`
- `gbsp/components/gbsp-libretro/bios/source/reset.s`
- `gbsp/components/gbsp-libretro/bios/source/softwareinterrupts.c`
- `gbsp/components/gbsp-libretro/bios_data.S_`
- `gbsp/components/gbsp-libretro/cheats.c`
- `gbsp/components/gbsp-libretro/cheats.h`
- `gbsp/components/gbsp-libretro/common.h`
- `gbsp/components/gbsp-libretro/control`
- `gbsp/components/gbsp-libretro/cpu.cpp`
- `gbsp/components/gbsp-libretro/cpu.h`
- `gbsp/components/gbsp-libretro/cpu_instrument.h`
- `gbsp/components/gbsp-libretro/cpu_threaded.c`
- `gbsp/components/gbsp-libretro/gba_cc_lut.c`
- `gbsp/components/gbsp-libretro/gba_cc_lut.h`
- `gbsp/components/gbsp-libretro/gba_memory.c`
- `gbsp/components/gbsp-libretro/gba_memory.h`
- `gbsp/components/gbsp-libretro/gba_over.h`
- `gbsp/components/gbsp-libretro/gbp.c`
- `gbsp/components/gbsp-libretro/gpsp_config.h`
- `gbsp/components/gbsp-libretro/gpsp_memory_alloc.c`
- `gbsp/components/gbsp-libretro/input.c`
- `gbsp/components/gbsp-libretro/input.h`
- `gbsp/components/gbsp-libretro/jni/Android.mk`
- `gbsp/components/gbsp-libretro/jni/Application.mk`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/compat/compat_posix_string.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/compat/compat_strcasestr.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/compat/compat_strl.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/compat/fopen_utf8.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/encodings/encoding_utf.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/file/file_path.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/file/file_path_io.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/boolean.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/compat/fopen_utf8.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/compat/msvc.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/compat/msvc/stdint.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/compat/posix_string.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/compat/strcasestr.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/compat/strl.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/encodings/utf.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/file/file_path.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/libretro.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/retro_assert.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/retro_common_api.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/retro_environment.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/retro_inline.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/retro_miscellaneous.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/streams/file_stream.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/string/stdstring.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/time/rtime.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/vfs/vfs.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/include/vfs/vfs_implementation.h`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/streams/file_stream.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/streams/file_stream_transforms.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/string/stdstring.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/time/rtime.c`
- `gbsp/components/gbsp-libretro/libretro/libretro-common/vfs/vfs_implementation.c`
- `gbsp/components/gbsp-libretro/libretro/libretro.c`
- `gbsp/components/gbsp-libretro/libretro/libretro_core_options.h`
- `gbsp/components/gbsp-libretro/libretro/libretro_core_options_intl.h`
- `gbsp/components/gbsp-libretro/link.T`
- `gbsp/components/gbsp-libretro/main.c`
- `gbsp/components/gbsp-libretro/main.h`
- `gbsp/components/gbsp-libretro/memmap.c`
- `gbsp/components/gbsp-libretro/memmap.h`
- `gbsp/components/gbsp-libretro/mips/mips_codegen.h`
- `gbsp/components/gbsp-libretro/mips/mips_emit.h`
- `gbsp/components/gbsp-libretro/mips/mips_stub.S`
- `gbsp/components/gbsp-libretro/original_readme.txt`
- `gbsp/components/gbsp-libretro/retro_inline.h`
- `gbsp/components/gbsp-libretro/rfu.c`
- `gbsp/components/gbsp-libretro/riscv/riscv_codegen.h`
- `gbsp/components/gbsp-libretro/riscv/riscv_emit.h`
- `gbsp/components/gbsp-libretro/riscv/riscv_stub.S`
- `gbsp/components/gbsp-libretro/savestate.c`
- `gbsp/components/gbsp-libretro/savestate.h`
- `gbsp/components/gbsp-libretro/serial.c`
- `gbsp/components/gbsp-libretro/serial.h`
- `gbsp/components/gbsp-libretro/sound.c`
- `gbsp/components/gbsp-libretro/sound.h`
- `gbsp/components/gbsp-libretro/tests/Makefile`
- `gbsp/components/gbsp-libretro/tests/arm64gen.S`
- `gbsp/components/gbsp-libretro/tests/arm64gen.c`
- `gbsp/components/gbsp-libretro/tests/mipsgen.S`
- `gbsp/components/gbsp-libretro/tests/mipsgen.c`
- `gbsp/components/gbsp-libretro/tools/Makefile`
- `gbsp/components/gbsp-libretro/tools/generate_cc_lut.c`
- `gbsp/components/gbsp-libretro/video.cpp`
- `gbsp/components/gbsp-libretro/video.h`
- `gbsp/components/gbsp-libretro/x86/x86_emit.h`
- `gbsp/components/gbsp-libretro/x86/x86_stub.S`
- `gbsp/dependencies.lock`
- `gbsp/main/CMakeLists.txt`
- `gbsp/main/bios.h`
- `gbsp/main/main.c`

### targets / 板级配置（config.h / env.py）（20）

- `components/retro-go/targets/esp32-p4/config.h`
- `components/retro-go/targets/esp32-p4/docs/ESP32-P4_breadboard_setup.jpg`
- `components/retro-go/targets/esp32-p4/docs/ESP32-P4_devboard.png`
- `components/retro-go/targets/esp32-p4/docs/README.md`
- `components/retro-go/targets/esp32-p4/docs/SPI_ST7789V_2_inches.png`
- `components/retro-go/targets/esp32-p4/env.py`
- `components/retro-go/targets/esp32s3-devkit-c/config.h`
- `components/retro-go/targets/esp32s3-devkit-c/docs/README.md`
- `components/retro-go/targets/esp32s3-devkit-c/docs/device.jpg`
- `components/retro-go/targets/esp32s3-devkit-c/env.py`
- `components/retro-go/targets/esplay-s3/config.h`
- `components/retro-go/targets/esplay-s3/docs/README.md`
- `components/retro-go/targets/esplay-s3/env.py`
- `components/retro-go/targets/qtpy-gamer/config.h`
- `components/retro-go/targets/qtpy-gamer/docs/README.md`
- `components/retro-go/targets/qtpy-gamer/docs/device.jpg`
- `components/retro-go/targets/qtpy-gamer/env.py`
- `components/retro-go/targets/tab5/config.h`
- `components/retro-go/targets/tab5/env.py`
- `components/retro-go/targets/tab5/touch_layout.h`

### 构建 / 工具 / 文档（7）

- `docs/HANDOFF-2026-10-04-gba-name-entry-input.md`
- `docs/HANDOFF-2026-10-06-gba-dynarec-divergence.md`
- `docs/RESOLVED-2026-10-06-gba-input-dynarec.md`
- `tools/build_sdl2_mac.sh`
- `tools/play_sdl2_mac.sh`
- `tools/sdl2-compat/esp_attr.h`
- `tools/sdl2-compat/malloc.h`

### 其它（6）

- `.github/ISSUE_TEMPLATE/bug_report.md`
- `.github/ISSUE_TEMPLATE/feature_request.md`
- `assets/cjk12.bin`
- `components/ppa_engine/CMakeLists.txt`
- `components/ppa_engine/ppa_engine.c`
- `components/ppa_engine/ppa_engine.h`

### 框架核心（rg_*）（6）

- `components/retro-go/rg_cjk.c`
- `components/retro-go/rg_cjk.h`
- `components/retro-go/rg_touch_overlay.c`
- `components/retro-go/rg_touch_overlay.h`
- `components/retro-go/rg_touch_skin.c`
- `components/retro-go/rg_touch_skin.h`

### targets / 生成物（sdkconfig，多为噪声）（5）

- `components/retro-go/targets/esp32-p4/sdkconfig`
- `components/retro-go/targets/esp32s3-devkit-c/sdkconfig`
- `components/retro-go/targets/esplay-s3/sdkconfig`
- `components/retro-go/targets/qtpy-gamer/sdkconfig`
- `components/retro-go/targets/tab5/sdkconfig`

### 框架 drivers（显示/音频/输入驱动）（4）

- `components/retro-go/drivers/audio/tab5_es8388.c`
- `components/retro-go/drivers/display/mipi_dsi_tab5.h`
- `components/retro-go/drivers/display/mipi_dsi_tab5_p.h`
- `components/retro-go/drivers/display/tab5_power.h`

### launcher（ROM 前端）（3）

- `launcher/components/gbsp-core/CMakeLists.txt`
- `launcher/components/retro-core-main/CMakeLists.txt`
- `launcher/dependencies.lock`

### 其它机种 / 主题（1）

- `retro-core/dependencies.lock`

## 三、未带上的上游文件（25 个）

上游有、本仓没有。如果这里出现**非预期**的条目，说明某次同步或清理误删了上游文件。

### targets / 板级配置（config.h / env.py）（17）

- `components/retro-go/targets/brutzelboy/config.h`
- `components/retro-go/targets/brutzelboy/docs/README.md`
- `components/retro-go/targets/brutzelboy/docs/Schematic.pdf`
- `components/retro-go/targets/brutzelboy/docs/back.jpg`
- `components/retro-go/targets/brutzelboy/docs/back_PCB.jpg`
- `components/retro-go/targets/brutzelboy/docs/front.jpg`
- `components/retro-go/targets/brutzelboy/docs/front_PCB.jpg`
- `components/retro-go/targets/brutzelboy/docs/old version.jpg`
- `components/retro-go/targets/brutzelboy/env.py`
- `components/retro-go/targets/esp32-s3-devkit/config.h`
- `components/retro-go/targets/esp32-s3-devkit/docs/README.md`
- `components/retro-go/targets/esp32-s3-devkit/docs/device.jpg`
- `components/retro-go/targets/esp32-s3-devkit/env.py`
- `components/retro-go/targets/redroid-go/config.h`
- `components/retro-go/targets/redroid-go/docs/README.md`
- `components/retro-go/targets/redroid-go/docs/device.png`
- `components/retro-go/targets/redroid-go/env.py`

### targets / 生成物（sdkconfig，多为噪声）（3）

- `components/retro-go/targets/brutzelboy/sdkconfig`
- `components/retro-go/targets/esp32-s3-devkit/sdkconfig`
- `components/retro-go/targets/redroid-go/sdkconfig`

### 其它（2）

- `.github/ISSUE_TEMPLATE/01-bug_report.yml`
- `.github/ISSUE_TEMPLATE/02-feature_request.yml`

### 框架核心（rg_*）（2）

- `components/retro-go/docs/ILI9341.pdf`
- `components/retro-go/docs/ST7789.pdf`

### 其它机种 / 主题（1）

- `themes/default/background_gba.png`

## 四、怎么复跑 / 怎么当门禁

```sh
python3 tools/port-diff.py            # 重新生成本文件
python3 tools/port-diff.py --check    # 与已生成内容比对；不一致 → 退出码 1（可进 CI）
```

## 五、人工说明（手写区，脚本保留）

<!-- 手工说明请写在下面；脚本重新生成时会原样保留这一段 -->

- **为什么有 25 个上游文件没带上**：三个整板 target 目录（`brutzelboy`、`esp32-s3-devkit`、
  `redroid-go`）与它们自带的照片/原理图对这一台设备没用；上游 2 张面板数据手册 PDF 与 GBA 主题背景图同理。
  上游两个 issue 模板被换成了本仓自己的 md 版（不是删，是替换）。
- **哪些"修改"其实不是移植**：`targets/*/sdkconfig` 这类生成物会因为 IDF 版本/默认值差异产生漂移，
  归入「生成物」分组；看移植改动时可以直接跳过。
- **改上游文件的规矩**：能不动上游文件就别动；必须动时，一次只动一处并在 `docs/` 里留结论。
  上游跟进（pull upstream）时按本文件第一节逐条过。
