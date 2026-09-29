#!/bin/sh
# 刷 retro-go（tab5 target）合并镜像到 Tab5
#
# 用法: sh tools/flash-retro-go.sh            # 刷最新构建出的 .img
#       sh tools/flash-retro-go.sh <img路径>  # 刷指定 img
#
# 说明：
#  - rg_tool.py build-img 产出的是**合并镜像**：0x2000 bootloader / 0x8000 分区表 /
#    0x10000 launcher / 0x100000 gbsp（见 repo 根 partitions.csv），所以整片从 0x0 写。
#  - 首次刷 bootloader 必须 --force（P4 rev1.3 与镜像头的 rev 校验）。
#  - 判成功：esptool 的 Hash of data verified + 写后回读校验；不要用 tail 判断。
set -e
# 端口自动探测：Tab5 真断电重插后设备号会变（usbmodem101 ↔ usbmodem1101），写死会失联
PORT="${PORT:-$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)}"
[ -n "$PORT" ] || { echo "未找到 /dev/cu.usbmodem* 设备，请检查 USB 连接"; exit 1; }
ROOT="$HOME/esp32/retro-go-tab5"
PY="$HOME/.espressif/python_env/idf5.5_py3.14_env/bin/python"

if [ $# -lt 1 ]; then
  # 2026-09-29 走查 P2-19：**删掉"没传参数就按时间取最新"的兜底**。
  # 理由：并发构建会抢同一个文件名，ls -t 拿到的未必是你要的那版 ——
  # 历史上已经因为"点名写成 7 位、grep 空 → 悄悄 ls -t"差点刷错版本。
  echo "✗ 必须点名镜像（本脚本不再有 ls -t 兜底）。"
  echo "  正确姿势：SHA5=\$(git -C \"$ROOT\" log -1 --format='%h' | cut -c1-5)"
  echo "           sh tools/flash-retro-go.sh \"$ROOT/retro-go-p4/retro-go_v0.0.1-<构建号>-g\${SHA5}_tab5.img\""
  echo "  （镜像名里的 hash 是 5 位；7 位会 grep 不到。先 ls 确认文件真实存在再传。）"
  exit 1
fi
# 传了参数就用参数，且必须是存在的文件；不猜、不退化。
IMG="$1"
[ -n "$IMG" ] && [ -f "$IMG" ] || { echo "✗ 传入的镜像路径为空或不存在：'$1' —— 不猜，先修好点名命令"; exit 1; }
[ -f "$IMG" ] || { echo "找不到 img（先跑 rg_tool.py --target tab5 build-img launcher gbsp --no-networking）"; exit 1; }

echo "=== 待刷镜像 ==="; ls -la "$IMG"; shasum -a 256 "$IMG"

echo "=== 写前核对设备分区表（技能 8.58）==="
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x8000 0xC00 /tmp/dev_pt_now.bin
# 2026-09-29 走查 P2-20：原来只是"读出来"没有任何比较，等于没有核对作用。
# 这里把待刷镜像里的分区表抽出来做逐字节比对（布局：0x2000 bootloader / 0x8000 分区表，
# 见 repo 根 partitions.csv），不一致就把两边 hash 都打出来 —— 要么是设备上还装着别的固件，
# 要么是你拿错了 img；两种情况都该先停下来看清楚。
dd if="$IMG" of=/tmp/img_pt.bin bs=1 skip=$((0x8000)) count=$((0xC00)) 2>/dev/null
if cmp -s /tmp/img_pt.bin /tmp/dev_pt_now.bin; then
  echo "✓ 设备分区表与待刷镜像逐字节一致"
else
  echo "⚠ 设备分区表与镜像里的**不一致**（下面两个 hash 自己对比）："
  shasum -a 256 /tmp/img_pt.bin /tmp/dev_pt_now.bin
  echo "  继续刷会把设备分区表改成镜像里的那份；若差异来源不清楚，先 Ctrl-C 停下来。"
fi

echo "=== 整片写入（含 bootloader，首次必须 --force）==="
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 \
  --before default_reset --after hard_reset \
  write_flash --verify --force 0x0 "$IMG"

echo "=== 回读校验：从 img 里抽出 launcher/gbsp 头 64B 与设备比对 ==="
python3 - "$IMG" <<'EOF'
import sys
img = open(sys.argv[1], 'rb').read()
for name, off in (("bootloader", 0x2000), ("launcher", 0x10000), ("gbsp", 0x100000)):
    open(f"/tmp/expect_{name}.bin", 'wb').write(img[off:off+64])
    print(f"{name}: expect 前64B @0x{off:x}")
EOF
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x10000 0x40 /tmp/dev_launcher_after.bin
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x100000 0x40 /tmp/dev_gbsp_after.bin
shasum -a 256 /tmp/expect_launcher.bin /tmp/dev_launcher_after.bin /tmp/expect_gbsp.bin /tmp/dev_gbsp_after.bin

echo "=== 回读 NVS 阶段结束；如需看启动日志，用 tools/log-tab5.sh ==="
