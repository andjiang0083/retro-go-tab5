# dynarec 开关（默认开）。关掉时：
#   - 不编 riscv/（那是 dynarec 的寄存器桩，会和 cpu.cpp 的解释器定义重复）
#   - gpsp_memory_alloc.c 的全局定义自动被 HAVE_DYNAREC 屏蔽
# 2026-10-06 真因已修（v0.4.4）：dynarec 的「周期记账口径」与解释器不一致 ——
#   解释器按 ws_cyc_nseq/ws_cyc_seq[区域][索引] 动态扣费，dynarec 写死常数 ⇒
#   同一段重活（LZ77 解压/关卡加载）跨的 VBlank 帧数差 1 ⇒ 游戏自身计时相位永久错 1 帧
#   ⇒ 按键窗口整体错位（issue #3）。修法：dynarec 逐口径对齐解释器，见 riscv/riscv_emit.h。
#   真机验证：dynarec 下按键正确 + 59.7fps（解释器仅 34.7fps）。
option(RG_GBSP_DYNAREC "启用 RISC-V dynarec（默认开；周期记账口径已对齐解释器，v0.4.4 修复）" ON)
if(RG_GBSP_DYNAREC)
    set(COMPONENT_SRCDIRS "." "riscv")
    set(RG_GBSP_DYNAREC_FLAGS -DHAVE_DYNAREC -DHAVE_JIT -DRISCV_ARCH)
else()
    set(COMPONENT_SRCDIRS ".")
    set(RG_GBSP_DYNAREC_FLAGS "")
endif()
set(COMPONENT_ADD_INCLUDEDIRS ". libretro/libretro-common/include")
set(COMPONENT_REQUIRES "retro-go esp_mm heap")
register_component()
rg_setup_compile_options(
    # 2026-10-06 实验 E：对齐上游 HowBoyAdvance 的 gpsp 组件选项。
    # 上游用 -Oz -fno-exceptions -fno-rtti；我们之前是 -O3 -ffast-math。
    # ⚠ 当时怀疑「-O3 的激进优化编坏 JIT 翻译器（往缓冲区写机器码/函数指针强转/自修改结构）」，
    #   **该假设已被否掉**（真因是周期记账口径不一致）。
    #   先保留 -Oz：v0.4.4 的修复是在这套选项下真机验证通过的；-O3 是否更快待复测。
    -Oz
    -fno-exceptions -fno-rtti
    -fomit-frame-pointer
    -DOVERCLOCK_60FPS
    ${RG_GBSP_DYNAREC_FLAGS}
    # ↓ RISC-V dynarec（后端移植自 Irak4t0n/HowBoyAdvance，GPLv2）
    #   HAVE_DYNAREC   启用动态重编译
    #   RISCV_ARCH     选 riscv/ 后端
    #   MMAP_JIT_CACHE JIT 缓存走 map_jit_block（P4: esp_mmu_map 可执行映射）
    #   SMALL_TRANSLATION_CACHE 缓存 1MB ROM + 256KB RAM（默认 10MB 太大）
    #   -msmall-data-limit=0 防止 .sbss 溢出（HowBoyAdvance 实测必需）
    # ⚠⚠ 2026-10-06 真机 A/B 结论（**已修复，此段保留备查**）：
    #   现象：同一 ROM/输入/显示几何下，宿主(解释器)与真机(dynarec)的"游戏帧缓冲哈希"
    #         在开机约 5 秒（f=301，任何输入之前）分道扬镳；关掉 dynarec 后逐帧一致。
    #         用户可见症状：读键正确但菜单/光标不响应（issue #3）。
    #   真因：两引擎「周期记账口径」不一致 —— 解释器按区域表动态扣费，dynarec 写死常数
    #         ⇒ 同一段重活跨的 VBlank 帧数差 1 ⇒ 游戏自身计时相位永久错 1 帧。
    #   修法：dynarec 逐口径对齐解释器（访存运行时查表 / 取指改 ws_cyc_nseq / MUL 估算归零），
    #         见 riscv/riscv_emit.h + cpu_threaded.c。真机验证：按键正确 + 59.7fps。
    #   ⇒ 本开关默认 ON；如遇倒退可用 -DRG_GBSP_DYNAREC=OFF 回退到解释器。
    -DMMAP_JIT_CACHE -DSMALL_TRANSLATION_CACHE
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
