# Candidate 10 final ELF review

Image 1b963fd4c0e6db73ce7600576b8eef3ee049bffd84596e559567e2578605274c.
ELF 32c44dfa4695d3a115d11f64cbd23905a22f18307fa971adc43040ea3797e68a.
BLE C0:40:64:8A:D7:55 / OpenVela Mouse-648AD755; old bonds preserved.
BSS end 0x4ffacfe0 remains below 0x4ffaefc0. The application begins at 0x2000;
all of 0xc00000..0x1000000 remains data and is excluded from flashing.

Reviewed all of XIP-C9-DIFF.txt after re-running the independent final ELF
audit. The only focused changes from the previously reviewed C9 closure are
three lui/addi pairs forming the new text size/end: 0x382200 -> 0x382900,
0x48382200 -> 0x48382900. No branch, call, callback, copy ordering, or data
context change. Four jalr and one tail jr still use the same internal tuning
table and slots. All eight calibration callbacks retain their C8/C9 internal
addresses and direct-call closure. The final ELF still has exactly six
pre-copy PSRAM boundary address references and no PSRAM instruction target.
Both CPUs retain PMP initialization; SPI1 retains the cache-on mutex and
internal stack dispatcher. Rejected cache-off IRQ/autosuspend/reload are off.

Final segments: text Flash 0x680000 -> VMA 0x48000000, 0x382900 bytes;
rodata Flash 0x30020 -> VMA 0x48390020, 0x646a8c bytes. Real image offsets,
copy boundaries, reserved heap start, BSS and application/data limits pass.
Expected independent FNV: text 9ecd4e8d, rodata 83e16f06.

Only production change from C9 is the fresh-UID adapter in c6_wifi_phy_rpc.inc.
It obtains each UID while holding the existing transaction lock, then enters
the locked owner. Updated host tests execute the actual locked owner and
production mailbox. The old zero-UID path is rejected before send and fails
the positive assertion as required. Wire faults, storage-selector failures,
PHY rollback, generations and full STA/AP network regressions pass on the
frozen C10 source. C9 failure remains recorded; it is not physical acceptance.

Resolved config, XIP sources, codec metadata, C6 image, SDIO clock, deferred
TX scheduling and 2048-entry NAT match C8. Stable C8 is the explicit rollback.
Phone/PHY negotiation, throughput, physical Flash/display and cold boot
remain pending for this exact image until independently observed.
