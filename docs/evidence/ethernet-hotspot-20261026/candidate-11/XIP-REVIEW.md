# Candidate 11 final ELF review

Image 11aaf366e2ba2d758ef432a01141be69ea16b4ce3eff1f245608c16096228863.
ELF 89411b7152051139a8ed5e6f74f3bd4c3552ea0013bc904bf247a73b76d43f9a.
BLE C1:84:C4:9B:CE:CE / OpenVela Mouse-C49BCECE; old bonds preserved.

Reviewed the complete XIP-PREVIOUS-DIFF.txt against reviewed C10. Instruction
changes only form updated text size/end 0x383600/0x48383600 and rodata
size/end 0x646cb8/0x489d6cd8. The other diff is an objdump symbol annotation;
its store instruction is unchanged. No branch, call, table slot, context or
copy order changed. Four indirect jalr and one tail jr in tuning remain
identical, with all eight internal calibration callbacks at the same
addresses and the unchanged direct closure. Six pre-copy PSRAM references
are boundary calculations, not instruction targets or payload reads.

Primary and secondary CPU PMP initialization, SPI1 cache-on mutex and the
internal stack dispatcher are unchanged. The final ELF audit checks real
image offsets, boot copies and BSS/heap reservations. BSS 0x4ffad0d4 is below
0x4ffaefc0. Text is at Flash 0x680000 -> VMA 0x48000000; rodata at Flash
0x30020 -> VMA 0x48390020. Independent expected FNV: text 224b4fdb,
rodata e136f9d1. The app starts at 0x2000 and ends before data 0xc00000;
all four MiB of data remain excluded from the app write and are compared.

Production overlays, native NAT/XIP inputs, hardware JPEG, C6 image, SDIO
clock, deferred TX scheduling, IOB count and NAT limit match C10. Exactly
two resolved config changes: static forwarding structures 4 -> 16 and
network statistics enabled. Dynamic forwarding allocation stays zero.
The frozen builder helper's three host tests pass, including its IOB guard.
Previous host evidence applies only to unchanged code. Rejected cache-off
IRQ, Flash autosuspend and DSI reload remain disabled. No provisioning or
encryption options are enabled. C10 is the development rollback and C8
is separately retained; C10's recorded checksum failure is not cleared.

Warm boot, physical display/Flash, download integrity and 5 Mbps acceptance
are pending for this exact image. No C10 hardware acceptance is inherited.
