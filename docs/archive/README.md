# docs/archive — 过程性文档归档

这里放**已经完成使命、但仍有查阅价值**的过程文档。归档原则：**只移动，不删内容**
（原文同时贴在 GitHub Release notes 里，见下），`docs/` 根目录只留"活的"参考文档。

## 为什么归档

`docs/` 曾经混着两类东西：**当参考用的活文档**（接口约定、布局规则、当前状态）和
**一次性的过程记录**（某晚的排查日志、交接说明、代码走查报告、候选图）。后者越积越多，
新人打开 `docs/` 无法判断"哪份还作数"。2026-10-07 做了一次归档：过程文档进本目录，
根目录只留索引能解释清楚的活文档。

## 归档清单

| 文件 | 是什么 | 为什么归档 |
|---|---|---|
| `PORTRAIT-TAB5-HANDOFF.md` | 竖屏改版的完整交接（状态 / 构建刷机命令 / 纪律 / 待办 / 已知坑） | 竖屏改版已落地并发布，交接使命结束 |
| `NIGHT-2026-09-25-DISPLAY.md` | 显示带宽那一夜的完整证据链（AXI-ICM QoS / DPI 帧缓冲 / 分块） | 结论已进代码注释与 `esp32-p4-display-bandwidth` skill |
| `SNES-PERF-SESSION-2026-10-07.md` | SNES 性能会话记录（起点终点、测量体系、因果链） | 会话已结束；结论进 `TAB5-PORT-STATUS.md` |
| `CODE-REVIEW-v0.4.1.md` | v0.4.1 全项目代码走查处置报告 | 针对的是旧版本；源码注释里仍引用它（路径已同步） |
| `NEXT-SESSION-PROMPT.txt` | **内部 agent 提示词**（给下一轮自动化会话的开场指令） | 见下方"内部产物"说明 |
| `skin-candidates/` | 皮肤候选图 130 个文件 / 6.3 MB（含 `approved-2026-10-06`、`approved-2026-10-07` 两版基线） | docs 体积的大头；候选图不是文档 |

## 内部产物说明（重要）

`NEXT-SESSION-PROMPT.txt` 是**我们内部自动化流程的提示词**（写给 agent 的开场指令，
不是给用户的文档）。它出现在公开仓库里属于流程外溢 —— 保留是因为它是那条流程的唯一记录，
但**不要**把它当成项目文档引用；文件头也标了这一点。

## 从 Release notes 找回原文

归档内容的**原文**贴在对应 Release 的正文里（`v0.4.7`，以折叠块形式），即使将来本目录
被移动或重建，历史文本也不会丢。候选图（`skin-candidates/`）作为图片不贴进正文，
仍在仓库内可查。

## 引用与门禁（改路径时要一起看的）

- `tools/preview-skin.py` 的 `OUT_DIR` / `BASELINE_DIR` 指向本目录的 `skin-candidates/`；
  回归门禁 `python3 tools/preview-skin.py --check` 仍应通过（归档当日实测：0 失败）。
- 源码注释里引用这些文档的地方已同步改成 `docs/archive/...`。
  **例外**：`vendor/m5stack_tab5/m5stack_tab5.c` 里有一处注释仍写着 `docs/NIGHT-2026-09-25-DISPLAY.md`
  —— 那是上游继承的 vendor 文件，按纪律"能不动上游文件就别动"刻意没改（改一句注释也会在
  `docs/UPSTREAM-DIVERGENCE.md` 里多出一条差异）。
