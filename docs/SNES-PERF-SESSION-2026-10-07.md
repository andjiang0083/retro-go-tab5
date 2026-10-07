# SNES 帧率攻坚记录（2026-10-07）

> **状态：WIP，暂不发布。** 本轮只为定位瓶颈，产物不进入发行版。
>
> - 工作固化在分支 **`perf-probe/snes-2026-10-07`**（commit `27df21e`）
> - 仓库外备份：`~/esp32/snes-perf-backup/snes-perf-probe-2026-10-07.patch`（42KB，可 `git am` 复原）
> - 设备当前刷回的是**发行版 v0.4.7**（`dist/m5burner-0.4.7/retro-go-tab5-0.4.7-merged.bin`）

---

## 一、起点与终点

| | 起点（本轮开工前） | 终点（本轮收尾） |
|---|---|---|
| SNES 帧率 | 约 10 fps（跳帧被顶到上限 5） | **57.8 fps**，≥60fps 的帧占 **23.8%** |
| 帧时间 | 约 40 ms | **17.2 ms** |
| 花屏 | 频繁 | 基本消除（偶发） |
| 声音 | 正常 | 正常（还有轻微欠载） |

**这一轮真正的产出不是某一项优化，而是把"帧时间去哪儿了"这件事彻底量清楚了** ——
在此之前，10 fps 只能靠猜；现在每一段耗时都有实测数字和因果链。

---

## 二、测量体系（下次直接用）

### 屏幕分层探针（4 行，叠在画面顶部视口内）

```
F59 T16.9      F=帧率   T=帧总时间
C11.5 D 0.0    C=S9xMainLoop 核心耗时   D=显示提交
A 5.0 S 0.0    A=音频提交等待   S=系统段
G11.8 X14.9    G=display_task 实测耗时   X=audio_task 实测耗时  ← core1 上的两个任务
```

### 串口日志（同一批数字，每秒一行，**整数微秒**）

```
PERF-PROBE fps=.. total_us=.. core_us=.. audio_us=.. disp_us=.. sys_us=.. dtask_us=.. atask_us=..
```

### 工具（都在 hermes scratch 目录，用 idf 的 python 跑）

```sh
PY="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin/python"
$PY cap-serial.py /dev/cu.usbmodem1101 300 > cap.txt   # 只读不复位（log-tab5.sh 会复位，读不到游戏内数据）
$PY compare-perf.py 前轮.txt 后轮.txt                    # 并排均值/σ/帧率分布
```

### ⚠️ 本轮最重要的教训

**屏幕读数是瞬时值，不能用来下结论。** 本轮中途靠肉眼读数，得出过"队列深度改动净效果约等于零"
的错误判断；后来串口抓满 **302 组**，才发现它其实有效（fps +1.4、A 段 −631µs、≥60fps 占比 +8 个百分点）。
**任何"有/无效果"的结论都必须在 ≥200 组样本上做。**

---

## 三、已确认的因果链（本轮核心资产）

1. **core1 上跑着三个同优先级任务**：`rg_display` / `rg_input` / `snes_audio` —— 同优先级即时间片轮转，谁都不能独占。
2. **`audio_task` 一轮 = 18.5ms**，构成是：
   - `submit` 阻塞等 I2S ≈ **16.7ms**：这是**必然**的，I2S 消费速率恒定，其驱动源码注释写得很直白：
     `/* Blocking write — the I2S DMA paces the emulator loop. */`
   - **`mix_samples`（SNES DSP 混音）≈ 1.8ms** ← **它串在关键路径上，是"净多出来"的一帧**
3. 于是 **audio_task 周期 18.5ms > 帧时间 17.2ms** → 消息队列持续积压 →
   主循环 `rg_task_send` 每帧干等（`A` 段 5.2ms），帧率被钉在 57~58。
4. **同一个原因还造成了音质问题**：每轮欠 1.8ms 的音频量 → I2S 偶尔接不上 → 声音发飘。
   **花屏和声音怪是同一个根因。**
5. `display_task` 实测 ≈ **19ms**，其中缩放 245k 像素 ≈ 6.8ms、逐行哈希 ≈ 3.4ms（已关），其余是等 DSI。

---

## 四、本轮做过的改动

| # | 改动 | 效果 |
|---|---|---|
| 1 | 音频缓冲 `32000/50+1` → `/60+1`（534 样本，对齐 SNES 原生 60Hz） | 解除把帧率锁在 50fps 的节流 |
| 2 | 显式帧率限制器（按 `app->frameTime` 补 `rg_task_delay`） | 消除核心裸奔到 81~99fps 导致的双缓冲覆写花屏 |
| 3 | `audio_task` 优先级 6 → 2 | 显示优先（PCE 核心同款做法，照抄） |
| 4 | partial update 运行时开关，SNES 关闭逐行哈希 | 每帧省约 3.4ms 哈希（SNES 每帧全变，哈希结果永远是"变了"） |
| 5 | **修 `rg_task_create_ex` 漏赋值 `queueLen` 的真 bug** | **上游缺陷**：参数被接收却从未写入结构体 → 所有任务队列深度恒为 1，`rg_task_create_ex` 形同虚设 |

**合计实测**：56.4 → 57.8 fps，`A` 段 5791 → 5160 µs，≥60fps 占比 15.7% → 23.8%。

---

## 五、没走的路（下次从这儿接着干）

| 方案 | 做法 | 把握 / 代价 |
|---|---|---|
| **a. 优化混音** | `S9xMixSamples` 定点化 / 内联化 | 音质无损；工作量最大，收益不保证 |
| **b. 降采样率** | 32000 → 22050 Hz | 混音量减约三成（≈560µs），**数字上刚好补上差的 518µs**；代价是高音有损 |
| c. 收手 | 停在 57.8 fps | 已是起点的 5 倍多 |

**已否决（别再试）**：

- **PPA 硬件缩放/旋转** —— 六轮真机全部 `err=0x102`，见 `docs/TAB5-PORT-STATUS.md`
- **换 snes9x2005 / 2010 内核** —— 新版本更精确 ⇒ 更慢，方向相反；且已 diff 过，核心目录与上游**一字不差**
- **SNES 65c816 dynarec** —— SNES CPU 只占核心耗时约 20%，即使 1.5×（ARM7 的实测比例）收益也仅约 5%；
  且参考 gpSP dynarec 的前车之鉴（`~/esp32/ESP32-经验沉淀.md` §178-181）

---

## 六、怎么恢复工作

```sh
cd ~/esp32/retro-go-tab5
git checkout perf-probe/snes-2026-10-07      # 直接回到探针版
# 或者（从零复原）
git am ~/esp32/snes-perf-backup/snes-perf-probe-2026-10-07.patch
```

所有探针代码都带 `[PERF-PROBE]` 注释，可一次性定位：

```sh
grep -rn "PERF-PROBE" retro-go-p4/ | grep -v build
```

**撤销清单**（本来就不该进发行版的部分）：

- `cpuexec.c` / `spc700.h` 里的 PPU / APU 跳过开关（纯测量用）
- `main_snes.c` 里的探针菜单、滑窗计时、日志输出
- `rg_touch_overlay.h` 的 `RG_OVERLAY_SHOW_FPS` / `RG_OVERLAY_PROBE_LINES`
- `rg_display.c` 的 `rg_display_task_us` 探针

**⚠️ 请务必保留，这是真修复不是探针**：

- `rg_system.c` 里 `task->queueLen = queueLen;` —— 上游 bug 修复，与本次调优无关，**建议提 issue/PR 给上游**
- `main_snes.c` 里的音频缓冲 `/60`、显式帧率限制器
