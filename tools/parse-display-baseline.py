#!/usr/bin/env python3
"""P2-1 基线数据解析：从真机日志里算「每帧平均脏行数」与「推屏 fps」。

数据源（都是固件自己打的，不改一行日志代码）：
  DIAG_FB f=<帧号> game=<hash> push=<hash> n=<累计推送块数> drops=<累计丢块> dirty=<累计判脏行数>
  PERF: display=<x>.<xx>ms/<win>ms (transpose=.. submit=..) blocks=<n> [xpose=.. ovl=.. draw=..] rows=<avg>.<d> max=<mx>
用法：python3 tools/parse-display-baseline.py <日志文件> [--first N]

  --first N  只取前 N 个测量窗口（N 个 DIAG_FB + N 个 PERF 行）再算 —— A/B 对比时必须用
             同一 N，否则两边窗口长度不同，结论不可比（2026-10-08 踩过：一边 199 秒、一边 202 秒）。
              例：A 组完整窗口 = 213，故 A/B 都取 --first 213（帧区间会正好一致：f=1→12721）。
"""
import re
import sys

diag_re = re.compile(r"DIAG_FB f=(\d+) game=([0-9A-F]+) push=([0-9A-F]+) n=(\d+) drops=(\d+) dirty=(\d+)")
perf_re = re.compile(
    r"PERF: display=(\d+)\.(\d+)ms/(\d+)ms \(transpose=(\d+)\.(\d+) submit=(\d+)\.(\d+)\) blocks=(\d+)"
    r" \[xpose=(\d+)\.(\d+) ovl=(\d+)\.(\d+) draw=(\d+)\.(\d+)\] rows=(\d+)\.(\d+) max=(\d+)")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    first = 0
    if "--first" in sys.argv:
        first = int(sys.argv[sys.argv.index("--first") + 1])
    path = args[0]
    text = open(path, encoding="utf-8", errors="replace").read()
    if first:
        # 只保留前 first 个 DIAG_FB 与 first 个 PERF 行（保持原顺序）
        d = p = 0
        kept = []
        for line in text.split("\n"):
            if d < first and "DIAG_FB" in line:
                kept.append(line); d += 1
            elif p < first and "tab5_perf_report" in line:
                kept.append(line); p += 1
        text = "\n".join(kept)
        print(f"[--first {first}] DIAG_FB {d} 行 / PERF {p} 行")
    diag = [(int(m[0]), int(m[3]), int(m[4]), int(m[5])) for m in diag_re.findall(text)]
    perf = []
    for m in perf_re.findall(text):
        g = [int(x) for x in m]
        perf.append(dict(disp=g[0] + g[1] / 100, win=g[2], tr=g[3] + g[4] / 100, sub=g[5] + g[6] / 100,
                         blocks=g[7], xp=g[8] + g[9] / 100, ovl=g[10] + g[11] / 100,
                         dr=g[12] + g[13] / 100, rows=g[14] + g[15] / 10, mx=g[16]))

    print(f"=== {path} ===")
    print(f"DIAG_FB 行数 {len(diag)} | PERF 行数 {len(perf)}")
    if len(diag) < 5 or len(perf) < 5:
        print("✗ 数据不足（没进游戏 / 诊断没开？）")
        return 1

    # ── 用 DIAG_FB 的帧号算"推屏 fps"与"每帧平均脏行数"
    f0, n0, d0, dirty0 = diag[0]
    f1, n1, d1, dirty1 = diag[-1]
    # DIAG_FB 每 60 帧一行 ⇒ 行数也能反推时间轴（每行 = 1 秒）
    secs = len(diag) - 1
    frames = f1 - f0
    fps = frames / secs if secs else 0
    print(f"\nDIAG_FB 窗口：{secs} 秒 | 帧号 {f0} → {f1}（{frames} 帧）")
    print(f"  推屏 fps（游戏帧率）        : {fps:.1f} fps")
    print(f"  每帧平均脏行数              : {(dirty1 - dirty0) / frames if frames else 0:.1f} 行/帧")
    print(f"  每帧平均推送块数            : {(n1 - n0) / frames if frames else 0:.2f} 块/帧")
    print(f"  窗口内丢块 {d1 - d0} / 推送 {n1 - n0}"
          f"（丢弃率 {(d1 - d0) / (n1 - n0) * 100 if n1 != n0 else 0:.1f}%）")

    # ── 用 PERF 行算显示通路的负载占比（每行是一个 1 秒窗口）
    k = len(perf)
    avg = lambda f: sum(p[f] for p in perf) / k
    print(f"\nPERF 窗口：{k} 个 1 秒窗口（均值 / 最大）")
    print(f"  display 占用              : {avg('disp'):.1f} ms/s  / {max(p['disp'] for p in perf):.1f}")
    print(f"    └ transpose(xpose)      : {avg('xp'):.1f} ms/s  / {max(p['xp'] for p in perf):.1f}")
    print(f"    └ submit(sub)           : {avg('sub'):.1f} ms/s  / {max(p['sub'] for p in perf):.1f}")
    print(f"    └ overlay(ovl)          : {avg('ovl'):.1f} ms/s  / {max(p['ovl'] for p in perf):.1f}")
    print(f"    └ draw(dr)              : {avg('dr'):.1f} ms/s  / {max(p['dr'] for p in perf):.1f}")
    print(f"  推送块数/秒               : {avg('blocks'):.1f}  / {max(p['blocks'] for p in perf)}")
    print(f"  每次推送平均行数          : {avg('rows'):.2f}  / 单次最大 {max(p['mx'] for p in perf)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
