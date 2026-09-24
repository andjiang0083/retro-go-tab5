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

IMG="${1:-$(ls -t "$ROOT"/retro-go-p4/retro-go_*_tab5.img 2>/dev/null | head -1)}"
[ -f "$IMG" ] || { echo "找不到 img（先跑 rg_tool.py --target tab5 build-img launcher gbsp --no-networking）"; exit 1; }

echo "=== 待刷镜像 ==="; ls -la "$IMG"; shasum -a 256 "$IMG"

echo "=== 写前核对设备分区表（技能 8.58）==="
"$PY" -m esptool --chip esp32p4 -p "$PORT" -b 921600 read_flash 0x8000 0xC00 /tmp/dev_pt_now.bin

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
