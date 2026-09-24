
#include <stdint.h>
#include <stdbool.h>

#include "memmap.h"

// The JIT cache buffer is allocated via mmap (or win equivalent) so that it
// can be RWX. On top of that, we need the bufer to be "close" to the text
// segment, so that we can perform jumps between the two code blocks.
// Android and some other platforms discourage the usage of sections in the
// binary (ie. on-disk ELF) that are marked as executable and writtable for
// security reasons. Therefore we prefer to use mmap even though it can be
// tricky to map correctly.

// To map a block close to the code, we take the function address as a proxy
// of the text section address, and try to map the cache next to it. This is
// an iterative process of trial and error that is hopefully successful.

// Jump/Call offset requirements:
// x86-64 has a +/- 2GB offset requirement.
// ARM64 has a +/-128MB offset requirement.
// ARM32 has a +/- 32MB offset requirement (gpsp does not require this).
// MIPS requires blocks to be within the same 256MB boundary (identical 4 MSB)

#ifdef MMAP_JIT_CACHE

#if defined(ESP_PLATFORM)
/* ESP32-P4：JIT 代码必须能被执行，而 heap_caps_malloc 只能拿到"数据内存"。
 * 做法（照 Irak4t0n/HowBoyAdvance 的 gpsp_esp.c，GPLv2，其 P4 实机验证可用）：
 *   ① 在 PSRAM 上按 MMU 页对齐分配一段（此时是普通数据内存）
 *   ② 取它的物理地址
 *   ③ esp_mmu_map 把同一段物理页以 MMU_MEM_CAP_EXEC 再映射一份（PADDR_SHARED）
 * 返回的 exec 映射同时挂在 IBUS/DBUS 上，所以**读、写、执行都用这一个指针**。
 * 注意：caps 检查不允许 EXEC+WRITE 组合，所以这里只能申请 EXEC。 */
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_log.h"

static void *jit_exec_base;
static size_t jit_exec_size;

void *map_jit_block(unsigned size) {
    if (jit_exec_base)
        return jit_exec_base;

    const size_t page = CONFIG_MMU_PAGE_SIZE;
    size_t total = (size + page - 1) & ~(page - 1);

    uint8_t *data = (uint8_t *)heap_caps_aligned_alloc(page, total, MALLOC_CAP_SPIRAM);
    if (!data) {
        ESP_LOGE("jit", "JIT buffer alloc failed (%u bytes)", (unsigned)total);
        return 0;
    }

    esp_paddr_t paddr = 0;
    mmu_target_t target = 0;
    esp_err_t err = esp_mmu_vaddr_to_paddr(data, &paddr, &target);
    if (err != ESP_OK) {
        ESP_LOGE("jit", "vaddr_to_paddr failed: %s", esp_err_to_name(err));
        free(data);
        return 0;
    }

    void *base = NULL;
    err = esp_mmu_map(paddr, total, MMU_TARGET_PSRAM0, MMU_MEM_CAP_EXEC,
                      ESP_MMU_MMAP_FLAG_PADDR_SHARED, &base);
    if (err != ESP_OK) {
        ESP_LOGE("jit", "esp_mmu_map(EXEC) failed (%d bytes): %s",
                 (int)total, esp_err_to_name(err));
        free(data);
        return 0;
    }

    jit_exec_base = base;
    jit_exec_size = total;
    ESP_LOGI("jit", "JIT cache mapped: %u bytes, data=%p exec=%p",
             (unsigned)total, data, base);
    return base;
}

void unmap_jit_block(void *bufptr, unsigned size) {
    /* JIT 缓存全程存活，不做回收 */
    (void)bufptr;
    (void)size;
}
#else /* !ESP_PLATFORM */

// JIT block requirements translated to allocation code.
#if defined(MIPS_ARCH)
  #define _MAP_ITERATIONS           1024   // Test -/+2GB in 4MB steps
  #define _MAP_STEP         (4*1024*1024)
  #define _VALIDATE_BLOCK_FN(ptr, size) \
          validate_addr_section_mips(ptr, size)
#elif defined(ARM64_ARCH)
  #define _MAP_ITERATIONS            256   // Test -/+128MB in 1MB steps
  #define _MAP_STEP           (1024*1024)
  #define _VALIDATE_BLOCK_FN(ptr, size) \
          validate_addr_offset(ptr, size, 128)
#else
  #define _MAP_ITERATIONS           1024   // Test -/+2GB in 4MB steps
  #define _MAP_STEP         (4*1024*1024)
  #define _VALIDATE_BLOCK_FN(ptr, size) \
          validate_addr_offset(ptr, size, 2048)
#endif

bool validate_addr_offset(void *ptr, unsigned size, unsigned max_offset_mb) {
	// Ensure that the start and the end of the block is not too far away.
	uintptr_t ref_addr = (uintptr_t)(map_jit_block);
	uintptr_t start_addr = (uintptr_t)ptr;
	uintptr_t end_addr = start_addr + size;
	uintptr_t dist1 = start_addr > ref_addr ? start_addr - ref_addr
	                                        : ref_addr - start_addr;
	uintptr_t dist2 = end_addr > ref_addr ? end_addr - ref_addr
	                                      : ref_addr - end_addr;

	return dist1 < max_offset_mb * 1024 * 1024 &&
	       dist2 < max_offset_mb * 1024 * 1024;
}

bool validate_addr_section_mips(void *ptr, unsigned size, unsigned max_offset_mb) {
	uintptr_t ref_addr = (uintptr_t)(map_jit_block);
	uintptr_t start_addr = (uintptr_t)ptr;
	uintptr_t end_addr = start_addr + size;
	const uintptr_t msk = ~0xFFFFFFFU;  // 256MB block

	return (ref_addr & msk) == (start_addr & msk) &&
	       (ref_addr & msk) == (end_addr & msk);
}

#ifdef WIN32

	#include <windows.h>
	#include <io.h>

	void *map_jit_block(unsigned size) {
		unsigned i;
		uintptr_t base = (uintptr_t)(map_jit_block) & (~(_MAP_STEP - 1ULL));
		for (i = 0; i < _MAP_ITERATIONS; i++) {
			int offset = ((i & 1) ? 1 : -1) * (i >> 1) * _MAP_STEP;
			uintptr_t baddr = base + (intptr_t)offset;
			if (!baddr)
				continue;    // Do not map NULL, bad things happen :)

			void *p = VirtualAlloc((void*)baddr, size, MEM_COMMIT|MEM_RESERVE, PAGE_EXECUTE_READWRITE);
			if (p) {
				if (_VALIDATE_BLOCK_FN(p, size))
					return p;

				VirtualFree(p, 0, MEM_RELEASE);
			}

		}
		return 0;
	}

	void unmap_jit_block(void *bufptr, unsigned size) {
		VirtualFree(bufptr, 0, MEM_RELEASE);
	}

#else

	#include <sys/mman.h>

	// Posix implementation
	void *map_jit_block(unsigned size) {
		unsigned i;
		uintptr_t base = (uintptr_t)(map_jit_block) & (~(_MAP_STEP - 1ULL));
		for (i = 0; i < _MAP_ITERATIONS; i++) {
			int offset = ((i & 1) ? 1 : -1) * (i >> 1) * _MAP_STEP;
			uintptr_t baddr = base + (intptr_t)offset;
			if (!baddr)
				continue;    // Do not map NULL, bad things happen :)

			void *p = mmap((void*)baddr, size, PROT_READ|PROT_WRITE|PROT_EXEC,
			                                   MAP_ANON|MAP_PRIVATE, -1, 0);
			if (p) {
				if (_VALIDATE_BLOCK_FN(p, size))
					return p;

				munmap(p, size);
			}
		}
		return 0;
	}

	void unmap_jit_block(void *bufptr, unsigned size) {
		munmap(bufptr, size);
	}

#endif /* WIN32 */

#endif /* !ESP_PLATFORM */

#endif /* MMAP_JIT_CACHE */

