# Candidate 16 final ELF review

Image 5ee07606d05f3ba246a62b0c48bcac19d46f3dd071fee2adb6e3d229de543e34.
ELF 88e444ae7b8d331b0be7e2cd814e4d7cbeffac0841c636e3f89177ebdb278810.
Identity C9:9E:3C:DD:5A:CB / OpenVela Mouse-3CDD5ACB.

Reviewed complete XIP-PREVIOUS-DIFF against the installed C14. Only
segment sizes/boundaries and internal data-context relocations differ.
Text end 0x48384a80 -> 0x48385280; rodata end 0x489d71b8 -> 0x489d73b8.
The s_mmu_ctx, s_psram_ctx and tuning table move +16 bytes to 0x4ff6aa48,
0x4ff6aaa0 and 0x4ff6ada4. Every field offset, memory operation, branch,
copy order and PMP/two-core setup remains unchanged. Four indirect jalr
and one tail jr keep their exact internal instruction addresses/table
slots, and all eight calibration callback targets retain their addresses.
Six early PSRAM references remain boundary arithmetic, with no early
PSRAM instruction call. SPI1 cache-on mutex/entry/exit are unchanged.
Text Flash 0x680000 -> VMA 0x48000000; rodata Flash 0x30020 -> 0x48390020.
FNV text e15077f6 and rodata 82ef2ebf. BSS end 0x4ffad204 < 0x4ffaefc0.
Application offset 0x2000 and complete data boundary 0xc00000 are retained.

Ten production files differ: central/host accounting/shared snapshots,
UI/status/CLI, QPK owned-session cleanup/JSON and capability catalog.
Only resolved-config difference is NIMBLE_BLE_MAX_CONN 2 -> 3; the linked
NimBLE connection pool is independently verified as 336 bytes/capacity 3.
Large per-link reply/notification state stays in initialized user heap.
C6 firmware, NAT/XIP SDK inputs, SDIO and JPEG configuration are unchanged.
Rejected IRQ/autosuspend/reload experiments and security provisioning stay
disabled. No eFuse or user-data layout change is authorized or performed.

14 host groups pass. Host multi-link callbacks/GATT IO are substituted;
physical multi-peer radio traffic is not established by these results.
Physical screen and cold power-cycle acceptance are not inferred.
Existing compact source formatting is not claimed checkpatch-clean.
App-only flash uses installed C14 for rollback, not undeployed C15.
