# Native JPEG candidate 5 final-link review

Image 1165ffbe30d98dc713b01cbbdbdaa75adff9cd6c59f6577df9a56b028a9d8902.
ELF d63ed6cc71f421752ce93085229b2ca31802246dccb9b170d93915b19e6bdc2a.
Normal direct pre-copy closure contains no PSRAM instruction. The collector
continues to exclude pre-existing fatal abort/assert handlers; their safety is
not newly established by this work.

The final collectors passed: nine prepared XIP inputs each match their own
metadata; seven runtime/linker inputs and the preparation tool match the prior
accepted/network artifacts; Kconfig/Make.defs contain only the separately
reviewed JPEG/PPA integration changes. All 20 HAL tuning sources match. The
only resolved config change from network candidate 4 is ESP32P4_JPEG=y.

Four jalr a5 plus the tail jr a5 retain their accepted addresses and bytes.
All eight callback entry addresses remain internal and unchanged. The table
moves from JPEG4 0x4ff6acd4 to 0x4ff6acc4; phase/delay state remain internal.
Manual JPEG4-to-JPEG5 focused review finds text end/size +0x100, rodata end/size
+0xaf30, internal MMU/PSRAM/table data relocations -0x10, and a relocated
post-copy time target. No new branch or startup ordering change is present.
Six pre-copy PSRAM references only materialize lengths/boundaries. Both CPU
protection calls and the SPI1 cache-on mutex remain linked.

Text range 0x48000000..0x4837de00; rodata 0x48380020..0x489c62fc.
Application remains below 0xc00000; BSS 0x4ffabe5c remains below 0x4ffaefc0.
Nineteen explicitly listed production changes/additions match frozen sources.
Actual final c6probe sources remain identical to network4, with the known
intermediate SoftAP RPC metadata hash normalized to the separately checked
final credential-trial hash. C6 firmware and eFuses are not written.

Fresh BLE identity E4:E4:88:44:5E:9E / OpenVela Mouse-88445E9E. No prior pairing,
cold-start or physical display acceptance is transferred. Phone acceptance was
explicitly skipped by the user. Current hardware JPEG pixels, current Flash/CSI
display acceptance and this identity's pairing must still be recorded separately.
