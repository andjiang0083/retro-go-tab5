#!/usr/bin/env python3
"""port-diff.py — 生成「本移植 vs 上游」的差异清单（docs/UPSTREAM-DIVERGENCE.md）

为什么需要它：
  本仓的 retro-go-p4/ 是上游 ducalex/retro-go 的**整树拷贝 + 就地修改**，
  所以任何 reviewer 打开仓库看到的都是一棵完整的上游树，
  看不出「哪 70 个文件是移植改的、哪 173 个是新增的、哪些上游文件被删了」。
  这个脚本把那条边界**机器算出来**，不靠人记。

原理：
  内容相同的文件 ⇒ git blob SHA 相同。所以直接比 blob SHA 就能分类，
  不需要下载上游整树（只取 GitHub 的 tree 列表，几十 KB）。

用法:
  python3 tools/port-diff.py            # 生成/更新 docs/UPSTREAM-DIVERGENCE.md
  python3 tools/port-diff.py --check    # 只比对不写文件；与已生成文档不一致则退出码 1

注意:
  * 比对基准是**上游 master 的当前提交**（会把 SHA 写进文档）。上游一动，数字就会变，
    文档里因此记录 SHA —— 引用数字时连着 SHA 一起引用。
  * 本地一侧取 git **HEAD**（已提交状态），不是工作区，保证可复现。
"""

import json
import os
import subprocess
import sys
import urllib.request
from collections import Counter, defaultdict
from datetime import datetime, timezone

REPO = "ducalex/retro-go"
BRANCH = "master"
SUBTREE = "retro-go-p4/"          # 本仓里承载移植的子树（必须与上游根目录一一对应）
OUT = "docs/UPSTREAM-DIVERGENCE.md"

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)      # 仓库根（本脚本住在 <root>/tools/ 下）


def api(url):
    req = urllib.request.Request(url, headers={
        "Accept": "application/vnd.github+json",
        "User-Agent": "retro-go-tab5 port-diff",
    })
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)


def git(*args):
    return subprocess.run(["git", "-C", ROOT, *args],
                          capture_output=True, text=True, check=True).stdout


def fetch_upstream():
    """返回 (上游提交 SHA, {路径: blob SHA})"""
    ref = api(f"https://api.github.com/repos/{REPO}/git/refs/heads/{BRANCH}")
    sha = ref["object"]["sha"]
    tree = api(f"https://api.github.com/repos/{REPO}/git/trees/{sha}?recursive=1")
    if tree.get("truncated"):
        sys.exit("✗ 上游 tree 被截断，结果不完整；请改用 git clone 方式比对")
    blobs = {t["path"]: t["sha"] for t in tree["tree"] if t["type"] == "blob"}
    return sha, blobs


def local_tree():
    """返回本仓 retro-go-p4/ 子树下的 {相对路径: blob SHA}（取 HEAD）

    记录的是**子树对象 hash**（git rev-parse HEAD:retro-go-p4），不是 commit hash：
    文档一旦提交，写进去的 commit hash 立刻过期；子树 hash 描述的正是被比对的东西。
    """
    sub = git("rev-parse", f"HEAD:{SUBTREE.rstrip('/')}").strip()
    out = git("ls-tree", "-r", "HEAD", "--", SUBTREE)
    files = {}
    for line in out.splitlines():
        meta, path = line.split("\t", 1)
        files[path[len(SUBTREE):]] = meta.split()[2]
    if not files:
        sys.exit(f"✗ 本仓 HEAD 里找不到 {SUBTREE}/ —— 请确认在仓库根运行")
    return sub, files


def bucket(path):
    """把文件按「面」分组，好让人一眼看出哪些是板级改动、哪些是生成物噪声"""
    if path.startswith("components/retro-go/targets/"):
        if path.endswith("sdkconfig") or path.endswith("sdkconfig.defaults"):
            return "targets / 生成物（sdkconfig，多为噪声）"
        return "targets / 板级配置（config.h / env.py）"
    if path.startswith("components/retro-go/drivers/"):
        return "框架 drivers（显示/音频/输入驱动）"
    if path.startswith("components/retro-go/"):
        return "框架核心（rg_*）"
    if path.startswith("launcher/"):
        return "launcher（ROM 前端）"
    if path.startswith("gbsp/"):
        return "gbsp/（GBA 核心，本移植独立加入）"
    if path.startswith(("tools/", "docs/")) or path.split("/")[0] in {
            "rg_tool.py", "base.cmake", ".gitignore", "CHANGELOG.md", "PORTING.md",
            "retro-go.code-workspace", "CMakeLists.txt"} or path.endswith((".cmake", ".py")):
        return "构建 / 工具 / 文档"
    if path.startswith(("retro-core/", "prboom-go/", "gwenesis/", "themes/")):
        return "其它机种 / 主题"
    return "其它"


def grouped(paths):
    g = defaultdict(list)
    for p in paths:
        g[bucket(p)].append(p)
    return {k: sorted(v) for k, v in sorted(g.items(), key=lambda kv: (-len(kv[1]), kv[0]))}


def render(up_sha, ours_sub, ours, up):
    same = [p for p in ours if p in up and ours[p] == up[p]]
    mod = sorted(p for p in ours if p in up and ours[p] != up[p])
    new = sorted(p for p in ours if p not in up)
    gone = sorted(p for p in up if p not in ours)
    now = datetime.now(timezone.utc).astimezone().strftime("%Y-%m-%d %H:%M %Z")

    L = []
    A = L.append
    A("# 移植面清单：本仓 vs 上游 retro-go")
    A("")
    A("> **本文件由 `tools/port-diff.py` 生成，不要手改** —— 复跑一次就回来了。"
      "（手写说明只允许出现在最下面「人工说明」一节，那节不会被覆盖。）")
    A("")
    A(f"- 生成时间：{now}")
    A(f"- 上游基准：[`{REPO}@{BRANCH}`](https://github.com/{REPO}/tree/{BRANCH}) = `{up_sha}`")
    A(f"- 本仓基准：`retro-go-p4` 子树对象 `{ours_sub}`（`git rev-parse HEAD:retro-go-p4`），"
      f"共 {len(ours)} 个文件")
    A("- 这条记录的是**子树对象 hash** 而不是 commit hash：写 commit hash 会在提交那一刻就过期。")
    A(f"- 比对方式：git blob SHA（内容相同 ⇒ SHA 相同），**不下载上游整树**")
    A("")
    A("## 一句话")
    A("")
    A(f"`{SUBTREE}` 是上游的整树拷贝，其中 **{len(same)} 个文件逐字节相同**、"
      f"**{len(mod)} 个被就地修改**、**{len(new)} 个是移植新增**，"
      f"另有 **{len(gone)} 个上游文件没有带上**。"
      "要 review 这个移植，只需要看后三张表；第一张表之外都是上游代码。")
    A("")
    A("| 类别 | 数量 |")
    A("|---|---|")
    A(f"| 与上游逐字节相同（上游代码，勿改） | {len(same)} |")
    A(f"| 上游文件被就地修改 | {len(mod)} |")
    A(f"| 本仓新增（上游没有） | {len(new)} |")
    A(f"| 上游有、本仓未带上 | {len(gone)} |")
    A(f"| 上游文件总数 | {len(up)} |")
    A("")
    A(f"## 一、被修改的上游文件（{len(mod)} 个）")
    A("")
    A("这些是**移植真正改到上游代码的地方**，也是将来跟进上游更新时唯一需要看的部分。")
    A("")
    for name, items in grouped(mod).items():
        A(f"### {name}（{len(items)}）")
        A("")
        for p in items:
            A(f"- `{p}`")
        A("")
    A(f"## 二、新增文件（{len(new)} 个）")
    A("")
    for name, items in grouped(new).items():
        A(f"### {name}（{len(items)}）")
        A("")
        for p in items:
            A(f"- `{p}`")
        A("")
    A(f"## 三、未带上的上游文件（{len(gone)} 个）")
    A("")
    A("上游有、本仓没有。如果这里出现**非预期**的条目，说明某次同步或清理误删了上游文件。")
    A("")
    for name, items in grouped(gone).items():
        A(f"### {name}（{len(items)}）")
        A("")
        for p in items:
            A(f"- `{p}`")
        A("")
    A("## 四、怎么复跑 / 怎么当门禁")
    A("")
    A("```sh")
    A("python3 tools/port-diff.py            # 重新生成本文件")
    A("python3 tools/port-diff.py --check    # 与已生成内容比对；不一致 → 退出码 1（可进 CI）")
    A("```")
    A("")
    A("## 五、人工说明（手写区，脚本保留）")
    A("")
    A("<!-- 手工说明请写在下面；脚本重新生成时会原样保留这一段 -->")
    A("")
    A(MANUAL)
    return "\n".join(L) + "\n"


MANUAL = """- **为什么有 25 个上游文件没带上**：三个整板 target 目录（`brutzelboy`、`esp32-s3-devkit`、
  `redroid-go`）与它们自带的照片/原理图对这一台设备没用；上游 2 张面板数据手册 PDF 与 GBA 主题背景图同理。
  上游两个 issue 模板被换成了本仓自己的 md 版（不是删，是替换）。
- **哪些"修改"其实不是移植**：`targets/*/sdkconfig` 这类生成物会因为 IDF 版本/默认值差异产生漂移，
  归入「生成物」分组；看移植改动时可以直接跳过。
- **改上游文件的规矩**：能不动上游文件就别动；必须动时，一次只动一处并在 `docs/` 里留结论。
  上游跟进（pull upstream）时按本文件第一节逐条过。
"""


def load_manual(path):
    """取回已生成文档里的手写区，重新生成时不丢失"""
    try:
        with open(path, encoding="utf-8") as f:
            txt = f.read()
    except FileNotFoundError:
        return MANUAL
    marker = "<!-- 手工说明请写在下面；脚本重新生成时会原样保留这一段 -->"
    if marker in txt:
        body = txt.split(marker, 1)[1]
        # 去掉文件尾部的多余空行
        body = body.strip("\n")
        if body.strip():
            return body
    return MANUAL


def main():
    check = "--check" in sys.argv[1:]
    out_path = os.path.join(ROOT, OUT)
    up_sha, up = fetch_upstream()
    ours_sha, ours = local_tree()
    global MANUAL
    MANUAL = load_manual(out_path)
    doc = render(up_sha, ours_sha, ours, up)

    if check:
        try:
            cur = open(out_path, encoding="utf-8").read()
        except FileNotFoundError:
            print(f"✗ {OUT} 不存在，请先不带 --check 跑一次")
            return 1
        # 时间戳每次都变，比对时忽略
        def strip_ts(s):
            return "\n".join(l for l in s.splitlines() if not l.startswith("- 生成时间："))
        if strip_ts(cur) != strip_ts(doc):
            print(f"✗ {OUT} 与上游当前状态不一致（上游可能已更新）")
            return 1
        print(f"✓ {OUT} 与 {REPO}@{BRANCH} 一致")
        return 0

    with open(out_path, "w", encoding="utf-8") as f:
        f.write(doc)
    print(f"✓ 已生成 {OUT}（上游 {up_sha[:8]}）："
          f"相同 {sum(1 for p in ours if p in up and ours[p] == up[p])} / "
          f"修改 {sum(1 for p in ours if p in up and ours[p] != up[p])} / "
          f"新增 {sum(1 for p in ours if p not in up)} / "
          f"未带上 {sum(1 for p in up if p not in ours)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
