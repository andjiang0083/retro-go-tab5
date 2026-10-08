#!/usr/bin/env python3
"""汇总 A/A 噪声底 与 A/B 效果（口径统一：60 x 推送次数/帧 x 每次行数）。

用法:
  python3 tools/parse-aa-noise-floor.py \
      docs/baseline/display-path-A-raw.txt \
      docs/baseline/display-path-AA-run1-raw.txt \
      docs/baseline/display-path-AA-run2-raw.txt \
      docs/baseline/display-path-B-raw.txt

口径约定（重要）：
  - "推屏行/秒" = 60 x 推送次数/帧 x 每次推送行数。另一个看似合理的算法「块数 x 32 行」是错的
    （本驱动的推送按脏行连续段发出，单次可短于 32 行），两者会差十几个百分点。
  - "次数/帧" 走 DIAG_FB 的累计 n= 计数，"块数/秒" 走 PERF 行；两条路径应能互相推导到 1% 以内，
    否则就是口径混了。窗口一律取同长度（前 N 个窗口，同帧区间）。
"""
import sys, pathlib, re, statistics

N = 213


def series(path: pathlib.Path):
    t = path.read_text(errors="replace")
    fb = re.findall(r"DIAG_FB f=(\d+).*?n=(\d+) drops=(\d+) dirty=(\d+)", t)[:N]
    perf = re.findall(r"PERF: display=([\d.]+)ms/\d+ms.*?blocks=(\d+).*?rows=([\d.]+)", t)[:N]
    if len(fb) < 50 or len(perf) < 50:
        return None
    disp = statistics.mean(float(a) for a, _, _ in perf)
    rows_per_push = statistics.mean(float(c) for _, _, c in perf)
    dn = int(fb[-1][1]) - int(fb[0][1]); df = int(fb[-1][0]) - int(fb[0][0]); dd = int(fb[-1][3]) - int(fb[0][3])
    pushes_f = dn / df
    dirty_f = dd / df
    rows_s = 60 * pushes_f * rows_per_push
    return dict(name=path.name, windows=len(fb), dirty_f=dirty_f, pushes_f=pushes_f,
                rows_push=rows_per_push, rows_s=rows_s, need_s=dirty_f * 60,
                waste=100 * (rows_s / (dirty_f * 60) - 1), disp=disp, drops=int(fb[-1][2]))


rows = [r for r in (series(pathlib.Path(p)) for p in sys.argv[1:]) if r]
if not rows:
    sys.exit("没有可解析的输入（窗口不足 50）")
hdr = (f"{'样本':<34}{'窗口':>5}{'脏行/帧':>9}{'次数/帧':>8}{'每次行':>8}"
       f"{'推屏行/秒':>10}{'需要行/秒':>10}{'多推':>8}{'display':>9}{'丢块':>6}")
print(hdr); print("-" * len(hdr))
for d in rows:
    print(f"{d['name']:<34}{d['windows']:>5}{d['dirty_f']:>9.1f}{d['pushes_f']:>8.2f}{d['rows_push']:>8.2f}"
          f"{d['rows_s']:>10.0f}{d['need_s']:>10.0f}{d['waste']:>7.1f}%{d['disp']:>8.1f}{d['drops']:>6}")

for i in range(len(rows) - 1):
    a, b = rows[i], rows[i + 1]
    def rel(k): return (b[k] / a[k] - 1) * 100
    print(f"\n{a['name']} → {b['name']}: 行/秒 {rel('rows_s'):+.1f}%  "
          f"多推 {b['waste']-a['waste']:+.1f}pp  display {rel('disp'):+.1f}%")
