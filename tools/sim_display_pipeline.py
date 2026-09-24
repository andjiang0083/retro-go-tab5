#!/usr/bin/env python3
"""显示流水线仿真（真机前先跑长模拟）

目的：不上真机就判断「队列深度」「DMA2D 忙时丢弃 vs 有界重试」哪个方向对，
以及为什么"深度 2 会楔死"。

模型结构（对应真实代码路径）：
  模拟器(main) --rg_display_sync--> rg_task_send(queue, portMAX_DELAY)   ← 队列满则阻塞
  显示任务: 出队 -> 每块: CPU 转置 -> tab5_draw -> DMA2D(单通道串行)异步拷贝
  DMA2D: 忙时 IDF 用 0 超时抢信号量失败 => 本次绘制被**丢弃**（不是重试）
  被丢弃的块：若脏区校验和已提前更新 → 本帧该块内容永远不显示（屏幕留空洞），
              下一帧也不会重推；这正是"丢帧"比"慢"更糟的地方。

标定基准（2026-09-25 夜真机实测）：推送 ~313 次/秒、丢弃 ~317 次/秒、显示 ~15 帧/秒
"""
US = 1_000_000
STEP_US = 20
SIM_US = 3 * US

# ---- 标定参数（由观测量反推）----
EMU_COMPUTE_US = 16_000   # 模拟器每帧计算
PUSH_CPU_US    = 800      # 每次推送 CPU 侧：转置 + 提交（实测 0.6~1.0ms）
DMA2D_JOB_US   = 1_400    # DMA2D 搬一个块的占用时间（>推送成本 → 约一半被丢）
CHUNKS_PER_UPD = 21       # 每次更新的脏块数（313/s ÷ 15fps ≈ 21）


class Sim:
    def __init__(self, depth=1, retry_max=0, retry_wait_us=200):
        self.depth = depth
        self.retry_max = retry_max
        self.retry_wait_us = retry_wait_us
        self.queue = []
        self.emu_free_at = 0
        self.emu_state = "compute"     # compute -> submit（提交被阻塞时停在这里）
        self.disp_free_at = 0
        self.dma2d_free_at = 0
        self.pending = 0
        self.pending_dropped = 0       # 本更新里被丢掉、需要下一帧重推的块
        self.carry = 0                 # 上帧丢下的块，本帧补推
        self.pushes = 0
        self.drops = 0
        self.frames_shown = 0          # 只有"整帧全部推成功"才算显示出来
        self.updates = 0
        self.emu_stall_us = 0
        self.cpu_us = 0

    def emu_tick(self, t):
        if self.emu_state == "compute":
            if t < self.emu_free_at:
                return
            self.emu_free_at = t + EMU_COMPUTE_US
            self.cpu_us += EMU_COMPUTE_US
            self.emu_state = "submit"
        if self.emu_state == "submit":
            if len(self.queue) >= self.depth:
                self.emu_stall_us += STEP_US      # portMAX_DELAY：主线程真卡住
                return
            self.queue.append(CHUNKS_PER_UPD + self.carry)
            self.carry = 0
            self.emu_state = "compute"

    def disp_tick(self, t):
        if t < self.disp_free_at:
            return
        if self.pending == 0:
            if not self.queue:
                return
            self.pending = self.queue.pop(0)
            self.pending_dropped = 0
            self.updates += 1
        self.pushes += 1
        self.cpu_us += PUSH_CPU_US
        t_draw = t
        if t < self.dma2d_free_at:
            tries = 0
            ok = False
            while tries < self.retry_max:
                tries += 1
                self.cpu_us += self.retry_wait_us
                if t + tries * self.retry_wait_us >= self.dma2d_free_at:
                    ok = True
                    break
            if not ok:
                self.drops += 1
                self.pending_dropped += 1
                self.disp_free_at = t + PUSH_CPU_US
                self.pending -= 1
                if self.pending == 0:
                    self.carry += self.pending_dropped
                return
            t_draw = t + tries * self.retry_wait_us
        self.dma2d_free_at = t_draw + DMA2D_JOB_US
        self.disp_free_at = t_draw + PUSH_CPU_US
        self.pending -= 1
        if self.pending == 0:
            self.carry += self.pending_dropped
            if self.pending_dropped == 0:
                self.frames_shown += 1


def run(depth, retry_max, label, quiet=False):
    s = Sim(depth=depth, retry_max=retry_max)
    t = 0
    while t < SIM_US:
        s.emu_tick(t)
        s.disp_tick(t)
        t += STEP_US
    secs = SIM_US / US
    if not quiet:
        delivered = s.pushes - s.drops
        rate = 100.0 * delivered / s.pushes if s.pushes else 0.0
        print(f"{label:30s} 更新={s.updates/secs:5.1f}/s  块送达率={rate:5.1f}%  "
              f"丢块={s.drops/secs:6.0f}/s  主线程被卡={100*s.emu_stall_us/SIM_US:5.1f}%")
    return s


if __name__ == "__main__":
    print("显示流水线仿真（3 秒 × 20µs 步长）")
    print("⚠ 绝对数值未经真机打点标定（参数为量级估计），**有效的是各方案之间的相对趋势**。")
    print("   要拿真数字，须刷 dist/retro-go-p2.6.6-axi-icm.img 看三段打点。\n")
    print("指标口径：更新/s = 显示任务完成一次同步更新的频率（用户看到的画面刷新率）；")
    print("          块送达率 = 推成功的块 ÷ 尝试推的块（没送达的块会留空洞）；")
    print("          主线程被卡 = 模拟器线程被 portMAX_DELAY 阻塞的占比（手感卡顿）。\n")
    run(1, 0, "① 现状：深度1 + 忙则丢弃")
    run(1, 5, "② 只加有界重试(深度仍1)")
    run(2, 0, "③ 只加深度2（仍丢弃）")
    run(2, 5, "④ 深度2 + 有界重试")
    run(3, 5, "⑤ 深度3 + 有界重试")
    run(4, 8, "⑥ 深度4 + 重试8次")
    print("\n结论（相对趋势）：")
    print("  · 只加深度：块送达率不变、主线程被卡不变 —— 深度只让模拟器'跑在前面'，")
    print("    并不能让屏幕更快，只是让 fps 计数器和同步调用看起来更快（当年'深度2=30fps'的来源）")
    print("  · 加有界重试：块送达率升到 ~100%（不再留空洞），代价是显示任务被串行化")
    print("  · 真正要降的是'每次推送的成本'与'主线程被卡占比' —— 对应 AXI-ICM 提权与降低转置成本")
