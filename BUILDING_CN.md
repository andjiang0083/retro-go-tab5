# 构建指南

[English](BUILDING.md) · **中文**

这里每一条都是在真机 Tab5 上踩出来的。按顺序照做，就不用重新踩一遍。

## 环境要求

- **ESP-IDF v5.5.x**（实测 v5.5.2）。其它版本未经验证；组件布局要求 5.3 以上。
- Python 3.10+、`git`，以及 ESP-IDF 的常规工具链（`install.sh` 会装好）。
- 构建目录需要约 2GB 空闲磁盘。

> **确认你的 ESP-IDF 安装是完整的。** 一个常见故障是机器上还有第二份"半装"的 IDF，
> 它的 `export.sh` 会悄悄选到另一个（坏掉的）Python 环境。如果构建以完全说不通的方式失败，
> 先确认 `IDF_PATH` 与 `which python3`。

## 1. 导出 ESP-IDF

```bash
. /path/to/esp-idf-v5.5/export.sh
echo $IDF_PATH          # 必须指向你的 v5.5 安装
```

## 2. 构建

```bash
cd retro-go-p4
python3 rg_tool.py --target tab5 --no-networking build-img launcher gbsp
```

**⚠ `--no-networking` 是必填的。**

`rg_tool.py` 默认 `RG_ENABLE_NETWORKING=1`。ESP32-P4 没有射频模块，Tab5 的 `sdkconfig` 里
根本没有任何 `CONFIG_ESP_WIFI_*` 符号 —— 于是 `rg_network.c` 编译时报：

```
esp_wifi.h:311: error: 'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM' undeclared
```

更坑的是：**增量构建会掩盖它。** 如果 `rg_network.c.obj` 还是最新的，构建会成功，
错误只在后来被迫全量重编时才冒出来（全新克隆、换了工具链路径、clean 之后）。
日志里出现 `[19/39] … [33/39]` 这种大面积重编，就说明你在做全量构建，潜伏的配置错误会一起暴露。

预期产物 —— 一个合并镜像 + 两个 app：

```
retro-go-p4/build/tab5-retro-go.img        # 刷这个
retro-go-p4/launcher/build/launcher.bin
retro-go-p4/gbsp/build/gbsp.bin
```

## 3. 刷机

```bash
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 921600 \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  --force 0x0 retro-go-p4/build/tab5-retro-go.img
```

- `--flash-mode dio` 与"从 `0x0` 写合并镜像"两者都是必需的；刷错 flash 模式会得到一块开机黑屏的板子。
- `--force` 用于忽略芯片 revision 不匹配的告警（这块板报的 rev 是 esptool 不认识的）。
- 合并镜像会**重置 `otadata`**，所以刷完之后设备启动的是 **launcher**，而不是你上次在玩的游戏。

首次整片刷过之后，只更新 app 会快得多：

```bash
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 921600 write-flash 0x100000 gbsp/build/gbsp.bin
```

## 4. 怎么确认"构建真的成功了"

不要只看"没有报错"。查三样：

1. 构建命令的真实退出码；
2. 产物的 **mtime 与大小**（`ls -l launcher/build/launcher.bin gbsp/build/gbsp.bin`）；
3. 你新加的符号在不在：`riscv32-esp-elf-nm gbsp/build/gbsp.elf | grep <你的符号>`。

## 分区布局约束

两个 app 被刷进固定大小的 OTA 分区：**launcher 960KB、gbsp 704KB**（gbsp 只剩约 47KB 余量）。
不重排分区表就塞进一个大核心，会在链接阶段失败。移植新机种时，请把重做 `partitions.csv` 算进工作量。

## 串口监视的注意事项

**打开 USB-CDC 串口会复位设备。** 这一点绕不过去，意味着：

- 你无法"挂上监视器去接住已经发生的崩溃" —— 复位会把现场冲掉。
- 不要把串口控制台当调试循环。用眼睛验证（屏幕上是啥），或者从 SD 卡读崩溃日志。

retro-go 在 app 死亡时会把 `/crash.log` 写到 SD 卡根目录。有意义的字段：
`Panic configNs`（哪个 app 死的）、`Panic message`、`Panic context`，以及日志输出尾部。拔卡读它。

**关于怎么解读它**：`Application terminated!` 配一条空的 PANIC TRACE 不是异常 ——
那是 retro-go 的"应用无响应看门狗"在杀掉一个主循环停住的 app。
而头部的 `Application:` / `Version:` 是**写入方**的上下文，它可能写着另一个 app、或者是旧固件。
**先看文件的 mtime**，确认这份日志是不是你正在查的那次崩溃。如果**根本没有新的** `crash.log`，
说明故障在软件层之下（总线 / PSRAM 仲裁）—— 断电重启，按硬件级挂死处理。

## 故障对照表

| 现象 | 根因 | 修法 |
|---|---|---|
| `'CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM' undeclared` | 在无射频的 SoC 上开了联网 | 加 `--no-networking` |
| `IDF_PATH is not defined` | 当前 shell 没导出 ESP-IDF | `. /path/to/esp-idf/export.sh` |
| 构建过了但板子开机黑屏 | flash 模式错 / 镜像没合并 | `--flash-mode dio`，从 `0x0` 刷合并镜像 |
| `i2c.h` 新旧驱动冲突 | `esp_lcd` 拉新 I2C 驱动，`driver` 提供旧的 | 已由 `targets/tab5/sdkconfig` 里的 `CONFIG_I2C_SKIP_LEGACY_CONFLICT_CHECK=y` 处理 |
| `MALLOC_CAP_EXEC` 不可用 | P4 不暴露它 | 本移植把它映射为 `MALLOC_CAP_8BIT` |
| `driver/i2s.h` / `driver/adc.h` 找不到 | IDF 5.3+ 已移除 | 移植已改用新驱动路径 |
| 游戏中画面定格、无崩溃日志、只有断电才能恢复 | PSRAM/DSI 带宽仲裁饿死 | 见 README 的性能一节 —— 不要再给显示通路加并发 |

## 关键文件在哪

| 路径 | 是什么 |
|---|---|
| `retro-go-p4/components/retro-go/targets/tab5/` | 板级目标：引脚图、`config.h`、`env.py`、`sdkconfig` |
| `retro-go-p4/components/retro-go/drivers/display/` | DSI 面板初始化 + 转置/推送通路 |
| `retro-go-p4/components/retro-go/drivers/audio/tab5_es8388.c` | ES8388 音频驱动（走 M5Stack BSP） |
| `retro-go-p4/gbsp/` | GBA app：主循环、存档、RISC-V dynarec 后端 |
| `vendor/m5stack_tab5/` | 内置的 Tab5 BSP（构建必需，不从组件仓库拉） |
| `tools/` | 辅助脚本：构建、刷机、抓串口日志、备份 |
