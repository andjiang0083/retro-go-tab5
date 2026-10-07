# 竖屏虚拟手柄移植模板（Tab5 竖屏分支的经验固化）

> 目的：**换机型 / 换模拟器核时不用重做触摸层**。触摸层本身与机型无关，
> 机型差异全部集中在一个数据文件里；照本文填一遍即可。

## 1. 三层结构（谁消费什么）

| 文件 | 角色 | 说明 |
|---|---|---|
| `targets/<target>/touch_layout.h` | **唯一数据源** | 键位表 `RG_TAB5_TOUCH_MAP` + 调换按钮几何 + 调换规则宏 |
| `rg_touch_overlay.c/.h` | 可视层 + 状态 | 渲染按键/标签/高亮、脏矩形、调换（备用掩码） |
| `rg_input.c` | 命中判定 | 把触点转成 `RG_KEY_*` 注入（与可视层共用 `rg_overlay_map_key()`） |
| `tools/preview-touch-overlay.py` | PC 预览 | 从固件源码直接解析规则（不抄第二份），并断言自反性 |

**铁律：规则只能有一处。** 曾因"可视层和输入层各写一遍调换"而出现默认态按 X 亮 R
（§124）。任何"同一个映射的第二个实现"都是 bug 温床。

## 2. 起步：复制模板文件

```bash
cp targets/tab5/touch_layout.h targets/<new-target>/touch_layout.h
```

只改这几处：

```c
#define RG_TAB5_TOUCH_MAP { /* 每个键：{RG_KEY_x, 中心x, 中心y, 宽, 高} */ }
#define RG_TAB5_SWAP_X / _Y / _W / _H     /* L/R 之间那颗调换按钮 */
```

其余文件不需要动（可选：把 `RG_TAB5_*` 前缀换成自己的目标名，纯属可读性）。

## 3. 布局规矩（都是真机实测换来的）

1. **(x, y) 是命中区的中心**，不是左上角。命中层和可视层都用 `-w/2, -h/2` 展开
   （曾因两边约定不一致导致"高亮位置对、判定位置偏"）。
2. **零重叠**。上下键与左右键的中心距必须 ≥ 键宽（Tab5：84 宽 → 中心距 85，
   留 1px 间隙）。gywan94/tab5-vgbanext 的 odroid_vpad.c 是**故意重叠**的
   （注释：`D-pad zones overlap so a single finger near a corner triggers a diagonal`），
   照抄它就会"按上键串出下键"。
3. **不压游戏画面**。游戏视口在顶部 `y = 0..480`（3x 整数缩放 720×480），
   控制区全部落在 `y >= 500`；最上一排（肩键/调换键）中心 y = 545。
4. 手指尺寸参考：方向键/动作键 **84×84**，肩键 **180×84**，系统键 **150×76**。
   菱形排布 = 中心 ±85（上/下/左/右四个 84×84 矩形，正好留 1px 缝）。
5. 调换按钮放在 L/R 之间（Tab5：L 占 x 50–230、R 占 490–670，中间取 180 宽居中 = 360），
   命中矩形额外外扩 `RG_OVERLAY_SWAP_PAD`（10px），手指好点。

## 4. 调换机制（X/Y ↔ L/R）的三处必要件

1. **规则一处**：`rg_touch_swap_key()`（自反：换两次回原样）。
2. **唯一状态消费点**：`rg_overlay_map_key()`（内部按 `swap_yx` 门控）。
   输入层与可视层**都**调它 —— 结构上不可能再出现"两边不一致"。
3. **备用掩码**（点一下立刻变，不重建）：受影响的只有 X/Y/L/R 四个位置 + 那颗按钮
   （集合在调换下闭合）。这套掩码**延迟构建**（首次点调换时才建，约 99ms，
   省下开机时间；见 `rg_overlay_ensure_variants()`）。

## 5. 移植后必须跑的检查

```bash
python3 tools/preview-touch-overlay.py            # 默认态
python3 tools/preview-touch-overlay.py --swap     # 调换态（含自反性断言）
```
- 预览工具**从固件源码解析**几何与规则，所以它红了就是固件真的不一致。
- 真机三步（缺一不可）：① 列表里点几下（不花屏）→ ② 进游戏（控制条带在推帧循环
  覆盖不到的区域，必须能自己刷新）→ ③ 按住方向键看高亮是否跟随、且**位置与功能一致**。
- 开机日志三行体检（默认级别就能看到）：
  `touch overlay ready: N buttons, visible=1, ...`、
  `touch overlay: X/Y <-> L/R swapped = N (gen M)`、
  点调换时 `touch overlay: swap variants built: 4/4 key alts, switch btn alt=1`。

## 6. 已知坑（详见 `docs/archive/PORTRAIT-TAB5-HANDOFF.md` 与 `~/esp32/ESP32-经验沉淀.md`）

- **不在输入任务里调 `rg_display_force_redraw()`**：它会同步派发 `RG_EVENT_REDRAW`
  → 启动器 `gui_redraw()` 在输入线程里重画 → 抢 `gui.surface` → 花屏（§122）。
  刷新只记录脏矩形，交给显示线程消费。
- **控制条带在游戏视口之外**（GBA 视口 720×480），推帧循环 `y < draw_height` 到不了 →
  必须由 `rg_overlay_take_dirty_rects()` 报告脏矩形、显示层用条带背景重建（§123）。
- **抬手复位**：`esp_lcd_touch_get_coordinates()` 无触点时返回 false，复位块不能写在
  `if (read_ok)` 里面（否则松手不复位）（§120）。
- **自反 ≠ 恒等**：带状态的映射，规则和状态的消费点都要收敛到一处（§124）。
