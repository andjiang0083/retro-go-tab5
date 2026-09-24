/* macOS/SDL2 宿主垫片：ESP-IDF 的 <esp_attr.h> 不存在于宿主编译环境。
 * 这些属性（IRAM/DRAM/EXT_RAM 段放置、对齐）在宿主上无意义，全部定义为空即可。
 * 由 tools/build_sdl2_mac.sh 通过 -Itools/sdl2-compat 注入，避免改动上游源码。 */
#ifndef RG_SDL2_COMPAT_ESP_ATTR_H
#define RG_SDL2_COMPAT_ESP_ATTR_H

#define EXT_RAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_NOINIT_ATTR
#define IRAM_ATTR
#define IRAM_ATTR_ISR
#define DRAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define RTC_FAST_ATTR
#define RTC_SLOW_ATTR
#define RTC_RODATA_ATTR
#define WORD_ALIGNED_ATTR
#define DMA_ATTR
#define __NOINIT_ATTR
#define __IRAM_ATTR
#define __DRAM_ATTR
#define __RTC_DATA_ATTR

#endif
