# Candidate 13 final ELF review

Image 42fc7d5bea78ebb217fcc068a788cbe42a4b1f41dd75e4e129a0b248c4768836.
ELF 17ad00325777fa0f79b74b382f95dfca06a00f3210482e326713962334182969.
BLE E4:5D:41:76:8B:C1 / OpenVela Mouse-41768BC1.

Reviewed the complete XIP-PREVIOUS-DIFF.txt relative to C12. Startup text
size/end becomes 0x384300/0x48384300. RoData boundaries remain unchanged.
Internal context relocations are s_mmu_ctx 0x4ff6aa38 -> 0x4ff6aa48,
s_psram_ctx 0x4ff6aa90 -> 0x4ff6aaa0, and tuning table 0x4ff6ad94 ->
0x4ff6ada4. Every field offset and memory operation remains unchanged;
these contexts stay internal and within startup initialization bounds.
Four indirect jalr and one tail jr remain at identical internal addresses,
with the same table slots and eight calibration callback addresses/closure.

The additional SPI1-end diff relocates its existing call to
esp_system_get_time from 0x4804e44c to 0x4804e4b8. It remains a runtime
post-copy cache-on path; it is not a newly introduced pre-copy PSRAM call.
No call target identity, branch, copy order, PMP setup, mutex protocol or
internal stack dispatcher changes. Six pre-copy PSRAM references remain
boundary calculations; the audit finds no early PSRAM instruction target.

Real segment copies remain text Flash 0x680000 -> VMA 0x48000000 and
rodata Flash 0x30020 -> VMA 0x48390020. Expected FNV text e93be4dd,
rodata 9cd19a98. BSS ends at 0x4ffad134, below 0x4ffaefc0. Application
starts at 0x2000 and remains below data 0xc00000; full 4 MiB data layout
is unchanged. Final source hashes and boundaries pass independent checks.

Eight overlay file changes are confined to the PPA ABI extension, driver
error handling, private RGB565 adapter, JPEG adapter and its caller/build
entry. Resolved configuration, XIP/NAT SDK sources, JPEG hardware metadata,
C6, SDIO and BLE capacity match C12. Rejected IRQ/autosuspend/reload
experiments remain disabled. No security provisioning is enabled.

Host XIP verifier/SDK mutex, PPA/JPEG application and driver-sequence tests
pass; device transport/registers are substituted in the PPA host tests.
This image is not deployed. No physical pixel, performance, display or
prior-candidate hardware acceptance is inferred from these results.
