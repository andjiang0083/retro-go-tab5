set(COMPONENT_SRCDIRS "." "riscv")
set(COMPONENT_ADD_INCLUDEDIRS ". libretro/libretro-common/include")
set(COMPONENT_REQUIRES "retro-go esp_mm heap")
register_component()
rg_setup_compile_options(
    -O2
    -fomit-frame-pointer -ffast-math
    -DOVERCLOCK_60FPS
    # ↓ RISC-V dynarec（后端移植自 Irak4t0n/HowBoyAdvance，GPLv2）
    #   HAVE_DYNAREC   启用动态重编译
    #   RISCV_ARCH     选 riscv/ 后端
    #   MMAP_JIT_CACHE JIT 缓存走 map_jit_block（P4: esp_mmu_map 可执行映射）
    #   SMALL_TRANSLATION_CACHE 缓存 1MB ROM + 256KB RAM（默认 10MB 太大）
    #   -msmall-data-limit=0 防止 .sbss 溢出（HowBoyAdvance 实测必需）
    -DHAVE_DYNAREC -DRISCV_ARCH -DMMAP_JIT_CACHE -DSMALL_TRANSLATION_CACHE
    -msmall-data-limit=0
    -Wno-unused-variable
    -Wno-implicit-fallthrough
    -Wno-missing-field-initializers
    -Wno-error=unused-label
    -Wno-unused-label
)

# Dynarec 引入的 .flash.text / .flash_rodata_dummy 在同一 VMA（irom/drom 共用同一片 flash，
# 不同总线端口），--enable-non-contiguous-regions 会误判成重叠 → 按 HowBoyAdvance 的做法关掉该检查
idf_build_set_property(LINK_OPTIONS "-Wl,--no-check-sections" APPEND)
