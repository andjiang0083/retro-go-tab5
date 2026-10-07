#!/usr/bin/env python3
"""P2-5 门禁：BIOS 头的数组 md5 必须等于 open_gba_bios.bin 的 md5。

用法：
    python3 tools/check-bios-md5.py                    # 现生成一次（临时目录）再往返核对
    python3 tools/check-bios-md5.py <bios.h 路径>       # 直接核对某个已生成的头

为什么要这条往返：BIOS 过去在仓库里存两份（.bin + 手写 bios.h），改成"构建期从 .bin 生成"
之后，唯一能证明**生成器没把字节搞错**的，就是把生成结果解析回二进制再算一遍 md5
（.bin → 头文件 → 字节序列 → md5 == .bin 的 md5）。源漂移则由生成器里的 pin 值拦截。

退出码：0 = 一致；1 = 不一致/出错（可直接用在发布脚本门禁里）。
"""
import hashlib
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, "retro-go-p4/gbsp/components/gbsp-libretro/bios/open_gba_bios.bin")
GEN = os.path.join(ROOT, "retro-go-p4/gbsp/main/gen_bios_header.cmake")
# 与生成器（gen_bios_header.cmake）里的 pin 值必须一致 —— 两处不一致本身就是漂移
EXPECTED_MD5 = "1876f71b0d8c65eef3454547896ffb11"


def md5(path):
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()


def parse_header(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    return bytes(int(h, 16) for h in re.findall(r"0x([0-9a-fA-F]{2})", text))


def main():
    hdr = sys.argv[1] if len(sys.argv) > 1 else None
    if not os.path.exists(BIN):
        print(f"✗ 找不到 BIOS 源：{BIN}")
        return 1
    bin_md5 = md5(BIN)
    print(f"  源  {os.path.relpath(BIN, ROOT)}  md5={bin_md5}  {os.path.getsize(BIN)} B")

    tmp = None
    if hdr is None:
        tmp = tempfile.mkdtemp(prefix="bios-md5-")
        hdr = os.path.join(tmp, "bios.h")
        r = subprocess.run(["cmake", f"-DGBSP_BIOS_BIN={BIN}", f"-DGBSP_BIOS_HDR={hdr}",
                            "-P", GEN], capture_output=True, text=True)
        if r.returncode != 0:
            print("✗ 生成器失败：")
            print(r.stdout + r.stderr)
            return 1
    if not os.path.exists(hdr):
        print(f"✗ 找不到生成的头：{hdr}")
        return 1

    raw = parse_header(hdr)
    hdr_md5 = hashlib.md5(raw).hexdigest()
    print(f"  头  {hdr}  解析出 {len(raw)} 字节  md5={hdr_md5}")

    if len(raw) != os.path.getsize(BIN):
        print(f"✗ 字节数不符：头 {len(raw)} ≠ 源 {os.path.getsize(BIN)}")
        return 1
    if hdr_md5 != bin_md5:
        print(f"✗ md5 不符：头 {hdr_md5} ≠ 源 {bin_md5}")
        return 1
    if bin_md5 != EXPECTED_MD5:
        print(f"✗ pin 值不符：源 {bin_md5} ≠ pin {EXPECTED_MD5}（源文件被换过？）")
        return 1
    print("✓ 往返一致：头的数组 md5 == 源的 md5 == pin 值")
    if tmp:
        os.remove(hdr)
        os.rmdir(tmp)
    return 0


if __name__ == "__main__":
    sys.exit(main())
