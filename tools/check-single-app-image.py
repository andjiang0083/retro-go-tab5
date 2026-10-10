#!/usr/bin/env python3
"""0.4.9 单 app 镜像门禁 —— 刷机前用它断言「要刷的就是刚编的，而且是单 app 表」。

用法:
    python3 tools/check-single-app-image.py <merged.img> [--newer-than <path>] [--app launcher]

为什么需要它（都是踩过的坑）:
 1) **P3 隐患**：`rg_tool.py` 的内嵌分区表曾在单 app 构建里照样声明横屏 `_l` 槽（`launcher_l`
    等），把 app 槽钉死在 1984K、余量只剩 35KB，而且设备端表与真源 CSV 还不一致。单 app 形态
    下这些槽根本不该存在 —— 本脚本把这条件变成**机器可判**的断言，而不是靠人记得。
 2) **"刷的不是刚编的"**：本项目有多次刷到上一次镜像 / 刷错方向的记录。`--newer-than` 用
    mtime 做最低限度断言（配合 sha256 打印，人工可复核）。

判据（任一不满足即退出码 1）:
 A. 0x8000 处有合法分区表（magic 0xAA50）。
 B. 表里恰好**一个** app 槽（subtype ota_0），且没有任何以 `_l` 结尾的槽。
 C. 该 app 槽必须有镜像数据（镜像长度 > 0x10000），且镜像不超出槽尾。
 D. 表尾之后没有越界（最后一个分区不超出给定 flash 大小，默认 16MB）。
 E. 可选：镜像 mtime 不早于 `--newer-than` 指定的文件。

输出：分区表 + sha256 + 体积，便于人工复核。
"""
import argparse
import hashlib
import os
import struct
import sys

PART_MAGIC = 0x50AA
PART_ENTRY_SIZE = 32
PART_TABLE_OFFSET = 0x8000
DEFAULT_FLASH_SIZE = 16 * 1024 * 1024
APP_TYPE = 0x00
OTA_0 = 0x10


def parse_table(img: bytes, offset: int = PART_TABLE_OFFSET):
    """解析二进制分区表，返回 [(label, type, subtype, offset, size)]。"""
    entries = []
    for i in range(95):  # 表最大 95 项（0xC00 字节）
        pos = offset + i * PART_ENTRY_SIZE
        if pos + PART_ENTRY_SIZE > len(img):
            break
        magic, ptype, subtype, part_off, size = struct.unpack_from("<HBBII", img, pos)
        if magic != PART_MAGIC:
            break
        label = img[pos + 12:pos + 28].split(b"\x00")[0].decode("utf-8", "replace")
        entries.append((label, ptype, subtype, part_off, size))
    return entries


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", help="merged 镜像路径（含内置分区表）")
    ap.add_argument("--newer-than", default=None,
                    help="断言镜像 mtime 不早于该文件（防刷到上一次的镜像）")
    ap.add_argument("--app", default="launcher", help="期望的单 app 槽标签（默认 launcher）")
    ap.add_argument("--flash-size", type=lambda s: int(s, 0), default=DEFAULT_FLASH_SIZE)
    args = ap.parse_args()

    problems = []
    if not os.path.isfile(args.image):
        print("❌ 镜像不存在: %s" % args.image)
        return 1
    img = open(args.image, "rb").read()
    size = len(img)

    print("镜像  : %s" % args.image)
    print("sha256: %s" % hashlib.sha256(img).hexdigest())
    print("体积  : %d B (%.2f KB)" % (size, size / 1024))

    if args.newer_than:
        if not os.path.isfile(args.newer_than):
            problems.append("--newer-than 指定的文件不存在: %s" % args.newer_than)
        else:
            newer_ref = os.path.getmtime(args.newer_than)
            if os.path.getmtime(args.image) + 1 < newer_ref:
                problems.append("镜像 mtime 早于 %s ⇒ 可能刷到上一次的产物" % args.newer_than)
            else:
                print("mtime : 比 %s 新 ✓" % os.path.basename(args.newer_than))

    entries = parse_table(img)
    if not entries:
        print("❌ 0x8000 处没有合法分区表（magic 0xAA50 未命中）")
        return 1

    print("\n分区表:")
    print("  %-16s %-6s %-6s %10s %10s" % ("label", "type", "sub", "offset", "size"))
    for label, ptype, subtype, off, sz in entries:
        print("  %-16s 0x%02x   0x%02x   0x%08X %8.0fK" % (label, ptype, subtype, off, sz / 1024))

    apps = [e for e in entries if e[1] == APP_TYPE]
    land = [e for e in entries if e[0].endswith("_l")]
    if land:
        problems.append("单 app 镜像里出现了横屏槽 %s ⇒ rg_tool.py 的 P3 隐患复发（内嵌表被写成了双 app 表）"
                        % ", ".join(e[0] for e in land))
    if len(apps) != 1:
        problems.append("app 槽数量应为 1（单 app 形态），实为 %d 个: %s"
                        % (len(apps), ", ".join(e[0] for e in apps)))
    else:
        label, _, subtype, off, slot = apps[0]
        if subtype != OTA_0:
            problems.append("单 app 槽 subtype 应为 ota_0(0x10)，实为 0x%02x" % subtype)
        if label != args.app:
            problems.append("单 app 槽标签应为 %r，实为 %r" % (args.app, label))
        if size <= 0x10000:
            problems.append("镜像里没有 app 数据（长度 ≤ 0x10000）")
        elif size > off + slot + 1:
            problems.append("镜像长度 %d 超出 app 槽尾 %d（槽 0x%X + 0x%X）⇒ 被截断"
                            % (size, off + slot, off, slot))
        else:
            # ⚠ 镜像长度恒等于 0x10000 + 槽宽（build_image 按槽补 0xFF）⇒ 拿它算余量没有意义。
            # 真实用量 = 槽内**最后一个非 0xFF 字节**的位置（0xFF 是填充值）—— 这样才看得出余量，
            # 也才能在"再加功能就顶破槽"之前就报警。
            tail = img[off:off + slot]
            used = len(tail.rstrip(b"\xFF"))
            print("\napp 槽: %s @0x%X  槽 %dK  实际用 %.1fK（余 %.1f KB，余量 %.1f%%）"
                  % (label, off, slot / 1024, used / 1024,
                     (slot - used) / 1024, 100.0 * (slot - used) / slot))
            if slot - used < 8 * 1024:
                print("⚠ 余量不足 8KB —— 再加功能就会顶破槽（届时 rg_tool.py 会自动扩容，属预期行为）")

    for label, _, _, off, sz in entries:
        if off + sz > args.flash_size:
            problems.append("分区 %s 越界：0x%X + 0x%X > flash %d" % (label, off, sz, args.flash_size))

    if problems:
        print("\n❌ 门禁未通过:")
        for p in problems:
            print("  - %s" % p)
        return 1
    print("\n✅ 门禁通过：单 app 表形态正确、app 数据完整、无横屏槽残留")
    return 0


if __name__ == "__main__":
    sys.exit(main())
