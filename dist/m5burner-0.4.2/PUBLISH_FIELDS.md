# M5Burner 发布字段（retro-go Tab5 v0.4.2 · 竖屏版 · 肩键可换位）

现有条目：**Name 保持不变即可追加新版本**。

| 字段 | 值 |
|---|---|
| Name | `retro-go Tab5`（不变） |
| Version | `0.4.2` |
| Device Type / 类别 | `tab5` |
| Github | `https://github.com/andjiang0083/retro-go-tab5` |
| FirmWare（上传文件） | `retro-go-tab5-0.4.2-merged.bin`（2,490,368 字节） |
| Cover | `cover-320x200.png`（本目录，本次重画：真渲染的竖屏界面 + 设备外框，含新的调换键） |
| author | `andjiang`（不变） |

## Description / Change log

直接复制同目录的 `PUBLISH_DESCRIPTION.txt`（中英双语）与 `PUBLISH_CHANGELOG.txt`。

## 上传前校验（本次实测）

```
bootloader@0x2000  e903024fda9ef24f   ✓ DIO + ESP32-P4
partitions@0x8000  aa50010200900000   ✓ 分区表魔数 aa50
app@0x10000        launcher            ✓ 与设备回读一致（刷机脚本自查通过）
app@0x100000       gbsp                ✓ 与设备回读一致
flash_mode 字节    = 02 (DIO)         ✓ 与 v0.4.1 / v0.4 / v0.3 一致
sha256             f01d69b55c57518c…   （见 .sha256 附件）
```

与 v0.4.1 的 bin 对比：**大小相同（2,490,368）、前 64 KB 逐字节完全一致**（bootloader 与分区表都没动），
差异全部落在 app 段（版本戳 + 触摸叠加层/输入改动）→ 从 0x0 整片写入可正常启动。

## 发布后

- 发布必须由用户本人在 M5Burner 桌面软件里点（账号登录不可代劳）：右上角登录 → 左下角 **Publish**
- 版本号填 `0.4.2`、FirmWare 选本目录的 merged bin、Cover 选 `cover-320x200.png`
- 发布成功后本机可先自己 Burn 一次验证（Share Code 可分享）
