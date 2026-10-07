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
>
> 实测到的两种具体形态：
> - eim（ESP-IDF Installation Manager）装的 IDF，其激活脚本只把 `idf.py` 定义成 shell **alias**，它不在 `PATH` 里
>   —— 于是 `rg_tool.py` 直接报 `No such file or directory: 'idf.py'`。改用 `. $IDF_PATH/export.sh`。
> - 如果那个 venv 里的 `python3.x` 是**断链**（指向已被卸掉的解释器），`idf.py` 会报 `No module named 'click'`。
>   换用完整的那份 IDF，或重装该 venv。

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

预期产物 —— 一个合并镜像 + 两个 app（都在 `retro-go-p4/` 下）：

```
retro-go-p4/retro-go_<版本>_tab5.img       # 刷这个；<版本> 形如 v0.4.7-4-g086bd
retro-go-p4/launcher/build/launcher.bin
retro-go-p4/gbsp/build/gbsp.bin
```

- 合并镜像的文件名由 `rg_tool.py` 自己拼：`<项目名>_<版本>_<目标>.img`（小写）；加 `--single-app` 时是 `..._tab5-single.img`。
  **不要照抄某个固定文件名**，它随版本变 —— 用 `ls -1t retro-go-p4/retro-go_*_tab5.img` 找最新的那个。
- `<版本>` 来自 `git describe --tags --abbrev=5 --dirty --always`（可用环境变量 `PROJECT_VER` 覆盖）。
  ⚠ 浅克隆（`git clone --depth 1`）里没有 tag，版本号会退化成裸 commit hash。

## 3. 刷机

```bash
IMG=$(ls -1t retro-go-p4/retro-go_*_tab5.img | head -1)   # 找最新构建出的合并镜像
python3 -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 921600 \
  write-flash --flash-mode dio --flash-size 16MB --flash-freq 80m \
  --force 0x0 "$IMG"
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
3. 你新加的符号在不在：`riscv32-esp-elf-nm gbsp/build/gbsp.elf | grep <你的符号>`；
4. 合并镜像的**结构锚点** —— 构建成功不等于镜像正确，它必须是 bootloader + 分区表 + app 三合一：

   ```bash
   IMG=$(ls -1t retro-go-p4/retro-go_*_tab5.img | head -1)
   od -An -tx1 -j8192  -N1 "$IMG"   # 期望 e9          → bootloader 镜像魔数
   od -An -tx1 -j32768 -N2 "$IMG"   # 期望 aa 50       → 分区表魔数 0x50AA
   od -An -tx1 -j65568 -N4 "$IMG"   # 期望 32 54 cd ab → app 描述符魔数
   ```

   （CI 里跑的是同一套断言，见下节。）

## CI（GitHub Actions）

`.github/workflows/build.yml` 在 push / PR 到 `main` 时跑一次**干净克隆的全量构建**，只回答三个问题：

1. 能不能构建（`--no-networking`，全量 —— 增量构建会掩盖配置错误；**双 app 与单 app 两种形态都建**）；
2. 产物是不是**真的合并镜像**：三个结构锚点 + 512KB 下限；
3. 仓库里**有没有「只在一台机器上成立」的路径**，以及用的 IDF 是不是 5.5.2。

关于第 3 条（CI 首跑就是这么红的）：`*/dependencies.lock` 是组件管理器生成的文件，里面记的是
**生成时那台机器的绝对路径**（`/Users/<某人>/.../vendor/managed_components/...`）。这种文件在生成它的机器上
构建毫无问题 —— 那个路径真的存在，所以本地一直没暴露 —— 但换台机器就会
`CMake Error: The "path" field in the manifest file ... does not point to a directory`。
因此这三个 lock **不跟踪**（`.gitignore` 覆盖，构建时自动重建）；CI 里两条断言负责防止它再被提交回来。

它**不能**验证帧率、显示通路、崩溃、时序 —— 没有硬件无从验证（看屏幕 / 读 SD 卡 `/crash.log`）。
它也不需要联网装组件（`vendor/` 已入库）。

**要确认门禁真的会红**：开一个分支故意改坏一个 `.c`（比如引一个不存在的标识符）提 PR，CI 必须红在编译错误上。

**首次真实耗时（run #6，无任何缓存，两种形态都建）**：整轮 **7 分 49 秒** —— checkout 51s（必须带全历史与 tag）、
装 ESP-IDF **226s**（官方 action 用 EIM 拉 5.5.2 + 工具链，是最大的一块）、双 app 构建 121s、单 app 构建 54s、
门禁与上传 <5s。
⚠ 会**浮动**：三次绿灯实测 7 分 49 秒 / 9 分 38 秒（同一份代码，IDF 安装 226~263s、双 app 构建 121~204s、
checkout 5~55s 都随 runner 变）—— 别把上面那组数字当 SLA，当"量级"看；要提速就该缓存 IDF 工具链（占一半以上）。
产物 artifact 4,180,865 B —— 两个合并镜像 2293760 / 2097152 B、两个 app bin、`SHA256SUMS.txt`（只列两个镜像，见下）与两份构建日志；runner 磁盘无压力（78 GB 可用）。

> `SHA256SUMS.txt` **只列两个合并镜像**：单 app 构建会重建 `launcher.bin`/`gbsp.bin`，上传时它们已被覆盖（本轮实测抓到过一次清单与文件对不上）。bin 仍然上传供排查，但清单只描述不会被后续步骤改写的文件。

⚠ 一个环境坑（run #2 就是死在这里，workflow 第 2 步已处理）：官方 `install-esp-idf-action` 只导出
`IDF_PATH` 与工具链，**不把 IDF 自己的脚本目录铺进 PATH**（`$IDF_PATH/tools`、`components/partition_table`…）。
而 `rg_tool.py` 在非 Windows 上按**裸命令名**调用 `idf.py` / `gen_esp32part.py` / `esptool.py` / `parttool.py`
（见 `rg_tool.py:52-57`）⇒ 症状是"编译全过、Packing 阶段才炸：`No such file or directory: 'gen_esp32part.py'`"。
本地开发树之所以没事，是因为 `tools/idf-env.sh` 会 source IDF 的 `export.sh`。

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
| `.github/workflows/build.yml` | CI 门禁：干净克隆全量构建 + 合并镜像结构锚点 + IDF 版本一致性 |
