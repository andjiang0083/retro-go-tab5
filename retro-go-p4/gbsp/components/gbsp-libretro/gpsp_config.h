
#ifndef GPSP_CONFIG_H
#define GPSP_CONFIG_H

#define GPSP_NAME                "gpSP"
#define GPSP_VERSION             "v1.0.0"
#define GPSP_NETPACKET_VERSION   "gpSP v1.0"

/* Default ROM buffer size in megabytes (this is a maximum value!)
 *
 * 2026-10-06：原为 2（仅 2MB），8MB 的 ROM 装不进时会被持续换页 ⇒ 改为 32
 * （对齐上游 HowBoyAdvance：按 PSRAM 上限尽量全装，ROM 常驻则不换页）。
 * 属性能/余量优化，**不是**按键失灵的原因（真因是周期记账口径不一致，
 * 见 gbsp-libretro/CMakeLists.txt 顶部）。
 * 实际上限由下面的 PSRAM 预算动态决定。 */
#ifndef ROM_BUFFER_SIZE
#define ROM_BUFFER_SIZE 32
#endif

/* Cache sizes and their config knobs */
#if defined(SMALL_TRANSLATION_CACHE)
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)
#else
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
#endif

/* Should be an upperbound to the maximum number of bytes a single JIT'ed
   instruction can take. STM/LDM are tipically the biggest ones */
#define TRANSLATION_CACHE_LIMIT_THRESHOLD (1024 * 2)

/* Hash table size for ROM trans cache lookups */
#define ROM_BRANCH_HASH_BITS                           16
#define ROM_BRANCH_HASH_SIZE   (1 << ROM_BRANCH_HASH_BITS)

/* RFU Multiplayer config, do not mess around too much with it */
#define MAX_RFU_NETPLAYERS       32

#endif
