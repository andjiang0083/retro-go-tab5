# M5Burner 发布字段（retro-go Tab5 v0.3）

现有条目：**Name 保持不变即可追加新版本**。

| 字段 | 值 |
|---|---|
| Name | `retro-go Tab5`（不变） |
| Version | `0.3` |
| Device Type / 类别 | `tab5` |
| Github | `https://github.com/andjiang0083/retro-go-tab5` |
| FirmWare（上传文件） | `retro-go-tab5-0.3-merged.bin` |
| Cover | 保持不变（沿用现有封面） |
| author | `andjiang` |

## Description（⚠ 建议替换 —— 现有描述被翻译工具搞坏了）

现有描述里有三处明显的机器翻译事故：
- `roms/ ← ROM Table of contents`（"目录"被当成"内容目录"）
- `config/ ← set up、最近游玩、Error 500 (Server Error)!!1500.That's an error...`
  （"收藏"被翻成了一整段 HTTP 500 错误页文案 ✗）
- `bios/ ← 其他机种的 BIOS Table of contents`

干净版本见同目录 **`PUBLISH_DESCRIPTION.txt`**（中英双语，直接全选复制）。

## Change log

见同目录 **`PUBLISH_CHANGELOG.txt`**。

## 上传文件

`retro-go-tab5-0.3-merged.bin`（2,424,832 字节）
sha256 见同名 `.sha256` 文件；布局已校验：
bootloader@0x2000 `e903024f`（DIO + P4）✓ / partitions@0x8000 `aa50` ✓ / launcher@0x10000 ✓ / gbsp@0x100000 ✓
并与 v0.2 包前 0x10000 字节**完全一致**（bootloader 与分区表同源，金标准校验 ✓）
