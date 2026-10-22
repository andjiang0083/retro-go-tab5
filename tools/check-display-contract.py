#!/usr/bin/env python3
"""显示驱动契约门禁 —— 守"三份驱动各自把 8 个 lcd_* 都定义了、且都调用了契约自检"。

为什么需要它（而不是只靠源码头里的 _Static_assert）：
  编译期断言只能钉**签名**（类型不符 ⇒ 红）。它**钉不住"有没有定义"** —— 因为
  `rg_display.c` 自己也在文件头声明了同样 8 个 `lcd_*`，只要那个声明在，名字总是"存在"的；
  而某个 `lcd_*` 若没有被任何地方调用（实测 `lcd_sync` 就是这种），驱动里把它改名/删掉
  **照样编得过**。C 语义里没有"零成本 + 能判是否定义"的写法（取地址能判，但会把本可
  优化掉的函数钉进 .data/.text，破坏"改动前后 .obj 逐字节相同"的判据）。
  ⇒ 存在性交给本门禁：机器可判、能变红、跑在 CI 里。

判据（任一不满足 ⇒ 退出码 1）：
  1. 三份驱动文件各自**恰好定义一次**这 8 个符号；
  2. 三份文件都调用了 `RG_DISPLAY_CONTRACT();`（签名钉，别被删掉）。
自检：打印"实际检查了几个符号"，防止"0 项检查也算 PASS"的假绿。

用法：python3 tools/check-display-contract.py [仓库根]
"""
import re
import sys
import pathlib

SYMBOLS = [
    ('lcd_init', 'void(void)'),
    ('lcd_deinit', 'void(void)'),
    ('lcd_sync', 'void(void)'),
    ('lcd_set_rotation', 'void(int)'),
    ('lcd_set_backlight', 'void(float)'),
    ('lcd_set_window', 'void(int,int,int,int)'),
    ('lcd_get_buffer', 'uint16_t *(size_t)'),
    ('lcd_send_buffer', 'void(uint16_t *,size_t)'),
]

DRIVERS = [
    'retro-go-p4/components/retro-go/drivers/display/mipi_dsi_tab5.h',
    'retro-go-p4/components/retro-go/drivers/display/mipi_dsi_tab5_p.h',
    'retro-go-p4/components/retro-go/drivers/display/tab5_dispatch.h',
]

# 定义 = 以 `static [inline] …` 开头、函数名紧跟 `(` 的行（名字必须正好在括号前，
# 否则字符类会把返回类型也吃进去 —— 第一版就栽在这里，捕获到 'void' 的尾字符）
DEF = re.compile(r'^\s*static\s+(?:inline\s+)?[^\n(]*?\b(\w+)\s*\(', re.M)

root = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
fail = []
checked = 0

for rel in DRIVERS:
    p = root / rel
    if not p.exists():
        fail.append(f"{rel}: 文件不存在")
        continue
    text = p.read_text(encoding='utf-8')
    # 收集本文件里所有 "static [inline] 返回类型 名字(" 形式的定义名
    names = re.findall(DEF, text)
    for sym, sig in SYMBOLS:
        checked += 1
        n = names.count(sym)
        if n != 1:
            fail.append(f"{rel}: {sym} 定义 {n} 次（期望恰好 1 次，{sig}）")
    if 'RG_DISPLAY_CONTRACT();' not in text:
        fail.append(f"{rel}: 缺少 RG_DISPLAY_CONTRACT(); 调用（签名钉被删了）")

print(f"检查了 {len(DRIVERS)} 份驱动 × {len(SYMBOLS)} 个符号 = {checked} 项")
if checked != len(DRIVERS) * len(SYMBOLS):
    fail.append("自检失败：检查项数与预期不符")
if fail:
    print("FAIL:")
    for f in fail:
        print("  " + f)
    sys.exit(1)

print("PASS: 三份驱动各自的 8 个 lcd_* 定义齐全，且都挂了签名钉")
