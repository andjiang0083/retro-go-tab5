# Tab5 竖屏 retro-go —— 交接文档（2026-09-29）

## 一、当前状态
- **设备在跑 v34 (`g074f4`)**，源码已全部本地提交（分支 `portrait`）。
- 今晚已完成并真机验证：显示占空 35%→7~19%、面板报错清零、`-O3`+dynarec 生效、
  十字键/ABXY 零重叠(±85, 84x84 处处 1px 间隙)、音量菜单崩溃已修、游戏内存档跨重启。
- **未完成三件**（见第四节）。

## 二、开工必备
```bash
cd ~/esp32/retro-go-tab5
. tools/idf-env.sh
cd retro-go-p4 && python3 rg_tool.py --target tab5 --no-networking build-img launcher gbsp
# 刷机：必须【按提交号点名】挑镜像，绝不用 ls -t（并发构建会抢"最新"，曾刷错过一版）
SHA=$(cd .. && git log --oneline -1 --format='%h'); IMG=$(ls retro-go-p4/*_tab5.img | grep -F "$SHA" | head -1)
cd .. && sh tools/flash-retro-go.sh "$IMG"
# 抓日志：用不复位版（复位版会打断用户操作）
sh tools/log-tab5-nr.sh 60 > /tmp/x.log
```
设备端口 `/dev/cu.usbmodem*`（自动探测；插拔后会变号）。SD 卡 `/Volumes/SD/`：**只读，不写不动**。

## 三、纪律（都是踩过坑换来的）
1. **一次只改一个变量**；实验被否**整笔回退**到已验证基线，**不在失败点上叠加第二刀**。
2. **出错/被否必记** → 追加 `~/esp32/ESP32-经验沉淀.md`（§N 格式，现已到 §112）。
3. **遇到别人做过的功能，先读参考实现的注释**，别自造插桩试参数（§110）。
4. **不猜**：能抓 backtrace 就抓（USB 通时）；崩一次胜过十轮猜。
5. 真机前先 PC 重渲染 + 看图确认（`python3 tools/preview-touch-overlay.py` 生成 `docs/touch-overlay-*.png`）。
6. 用户会看报告末尾的「待办建议」并据此批准执行 —— 待办必须写具体（文件/参数/验证方式）。

## 四、待办三件（按此顺序）
### 1) 电量没读通 —— I2C 总线引脚从未定义 ★当前卡点
- 现状：`BATT:0`；日志 `INA226-DBG: rc=0`（读不通），在游戏内也一样。
- **根因已定**：`rg_i2c.c` 用 `RG_GPIO_I2C_SDA/SCL`，而 **`targets/tab5/config.h` 里这两个宏一条都没有**
  → 用了别的板子的默认引脚 → 总线没挂在 INA226 上。
- 硬件契约：Tab5 电池电压由 **INA226 电源监测芯片**给出，**I2C 地址 0x41**，
  **寄存器 0x02 = Bus Voltage（1.25mV/LSB）**（见 `~/esp32/wiki/tab5-sensor-components.md`）。
  板上**有两条 I2C**：面板自己的（ST7123，已工作）+ 内部总线（INA226/扩展器所在，未配）。
- 下一步：读官方 demo `~/esp32/M5Tab5-UserDemo/platforms/tab5/main/hal/hal_esp32.cpp`
  里 `i2c_bus_handle` 的创建处，**取出内部总线的 SDA/SCL 引脚**，写进 `targets/tab5/config.h`。
- 代码已就位：`rg_input.c` 的 `RG_BATTERY_DRIVER == 3` 分支（INA226，含 `INA226-DBG` 原始值日志）；
  `config.h` 里 `RG_BATTERY_CALC_*` 用恒等换算（3.3V=0% ~ 4.2V=100%）。
- 参考项目 `gywan94/tab5-vgbanext` 写的"GPIO53=电池ADC"**与官方 BSP 冲突**（官方 GPIO53 是 I2C SDA），**不要照抄**。

### 2) 竖屏字体过大，launcher 标题显示不全
- 现象：`Nintendo Gameboy Advance` 标题过大被截断；日志有
  `rg_gui_draw_text: Texbox (pos: 1076x80, ...) will be truncated!`（1076 是横屏宽度下的坐标）。
- 根因：`rg_gui.c:226` 的**大屏字体放大倍数按横屏 1280x720 调**，竖屏没跟着改（同一批"横屏残留"）。
- 做法：看该宏当前值，按竖屏 720 宽给一个合适值（或降到 1×）。

### 3) 电量圆灯（用户规格）
- 位置：**START 正上方、居中**（START 中心 (360,1180) → 灯 ≈ 逻辑 (360, 1090)）。
- 样式：**实心小圆**（不要光晕/渐变 —— 用户审美：光晕易显塑料感）。
- 颜色：绿 100–60% / 橙 60–20% / 红 20–10% / **<10% 红闪**；
  **充电中 → 绿闪（优先级最高）**。
- 充电判定要靠 INA226 **电流方向**（分流电阻 5mΩ），**符号约定需真机实测**（插/拔充电器各读一次）。

## 五、已知坑（别再踩）
- **双缓冲**（`35e481c`）→ 菜单往返卡死（换缓冲前没等显示任务用完）。
- **把 swap 烘进调色板 + surface 改 565_BE**（`efdbaaa`）→ 切画面花屏（GUI 仍按旧字节序写同一块 surface）。
- **驱动侧"小推送合并"**（`5ff7048`）→ 算出越界区域（要推 11520 行 vs 屏幕 1280 行）→ 巨型 memcpy 冲垮内存
  → 之后随机崩（`Guru Meditation`，跳到非法地址 `0x4a00xxxx`）。**已整笔回退**。
  → 教训：**凡"合并/攒批"逻辑必须钳制合并后的范围**；"时崩时不崩、与触发点无因果关系"＝内存被写坏。
- PPA 可用，但必须**自管 64B 对齐缓冲 + 显式传 buffer_size + 整帧单次 BLOCKING**。
- SRAM 电池存档**已修好**（三步法：算路径 → 开机只读回 → 运行中检测变化写回；
  **开机阶段绝不碰 SD，建目录推迟到第一次真要写**；主循环 while(1) 没有干净退出路径，
  所以只能"定期检测"而非"退出时保存"）。

## 六、镜像与基线
- 可用基线：`retro-go-p4/retro-go_v0.0.1-28-ge34ac_tab5.img`（撤掉越界合并后的干净版）
- 最新：`v34`（含 INA226 读取尝试，`BATT` 仍 0）
- 发布包：`dist/tab5-portrait-0.0.1-17/`（旧，含已修 bug）；`dist/_BAD-0.0.1-18-.../` 是**已隔离的坏包，勿发**。
