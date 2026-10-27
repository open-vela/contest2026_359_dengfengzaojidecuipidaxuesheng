# Candidate 8 final ELF review

Image 232409de5177f07cbdcd08dd40c377452892be3820c59b6df9b89bb58cbcc179.
ELF 1624c78c727fad7379715f0f3924308d69b9d270bed5a9fbd099965e464cf7ef.
BSS end 0x4ffacfe8 is below 0x4ffaefc0. Application/data boundaries passed.
BLE DC:1B:9A:E5:44:F5 / OpenVela Mouse-9AE544F5; old bonds preserved.

Manually reviewed the entire XIP-PREVIOUS-DIFF.txt against C7. Three rodata
size/end immediates change by 16 bytes, ten internal MMU/PSRAM/tuning context
references relocate by 16 bytes, and one symbol-only comment changes. No
control flow changes. Four jalr and one tail jr, all eight calibration
callbacks, two-core PMP and SPI1 cache-on mutex closure remain verified.
Rejected cache-off IRQ/autosuspend/DSI reload experiments remain disabled.
Expected independent image FNV values: text ebde48fa, rodata c4fc995e.

Exact production overlay change from C7 is the capacity header: 512 to 2048.
The only resolved configuration change is NAT hash bits 6 to 8. Native SDK
prepared source hashes, radio sources, XIP input sources and codec metadata
are identical. Hash tables add 3072 bytes of static storage (total BSS end
changes by 3080 with alignment). Mappings remain bounded PSRAM user-heap
allocations, 2048 x 36 bytes plus allocator overhead; no active eviction or
timeout shortening. No C6 firmware, partition, eFuse or encryption change.

Actual prepared allocation/free functions use the production capacity header
in the 2048-entry host test: full rejection, slot reuse, allocation failure,
complete release and enable/disable balance pass. The old 512 gate fails
the same test as expected. Other unchanged-component host results explicitly
inherit C7 results; no phone/display/cold-boot acceptance is inherited.

C7 phone browsing was user-confirmed stable with normal screen, and close/
reopen after real traffic succeeded. C7 nevertheless reached 512 with 28 cap
rejections. C8 is a capacity correction and requires its own runtime checks.
Prior transient blue screen cause remains unexplained; C7's observation was
limited to ordinary browsing with zero Flash writes/erases.
