# ── P2-5：从 BIOS 二进制生成 C 数组头（消除"同一份 BIOS 在仓库里存两份"）───────────
# 调用：cmake -DGBSP_BIOS_BIN=<open_gba_bios.bin> -DGBSP_BIOS_HDR=<out.h> -P gen_bios_header.cmake
#
# 事实来源链（唯一）：
#     bios/source/ + bios/Makefile  ──(devkitARM)──▶  bios/open_gba_bios.bin  ──▶  本脚本  ──▶  bios.h（构建期产物）
# 仓库里**不再提交** bios.h；gbsp/main/main.c 的兜底路径（SD 卡没有 gba_bios.bin 时）用生成的数组。
#
# 为什么要门禁：换 BIOS 是"看得见的行为变更"，一旦 .bin 被换掉而没人发现，
# 表现是"某些游戏的行为悄悄变了"。所以这里 pin 住 md5，不符就**构建失败**。

if(NOT DEFINED GBSP_BIOS_BIN OR NOT DEFINED GBSP_BIOS_HDR)
    message(FATAL_ERROR "用法：cmake -DGBSP_BIOS_BIN=... -DGBSP_BIOS_HDR=... -P gen_bios_header.cmake")
endif()
if(NOT EXISTS "${GBSP_BIOS_BIN}")
    message(FATAL_ERROR "找不到 BIOS 源文件：${GBSP_BIOS_BIN}")
endif()

# ── 门禁①：源文件 md5 必须等于 pin 值（来源漂移立刻失败，不静默换 BIOS）
set(GBSP_BIOS_MD5_EXPECTED "1876f71b0d8c65eef3454547896ffb11")
file(MD5 "${GBSP_BIOS_BIN}" bios_md5)
if(NOT bios_md5 STREQUAL GBSP_BIOS_MD5_EXPECTED)
    message(FATAL_ERROR
        "BIOS md5 漂移：实际 ${bios_md5} ≠ 期望 ${GBSP_BIOS_MD5_EXPECTED}\n"
        "  源文件：${GBSP_BIOS_BIN}\n"
        "  若确实要换 BIOS：先用 bios/Makefile 重建 .bin，再更新本脚本里的 pin 值，并在 CREDITS.md 说明。")
endif()

file(READ "${GBSP_BIOS_BIN}" hex HEX)
string(LENGTH "${hex}" hexlen)
math(EXPR nbytes "${hexlen} / 2")
# 每字节写成 0xNN,，再每 16 字节断一行（可读性；行宽与旧的手写文件一致）
string(REGEX REPLACE "(..)" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){16})" "\\1\n    " pretty "${bytes}")

file(WRITE "${GBSP_BIOS_HDR}"
"/* 本文件由 CMake 在构建期生成（P2-5）—— **不要提交、不要手改**。
 * 源：components/gbsp-libretro/bios/open_gba_bios.bin
 *     md5 ${bios_md5}（已 pin，见 gen_bios_header.cmake）
 *     ${nbytes} 字节
 * 事实来源链：bios/source/ + bios/Makefile → open_gba_bios.bin → 本头文件。
 * 用途：SD 卡上不存在 gba_bios.bin 时的兜底 BIOS（gbsp/main/main.c）。
 * 生成器：gbsp/main/gen_bios_header.cmake */
const unsigned char open_gba_bios_rom[] = {
    ${pretty}
};
")

# ── 门禁②：把刚写出的文件读回来数一遍 —— 字节数必须与源一致（生成逻辑回归即失败）
file(READ "${GBSP_BIOS_HDR}" back)
string(REGEX MATCHALL "0x[0-9a-f][0-9a-f]" toks "${back}")
list(LENGTH toks ntoks)
if(NOT ntoks EQUAL nbytes)
    message(FATAL_ERROR "生成的数组字节数 ${ntoks} ≠ 源 ${nbytes}（生成器回归）")
endif()
