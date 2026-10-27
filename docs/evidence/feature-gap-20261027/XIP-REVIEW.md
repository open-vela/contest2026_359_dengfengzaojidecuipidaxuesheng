# Candidate 12 final ELF review

Image d59370b63de3f2eca8e6445e1fa627777b566e9843d1ccc45b45942c4f03bb85.
ELF 4e13ead8ef628b3948e3280d932688748ab7e3d6961b429f459f971bbfcd0bee.
BLE F3:52:3C:75:04:A7 / OpenVela Mouse-3C7504A7.

Reviewed the complete XIP-PREVIOUS-DIFF.txt against C11. Changes form updated
text size/end 0x383c00/0x48383c00, rodata size/end 0x646e58/0x489d6e78,
and internal data relocations: s_mmu_ctx 0x4ff6aa48 -> 0x4ff6aa38,
s_psram_ctx 0x4ff6aaa0 -> 0x4ff6aa90 and tuning configuration table
0x4ff6ada4 -> 0x4ff6ad94. Their field offsets and memory operations are
unchanged and remain in internal memory covered by startup initialization.
The store annotation relative to rodata end changes without changing that
store instruction. No branch, direct call, table slot or copy order changes.

Four indirect jalr and one indirect tail jr remain at identical internal
instruction addresses. All eight tuning callbacks retain their internal
addresses and unchanged direct-call closure. The tuning table accesses still
use the same offsets, now relative to its correctly relocated internal base.
Six pre-copy PSRAM references remain boundary calculations, not PSRAM code
targets. Both CPUs' PMP initialization, SDK SPI1 cache-on mutex and internal
stack dispatcher remain unchanged. Host XIP verifier and SDK mutex tests pass.

The final ELF audit verifies real image offsets, boot copies, BSS/heap and
partition boundaries. BSS 0x4ffad154 remains below 0x4ffaefc0. Text Flash
0x680000 -> VMA 0x48000000, rodata Flash 0x30020 -> VMA 0x48390020.
Expected FNV hashes are 080960f9 / 65bec803. Application starts at 0x2000
and ends below 0xc00000; the 4 MiB data layout is unchanged.

Exactly twelve overlay files differ from C11, restricted to BLE role/lifecycle,
UI admission/status and the P4 network adapter diagnostics. Resolved config,
native NAT/XIP input sources, JPEG, C6 image, SDIO clock and scheduler match
C11. Flash autosuspend and DSI reload remain disabled, as do provisioning and
encryption options. No rejected cache-off IRQ prototype is re-enabled.

This image is built and audited only. It has not been written to a device.
The recorded C10 large-transfer checksum regression remains unresolved and
the 5 Mbps goal is not claimed. No prior device result is inherited.
