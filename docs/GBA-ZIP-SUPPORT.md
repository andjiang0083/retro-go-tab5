# GBA 的 zip 支持（0.4.8）

## 一句话

GBA 现在可以直接放 `.zip`。**内存增量是 0 块**：解压结果直接**就是** ROM 缓冲，不额外拷贝。

## 为什么内存不涨

`gbsp` 的 `load_gamepak_raw()` 本来就把整份 ROM 读进 PSRAM（1 MB 一块，最多 32 块；预算 = 空闲 PSRAM − 8 MB）。
zip 只是把「从 SD 顺序读 ROM」换成「读压缩流 + 解压进同一块缓冲」，所以：

- 原先的 1 MB 块会**先被释放**，再要一整块连续缓冲（两者不能同时存在，否则 16 MB ROM 会 OOM）；
- 整块按 1 MB 切片挂进 `gamepak_buffers[]`（块数 = 页数/32 ⇒ 1024 项页表恰好够全量常驻，**永不换页**）；
- `memory_term()` 只 free 一次（`gamepak_buffers[1..n-1]` 只是同一块的别名）。

## 读取器（`components/retro-go/rg_storage.c`）

上游实现只读 zip 的**本地头**。实测发现：**macOS / Java / 部分 Windows 工具产出的 zip 用 data descriptor 模式**
（`flags` bit3），本地头里 `compressed_size`/`uncompressed_size`/`CRC` **全是 0**，真值只写在中央目录或数据后面的
data descriptor 里 —— 上游会把压缩数据长度读成 0，解压必然失败。这不是个例，是常见格式。

升级后的 `rg_storage_unzip_file()`（11 个系统的 zip 支持**一起受益**）：

1. 先走**本地头快路径**（对普通 zip 行为与上游完全一致）；
2. 本地头 size 为 0（data descriptor / 目录条目）⇒ 回退**中央目录**解析（EOCD 定位；一次读入文件尾 64 KB 在内存里搜签名）；
3. 跳过目录条目与 `__MACOSX/`、`.` 开头的垃圾条目；
4. 支持 `method=0`（stored，直接搬）与 `method=8`（deflate，ROM 里的 tinfl）；
5. ZIP64 明确报错（不静默出错）；
6. **解压后校验 CRC**（zip 里带 CRC 才校验）：宁可明确报错，也不让模拟器拿着坏 ROM 黑屏或崩溃。

## 真机实测（Tab5 / ESP32-P4）

| 项目 | 结果 |
|---|---|
| 真实 GBA ROM 头校验 | `SSF2XREVIVAL` / `AXRJ` |
| **SD 直读 4 MB（冷 / 热）** | 4773 / 4774 ms ⇒ **≈ 0.86 MB/s**（VFS 不缓存，重复读一样慢） |
| tinfl（解压）可用性 | **可用**：嵌入式真 deflate zip 解出 131072 B，**CRC32 逐字节一致** |
| tdefl（压缩） | 不可用（P4 的 ROM miniz 返回 0）⇒ 设备上无法现场压 zip |
| 用户电脑压的 zip（data descriptor + `__MACOSX` 垃圾条目） | 走中央目录回退成功：`4324456 -> 8388608 bytes, method 8`，进游戏 ✓ |
| stored zip（method=0） | `ok=1 out=8388608B crc=22EFD2B9`（与真 ROM 逐字节一致） |
| 重复加载 / 释放 | 连续 3 次加载 + 2 次释放，PSRAM 余量 **19652 KB → 19652 KB → 19652 KB**（无泄漏、无双释放） |
| CRC 校验 | 8 MB 解压后 CRC 通过 |
| 游戏运行 | 5281 帧、丢块 0 |
| **进游戏耗时（8 MB 的晓月）** | 裸 `.gba` **9.23 s** ／ 压缩到 4.3 MB 的 `.zip` **5.94 s** ⇒ **快 ~36%**（瓶颈是 SD 读：本板实测 ≈ 0.86 MB/s） |

## 已知限制

- ROM 缓冲仍受预算约束（空闲 PSRAM − 8 MB ⇒ 真机约 19 MB）：**16 MB 的 ROM 能进，32 MB 的进不去**（与是否 zip 无关，裸文件也一样）。
- 一个 zip 只取**第一个可用条目**（与上游及其余 11 个系统口径一致）：zip 里放一个 ROM，不要套文件夹。
- 加密 zip、ZIP64（>4 GB 或 ZIP64 扩展）不支持，会明确报错。
- 首次进游戏的解压是**一次性**开销；压缩比越高、进游戏越快（因为瓶颈是 SD 读而不是 CPU）。
