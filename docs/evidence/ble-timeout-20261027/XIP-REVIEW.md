# Candidate 19 final linked XIP review

Image b6b17eed6481edfa4ba977a2c19a5930452081281b0ae585f44a039c8ca39b0f; ELF 574c8fad502bbb1c7bf31aefb08b8887bb90e18d00a5f100a91723d577ac428c. Reviewed the complete focused disassembly diff against installed candidate 18, with the independent source/configuration allowlist and ELF audit.

Only copy-size/end constants and internal data relocations changed in the early closure. Text end is 48386300, rodata end 489d7808 (crosses the 489d8000 lui/addi boundary); s_mmu_ctx, s_psram_ctx and s_tuning_cfg_drv move +0x10. The store at 4ff4ed88 has identical bytes and only a nearest-symbol comment difference. No new branch, indirect target, load/store width or lock order appears in the focused diff. SPI1 mutex code is unchanged.

The same four indirect jalr plus tail jr dispatch the eight original internal calibration callbacks at identical addresses. Six early PSRAM references are linker segment/copy bounds; no pre-copy PSRAM execution. SDK XIP sources and all configuration are unchanged. NPL structure sizes change only in the fully rebuilt host, not before-copy code.

BSS 4ffad2b0 remains below 4ffaefc0; app-only offset 2000 stays below c00000. Preserve all data c00000..1000000. Expected copied text/rodata FNV d536b33c / ccf853f1. IRQ cache-off, flash auto-suspend and DSI reload experiments remain disabled. This is source/linked-code review, not cold-power or physical-display acceptance.
