#!/usr/bin/env python3
"""双方向分区表一致性门禁（Tab5 / S2 横竖屏）。

为什么存在：分区表在这套工程里有**三处副本**，任一处漂移都会造成
"切了方向却进不去 / 刷了机槽却不存在"这类难查故障（实证教训见 ESP32-经验沉淀.md §233）：

  ① tools/partitions-dual-tab5.csv      —— 刷机脚本写 0x8000 的来源
  ② retro-go-p4/rg_tool.py              —— merged 镜像内嵌表的来源（build_image() 里的元组）
  ③ <merged>.img 的 0x8000 处那张表      —— 设备实际拿到的表（可选：给了镜像才查）

规矩（用户口径）：真源必须唯一、双副本必漂移 ⇒ **机器能判的绝不靠人判**，
所以把它做成门禁：不一致就非 0 退出，刷机脚本会在动设备前先跑它。

用法：
  python3 tools/check-dual-partition.py                      # 只查 ① vs ②
  python3 tools/check-dual-partition.py dist/xxx/merged.img   # 再查 ③（镜像内嵌表）
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.path.join(ROOT, "tools", "partitions-dual-tab5.csv")
RG_TOOL = os.path.join(ROOT, "retro-go-p4", "rg_tool.py")
LANDS = ("launcher", "retro-core", "gbsp")   # 横屏槽 = 同名 + `_l`


def fail(msg):
    print("✗ " + msg)
    sys.exit(1)


def parse_csv(path):
    """返回 [(name, type, subtype, offset, size)]"""
    rows = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            name, typ, sub, off, size = [x.strip() for x in line.split(",")[:5]]
            rows.append((name, typ, sub, int(off), int(size)))
    return rows


def csv_landscape(rows):
    return [(n, o, s) for (n, t, _, o, s) in rows if n.endswith("_l")]


def rg_tool_landscape():
    """从 rg_tool.py 的 build_image() 里取横屏槽元组（这是 merged 内嵌表的来源）。"""
    src = open(RG_TOOL, encoding="utf-8").read()
    m = re.search(r"if img_format == \"esp32p4\":\s*\n(.*?)\n\n", src, re.S)
    if not m:
        fail("在 rg_tool.py 里找不到 esp32p4 的横屏槽声明段（build_image 是否被改回？）")
    pairs = re.findall(r"\(\"([\w-]+)\",\s*(0x[0-9a-fA-F]+)\)", m.group(1))
    if not pairs:
        fail("rg_tool.py 的横屏槽元组解析不到")
    return [(f"{n}_l", int(s, 16)) for n, s in pairs]


def embedded_table(img_path):
    """从 merged 镜像 0x8000 处抠出分区表并解析（ESP-IDF 表格式：魔数 0x50AA + 32 字节/项）。"""
    with open(img_path, "rb") as f:
        f.seek(0x8000)
        blob = f.read(0xC00)
    entries = []
    for i in range(0, len(blob) - 31, 32):
        magic, typ, sub, off, size = struct.unpack("<HBBII", blob[i:i + 12])
        name = blob[i + 12:i + 28].split(b"\x00")[0].decode("utf-8", "replace")
        if magic != 0x50AA or not name:
            break
        entries.append((name, off, size))
    if not entries:
        fail(f"{img_path} 的 0x8000 处没有解析出分区表")
    return entries


def main():
    if not os.path.exists(CSV):
        fail(f"缺真源文件 {CSV}")
    rows = parse_csv(CSV)
    csv_l = csv_landscape(rows)
    tool_l = rg_tool_landscape()

    print(f"真源 {os.path.relpath(CSV, ROOT)}:")
    for n, o, s in csv_l:
        print(f"  {n:<14} off=0x{o:06x} size=0x{s:x} ({s // 1024}K)")

    # ① 名称 + 尺寸必须一致
    if [n for n, _ in tool_l] != [n for n, _, _ in csv_l]:
        fail(f"槽名不一致：rg_tool.py={[n for n,_ in tool_l]} vs csv={[n for n,_,_ in csv_l]}")
    for (n, o, s), (n2, s2) in zip(csv_l, tool_l):
        if s != s2:
            fail(f"槽 {n} 尺寸不一致：csv=0x{s:x} vs rg_tool.py=0x{s2:x}")

    # ② 偏移必须等于"竖屏最后一个分区的终点 + 前面横屏槽尺寸累加"
    app_rows = [(n, o, s) for (n, t, _, o, s) in rows if t == "app" and not n.endswith("_l")]
    if not app_rows:
        fail("真源里没有竖屏 app 槽")
    expect = max(o + s for _, o, s in app_rows)
    for n, o, s in csv_l:
        if o != expect:
            fail(f"槽 {n} 偏移算错：csv=0x{o:x} 但应为 0x{expect:x}（竖屏结尾/前槽累加）")
        expect += s
        if o % 0x10000:
            fail(f"槽 {n} 偏移 0x{o:x} 未按 64K 对齐（app 分区必须 64K 对齐）")
    print(f"✓ ①-② 一致：{len(csv_l)} 个横屏槽，结束于 0x{expect:x} ({(expect + 0xFFFF) // 0x10000 * 64 // 1024}MB 内)")

    # ③ 给了 merged 镜像就再核对内嵌表
    if len(sys.argv) > 1:
        img = sys.argv[1]
        if not os.path.exists(img):
            fail(f"镜像不存在：{img}")
        emb = {n: (o, s) for n, o, s in embedded_table(img)}
        for n, o, s in csv_l:
            if n not in emb:
                fail(f"镜像 {img} 内嵌表里没有槽 {n}（说明这份镜像不是用修好的 rg_tool.py 编的）")
            eo, es = emb[n]
            if (eo, es) != (o, s):
                fail(f"槽 {n} 镜像内嵌表与真源不一致：img=0x{eo:x}/0x{es:x} vs csv=0x{o:x}/0x{s:x}")
        print(f"✓ ③ {os.path.basename(img)} 内嵌表与真源一致（{len(csv_l)} 个横屏槽）")
    else:
        print("（未给镜像路径，跳过 ③ 内嵌表核对）")
    print("\n全部门禁通过 ✓")


if __name__ == "__main__":
    main()
