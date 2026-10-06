/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef COMMON_H
#define COMMON_H

#define ror(dest, value, shift)                                               \
  dest = ((value) >> (shift)) | ((value) << (32 - (shift)))                   \

#define MAX(a,b)  ((a) > (b) ? (a) : (b))
#define MIN(a,b)  ((a) < (b) ? (a) : (b))

#if defined(_WIN32)
  #define PATH_SEPARATOR "\\"
  #define PATH_SEPARATOR_CHAR '\\'
#else
  #define PATH_SEPARATOR "/"
  #define PATH_SEPARATOR_CHAR '/'
#endif

/* On x86 we pass arguments via registers instead of stack */
#ifdef X86_ARCH
  #define function_cc __attribute__((regparm(2)))
#else
  #define function_cc
#endif

#ifdef ARM_ARCH

#define _BSD_SOURCE // sync
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <sys/time.h>

#endif /* ARM_ARCH */

// Huge thanks to pollux for the heads up on using native file I/O
// functions on PSP for vastly improved memstick performance.

#ifdef PSP
  #include <pspkernel.h>
  #include <pspdebug.h>
  #include <pspctrl.h>
  #include <pspgu.h>
  #include <pspaudio.h>
  #include <pspaudiolib.h>
  #include <psprtc.h>
  #include <time.h>
#else
  typedef unsigned char u8;
  typedef signed char s8;
  typedef unsigned short int u16;
  typedef signed short int s16;
  typedef unsigned int u32;
  typedef signed int s32;
  typedef unsigned long long int u64;
  typedef signed long long int s64;
#endif

#if defined(USE_XBGR1555_FORMAT)
  #define convert_palette(value)  \
    (value & 0x7FFF)
#else
  #define convert_palette(value) \
    (((value & 0x1F) << 11) | ((value & 0x03E0) << 1) | ((value >> 10) & 0x1F))
#endif

#define GBA_SCREEN_WIDTH  (240)
#define GBA_SCREEN_HEIGHT (160)
#define GBA_SCREEN_PITCH  (240)

// The buffer is 16 bit color depth.
// We reserve extra memory at the end for extra effects (winobj rendering).
#define GBA_SCREEN_BUFFER_SIZE  \
  (GBA_SCREEN_PITCH * (GBA_SCREEN_HEIGHT + 1) * sizeof(uint16_t))

#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  #define netorder32(value) (value)
#else
  #define netorder32(value) __builtin_bswap32(value)
#endif

typedef u32 fixed16_16;
typedef u32 fixed8_24;

#define float_to_fp16_16(value)                                               \
  (fixed16_16)((value) * 65536.0)                                             \

#define fp16_16_to_float(value)                                               \
  (float)((value) / 65536.0)                                                  \

#define u32_to_fp16_16(value)                                                 \
  ((value) << 16)                                                             \

#define fp16_16_to_u32(value)                                                 \
  ((value) >> 16)                                                             \

#define fp16_16_fractional_part(value)                                        \
  ((value) & 0xFFFF)                                                          \

#define float_to_fp8_24(value)                                                \
  (fixed8_24)((value) * 16777216.0)                                           \

#define fp8_24_fractional_part(value)                                         \
  ((value) & 0xFFFFFF)                                                        \

#define fixed_div(numerator, denominator, bits)                               \
  (((numerator * (1 << bits)) + (denominator / 2)) / denominator)             \

#define address8(base, offset)                                                \
  *((u8 *)((u8 *)base + (offset)))                                            \

#define address16(base, offset)                                               \
  *((u16 *)((u8 *)base + (offset)))                                           \

#define address32(base, offset)                                               \
  *((u32 *)((u8 *)base + (offset)))                                           \

#define eswap8(value)  (value)
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  #define eswap16(value) __builtin_bswap16(value)
  #define eswap32(value) __builtin_bswap32(value)
#else
  #define eswap16(value) (value)
  #define eswap32(value) (value)
#endif

#define  readaddress8(base, offset) eswap8( address8( base, offset))
#define readaddress16(base, offset) eswap16(address16(base, offset))
#define readaddress32(base, offset) eswap32(address32(base, offset))

#define read_ioreg(regnum) (eswap16(io_registers[(regnum)]))
#define write_ioreg(regnum, val) io_registers[(regnum)] = eswap16(val)
#define read_ioreg32(regnum) (read_ioreg(regnum) | (read_ioreg((regnum)+1) << 16))

#define read_dmareg(regnum, dmachan) (eswap16(io_registers[(regnum) + (dmachan) * 6]))
#define write_dmareg(regnum, dmachan, val) io_registers[(regnum) + (dmachan) * 6] = eswap16(val)

#include <rg_system.h>
#include <esp_attr.h>
#include <unistd.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "cpu.h"
#include "gba_memory.h"
#include "savestate.h"
#include "video.h"
#include "input.h"
#include "sound.h"
#include "main.h"
#include "cheats.h"
#include "serial.h"

/* ── 宿主专用 GBA 侧探针（真机一行都不编，零影响）──────────────────────────
 * 用法：RG_GBA_TRACE=/tmp/trace.txt 跑宿主 gbsp → 键/中断事件逐行写文件（每行 fflush，
 *       进程被强杀也不丢）。存在的理由：只看画面分不清下面三件事，这三件事必须分开证：
 *         INPUT   = 我们的输入层有没有把键送到核心
 *         P1CNT   = 游戏有没有设置键盘中断（写 KEYCNT=0x04000132）
 *         TRIGGER = 模拟器认不认这个设置、有没有抬 IRQ_KEYPAD
 *       三者交叉就能定位"名字/存档界面无法交互"到底是哪一层。
 * 历史坑：ESP-IDF 的 esp_log 不支持 %lld/%f（会让后面参数错位），所以这里一律整数格式化。 */
#ifndef RG_GBA_TRACE_HOOK
#define RG_GBA_TRACE_HOOK
#if !defined(ESP_PLATFORM)
#include <stdio.h>
static FILE *rg_trace_fp(void)
{
  static FILE *fp = NULL;
  static int state = 0;
  if (!state)
  {
    const char *p = getenv("RG_GBA_TRACE");
    state = p ? 1 : -1;
    if (p) fp = fopen(p, "w");
  }
  return (state == 1) ? fp : NULL;
}
#define RG_TRACE(...) do { FILE *f_ = rg_trace_fp(); if (f_) { fprintf(f_, __VA_ARGS__); fflush(f_); } } while (0)
#else
#define RG_TRACE(...) do {} while (0)
#endif
#endif

#endif
