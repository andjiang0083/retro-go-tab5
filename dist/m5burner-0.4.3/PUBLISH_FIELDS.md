# M5Burner 发布字段（retro-go Tab5 v0.4.3 · 竖屏版 · 支持 M5Launcher 安装）

现有条目：**Name 保持不变即可追加新版本**。

| 字段 | 值 |
|---|---|
| Name | `retro-go Tab5`（不变） |
| Version | `0.4.3` |
| Device Type / 类别 | `tab5` |
| Github | `https://github.com/andjiang0083/retro-go-tab5` |
| FirmWare（上传文件） | `retro-go-tab5-0.4.3-merged.bin`（2,424,832 字节 —— 比 0.4.2 小 64KB：字库并入 app、少了独立字库分区） |
| Cover | `cover-320x200.png`（沿用 0.4.2：界面外观未变） |
| author | `andjiang`（不变） |

## 本版额外提供一个 M5Launcher 专用包

`retro-go-tab5-0.4.3-launcher-singleapp.bin`（单 app 镜像，1,441,152 字节）——
**不用于 M5Burner**，是给 [bmorcelli/Launcher](https://github.com/bmorcelli/Launcher) 那种"只能装一个
app 镜像"的启动器用的。M5Launcher 会自动把它装进一个自适应大小的 app 分区，用户从它的界面里运行。
README 与 Release Notes 里都写清了"哪个渠道装哪一份"。

## Description / Change log

直接复制同目录的 `PUBLISH_DESCRIPTION.txt`（中英双语）与 `PUBLISH_CHANGELOG.txt`。

## 上传前校验（本次实测）

```
bootloader@0x2000  e903024fda9ef24f   ✓ DIO + ESP32-P4
partitions@0x8000  aa50010200900000   ✓ 分区表魔数 aa50
app@0x10000        launcher            ✓ 与设备回读一致（刷机脚本自查通过）
app@0x100000       gbsp                ✓ 与设备回读一致
flash_mode 字节    = 02 (DIO)         ✓ 与 v0.4.1 / v0.4 / v0.3 一致
sha256             89a9898c688edf9c…   （见 .sha256 附件）
```

与 v0.4.1 的 bin 对比：**大小相同（2,490,368）、前 64 KB 逐字节完全一致**（bootloader 与分区表都没动），
差异全部落在 app 段（版本戳 + 触摸叠加层/输入改动 + 调换键映射修复）→ 从 0x0 整片写入可正常启动。

## 发布后

- 发布必须由用户本人在 M5Burner 桌面软件里点（账号登录不可代劳）：右上角登录 → 左下角 **Publish**
- 版本号填 `0.4.2`、FirmWare 选本目录的 merged bin、Cover 选 `cover-320x200.png`
- 发布成功后本机可先自己 Burn 一次验证（Share Code 可分享）
