/* Large gpSP memory arrays placed in PSRAM via EXT_RAM_BSS_ATTR.
   These would otherwise overflow internal SRAM BSS. */
#include "common.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"

/* reg[64], spsr[6], reg_mode[7][7] are defined in riscv_stub.S
   (or mips_stub.S) with guaranteed contiguous layout for asm offsets */
EXT_RAM_BSS_ATTR u8 *memory_map_read[8 * 1024];
EXT_RAM_BSS_ATTR u16 oam_ram[512];
EXT_RAM_BSS_ATTR u16 palette_ram[512];
EXT_RAM_BSS_ATTR u16 palette_ram_converted[512];
EXT_RAM_BSS_ATTR u8 ewram[1024 * 256 * 2];
EXT_RAM_BSS_ATTR u8 iwram[1024 * 32 * 2];
EXT_RAM_BSS_ATTR u8 vram[1024 * 96];
EXT_RAM_BSS_ATTR u16 io_registers[512];

/* From gba_memory.c — 我们那边已有定义，这里不重复（否则 multiple definition） */
/* EXT_RAM_BSS_ATTR u8 gamepak_backup[1024 * 128]; */

#ifdef HAVE_DYNAREC
/* Memory handler dispatch tables for dynarec (referenced by emit.h) */
/* tmemld[11][16]: load handlers, tmemst[4][16]: store handlers */
/* thnjal[15*16]: thumb handler jump table */
u32 tmemld[11][16];
u32 tmemst[4][16];
u32 thnjal[15 * 16];
#endif

#endif
