#!/bin/bash
# Tab5 发布流水线：提交 → 打 tag → **用 tag 重编**（让镜像名读作版本号）→ 门禁 → 打包 dist/m5burner-<ver>/
#
# 用法:  tools/release-tab5.sh <版本号> "<提交信息>" [--commit]
#        不带 --commit 时只做检查并打印将要执行的动作（dry-run，默认）
#
# 为什么要有这个脚本（而不是手敲五条命令）：
#   发布最容易出的错是「发布包不是刚编的那个」「镜像名里还带着 -dirty」——这两条都能机器判，
#   就不该靠人盯。脚本把断言写死：构建缺日志/有错误/工作区脏/版本号没进镜像名 ⇒ 立刻停。
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${1:?用法: tools/release-tab5.sh <版本号> \"<提交信息>\" [--commit]}"
MSG="${2:?缺少提交信息}"
DO_COMMIT="${3:-}"
# git tag 沿用既有约定带 v（历史 tag: v0.4.8）；目录/文件名/M5Burner 版本号用裸版本号（先例: m5burner-0.3）
VER_BARE="${VER#v}"
echo "=== 发布 ${VER}（裸版本号 ${VER_BARE}）==="

# ① 不能有构建在跑（一次只允许一个构建）
if pgrep -f "rg_tool.py --target tab5" >/dev/null || pgrep -f "build-tab5-skin.sh" >/dev/null; then
    echo "⛔ 有构建在跑，等它结束后再发布"; exit 1
fi

# ② 单 app 构建必须绿（日志自证；不绿说明代码没编过，别发布）
LOG=.buildlogs/"$(ls -t .buildlogs | grep -E '^B[0-9]+-single' | head -1)"
grep -q "BUILD END rc=0" "$LOG" || { echo "⛔ 最近的单 app 构建不成功：$LOG"; exit 1; }
echo "  最近单 app 构建: $LOG ✓"

# ③ 工作区必须干净
if [ -n "$(git status --porcelain)" ]; then
    echo "  工作区有未提交改动："; git status --short | sed 's/^/    /'
    [ "$DO_COMMIT" = "--commit" ] || { echo "⛔ 未加 --commit，停在检查阶段（dry-run）"; exit 0; }
else
    echo "  工作区干净 ✓"
fi

if [ "$DO_COMMIT" != "--commit" ]; then
    echo "--- dry-run：将会执行 ---"
    echo "  1) git add -A && git commit -m \"$MSG\""
    echo "  2) git tag $VER"
    echo "  3) RG_TAB5_ORIENTATION=0 tools/build-tab5-skin.sh single-app   # 用 tag 重编"
    echo "  4) python3 tools/check-single-app-image.py <镜像>             # 门禁"
    echo "  5) 打包到 dist/m5burner-$VER/（镜像 + sha256）"
    exit 0
fi

# ④ 提交代码 + 打 tag（tag 只指代码：产物是它的派生物，别把二进制混进 tag 的树里）
git add -A
git commit -m "$MSG"
git tag "$VER"
echo "  已提交代码并打 tag ${VER} ✓"

# ⑤ 用 tag 重编（版本号才会进镜像名）
RG_TAB5_ORIENTATION=0 bash tools/build-tab5-skin.sh single-app > .buildlogs/release-$VER.log 2>&1
grep -q "BUILD END rc=0" .buildlogs/release-$VER.log || { echo "⛔ 重编失败"; tail -3 .buildlogs/release-$VER.log; exit 1; }

IMG="$(ls -t retro-go-p4/*single.img | head -1)"
case "$(basename "$IMG")" in
    *"$VER"*) echo "  镜像名含 $VER ✓  ($IMG)";;
    *) echo "⛔ 镜像名里没有版本号 ⇒ tag 没生效？"; exit 1;;
esac

# ⑥ 门禁 + 打包
python3 tools/check-single-app-image.py "$IMG" | tail -3
OUT=dist/m5burner-$VER_BARE
mkdir -p "$OUT"
cp "$IMG" "$OUT/retro-go-tab5-$VER_BARE-merged.bin"
# M5Launcher 只能装单个 app ⇒ 同一构建的纯 app 镜像一起打包（README 已承诺这个文件名）
cp retro-go-p4/launcher/build/launcher.bin "$OUT/retro-go-tab5-$VER_BARE-launcher-singleapp.bin"
# 校验清单沿用 0.4.7+ 的 SHA256SUMS.txt 格式（两个镜像 + 两张封面）
( cd "$OUT" && shasum -a 256 retro-go-tab5-$VER_BARE-*.bin cover-*.png > SHA256SUMS.txt )
# ⑦ 产物**不入库**：publish.yaml 写了 bin_external: true（公开仓不携带固件），dist/ 也在 .gitignore 里。
#    这里只做本地打包 + 校验清单，上传与回读由发布流程单独完成。
echo "  产物留在本地 $OUT/（公开仓不携带固件，见 publish.yaml 的 bin_external）"

echo "=== 完成：$OUT/ ==="
ls -l "$OUT" | sed 's/^/  /'
echo "下一步（人工）：真机冒烟 + M5Burner 上传（后者需用户点头）"
