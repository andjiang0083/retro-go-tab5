/* macOS 兼容垫片：macOS 的 libc 没有独立的 <malloc.h>，malloc/free 声明在 <stdlib.h>。
 * 由 tools/build_sdl2_mac.sh 通过 -Itools/sdl2-compat 注入，避免改动上游源码。 */
#ifndef RG_SDL2_COMPAT_MALLOC_H
#define RG_SDL2_COMPAT_MALLOC_H
#include <stdlib.h>
#endif
