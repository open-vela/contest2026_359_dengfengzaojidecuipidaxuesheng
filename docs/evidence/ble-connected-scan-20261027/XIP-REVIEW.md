# Candidate 14 final ELF review

Image fde34fcd6ced46fa8c1dc7b8b10f14a73114495dfe115a9b8f53ccf23bf12011.
ELF 6330a321f847269f71115286d544426143719a4f680670738f7c1a81378eaa81.
BLE D3:38:F6:39:04:87 / OpenVela Mouse-F6390487.

Reviewed the full XIP-PREVIOUS-DIFF against C13. Text end moves from
0x48384300 to 0x48384a80; rodata end 0x489d6e78 to 0x489d71b8.
These are copy/mapping sizes and boundaries, not early dereferences.
The internal s_mmu_ctx/s_psram_ctx/tuning table relocate -16 bytes to
0x4ff6aa38 / 0x4ff6aa90 / 0x4ff6ad94. Field offsets, loads/stores,
control flow, copy order, PMP and both-core setup remain unchanged.
Four indirect jalr and one tail jr retain exact internal addresses,
table slots and all eight internal calibration callback addresses.
No pre-copy instruction target is in PSRAM. Six data references remain
boundary calculations. SPI1 cache-on mutex/entry/exit are unchanged.

Text Flash 0x680000 -> VMA 0x48000000; rodata Flash 0x30020 ->
VMA 0x48390020. FNV text e45e2f81, rodata 049c3da8.
BSS end 0x4ffad15c is below 0x4ffaefc0. Application offset 0x2000
and complete 4 MiB data boundary 0xc00000 are preserved.

Six changed production files relative to C13 contain BLE discovery,
scan lifecycle, diagnostic CLI and associated UI/header changes only.
Resolved config, XIP SDK source hashes, NAT, C6 and SDIO match C13.
Rejected IRQ/autosuspend/reload experiments remain disabled. No eFuse,
secure boot or flash encryption provisioning is enabled.

This review does not establish physical display, multi-peer concurrent
connections or cold power-cycle acceptance. Host substitutions are
reported separately. Whole-file style is not claimed clean; existing
compact formatting remains. Application-only flash uses the installed
C11 image for rollback; undeployed C12/C13 are not rollback baselines.
