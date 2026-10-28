#!/usr/bin/env python3
"""Add a bounded native-module text heap to an explicit prepared P4 XIP tree.

Does not enable XIP or change normal user-heap execute permissions. No flash.
"""
from pathlib import Path

def once(text, before, after):
    if text.count(before) != 1:
        raise RuntimeError('unsupported prepared source: '+before[:80])
    return text.replace(before, after)

def apply(nuttx):
    nuttx=Path(nuttx)
    if 'CONFIG_ESP32P4_PSRAM_XIP=y' not in (nuttx/'.config').read_text():
        raise RuntimeError('requires an explicitly prepared XIP baseline; never auto-enables XIP')
    kconfig=nuttx/'arch/risc-v/src/esp32p4/Kconfig'
    s=kconfig.read_text()
    if 'config OV_BROWSER_TEXT_POOL' not in s:
        s+='''
config OV_BROWSER_TEXT_POOL
    bool "Reserved native browser executable pool"
    default n
    depends on ESP32P4_PSRAM_XIP && ESPRESSIF_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
    select ARCH_HAVE_TEXT_HEAP
    select ARCH_USE_TEXT_HEAP
    select MODULE
    select LIBC_ELF
'''
        kconfig.write_text(s)
    linker=nuttx/'boards/risc-v/esp32p4/common/scripts/esp32p4_sections.rev3.ld'
    s=linker.read_text()
    if '_browser_exec_start' not in s:
        s=once(s,'    _ext_ram_bss_start = ABSOLUTE(.);', '''    _ext_ram_bss_start = ABSOLUTE(.);
#ifdef CONFIG_OV_BROWSER_TEXT_POOL
    /* Reserve before ordinary BSS; PSRAM allocator excludes this entire span. */
    _browser_exec_start = ABSOLUTE(.);
    . += 0x300000;
    _browser_exec_end = ABSOLUTE(.);
    ASSERT((_browser_exec_start & 0xffff) == 0, "module pool alignment");
#endif''')
        linker.write_text(s)
    s=linker.read_text()
    s=s.replace('ASSERT((_browser_exec_start & 0xffff) == 0, "module pool alignment")\n',
                'ASSERT((_browser_exec_start & 0xffff) == 0, "module pool alignment");\n')
    linker.write_text(s)
    sdk=nuttx/'arch/risc-v/src/esp32p4/esp-hal-3rdparty'
    protection=sdk/'components/esp_hw_support/port/esp32p4/cpu_region_protect.c'
    s=protection.read_text()
    if 'OV_BROWSER_TEXT_POOL' not in s:
        before='    PMP_ENTRY_SET_CACHED_AND_UNCACHED(14, 19, esp_psram_extram_vaddr_end(), PMP_TOR | RW);'
        s=once(s,before,'''#ifdef CONFIG_OV_BROWSER_TEXT_POOL
    /* Existing XIP code/rodata permissions stay intact. Only the reserved
     * pool gets X; cached ordinary BSS/heap and all uncached heap stay RW.
     * Entry 23 is unused in the prepared v3 table. Its TOR lower bound is
     * entry 22; higher-priority entry 14 covers the native module pool. */
    extern unsigned char _browser_exec_end[];
    PMP_RESET_AND_ENTRY_SET(14, (uint32_t)_browser_exec_end, PMP_TOR | RWX);
    PMP_RESET_AND_ENTRY_SET(19, CACHE_LL_L2MEM_NON_CACHE_ADDR(esp_psram_extram_vaddr_end()), PMP_TOR | RW);
    PMP_RESET_AND_ENTRY_SET(23, esp_psram_extram_vaddr_end(), PMP_TOR | RW);
#else
'''+before+'\n#endif')
        protection.write_text(s)
    verify=nuttx/'arch/risc-v/src/esp32p4/esp32p4_psram_xip.c'
    s=verify.read_text()
    if 'OV_BROWSER_TEXT_POOL' not in s:
        before='  __asm__ __volatile__("csrr %0, pmpaddr14" : "=r"(heap_end));'
        s=once(s,before,'''#ifdef CONFIG_OV_BROWSER_TEXT_POOL
  uintptr_t module_end, cfg5;
  extern unsigned char _browser_exec_end[];
  __asm__ __volatile__("csrr %0, pmpaddr14" : "=r"(module_end));
  __asm__ __volatile__("csrr %0, pmpaddr23" : "=r"(heap_end));
  __asm__ __volatile__("csrr %0, pmpcfg5" : "=r"(cfg5));
  if ((module_end << 2) != (uintptr_t)_browser_exec_end ||
      ((cfg5 >> 24) & 0x9f) != 0x8b) xip_fail();
#else
'''+before+'\n#endif')
        s=once(s,'      ((cfg3 >> 16) & 0x9f) != 0x8b || cpu > 1)', '''#ifdef CONFIG_OV_BROWSER_TEXT_POOL
      ((cfg3 >> 16) & 0x9f) != 0x8f || cpu > 1)
#else
      ((cfg3 >> 16) & 0x9f) != 0x8b || cpu > 1)
#endif''')
        before='  atomic_fetch_or_explicit(&g_protected_cpus, 1u << cpu,'
        s=once(s,before,'''#ifdef CONFIG_OV_BROWSER_TEXT_POOL
  esp_rom_printf("[browser-exec] cpu=%lu pool_end=%08lx heap_end=%08lx pool=8f heap=8b\\n",
                 (unsigned long)cpu, (unsigned long)(module_end << 2), (unsigned long)(heap_end << 2));
#endif
'''+before)
        verify.write_text(s)
    return [kconfig,linker,protection,verify]
