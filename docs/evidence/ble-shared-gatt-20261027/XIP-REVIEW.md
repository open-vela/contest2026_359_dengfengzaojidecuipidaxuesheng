# Candidate 18 final linked XIP review

Reviewed XIP-FOCUSED-DISASSEMBLY.txt, XIP-PREVIOUS-DIFF.txt and the independent audit against installed candidate 16. Image 7d9715148a1ddcac9a534b5feea1e4ae8d70592e4e5573c44589340daff4ed48; ELF 431188fe17171aee9e680d0523dfd3a1ce3f9f81e5ba2a9ea46a2c923dcd9cd6.

The focused diff changes internal data references by +0x40: ROM logging strings, GP-relative constants, XIP flash addresses, secondary readiness, MMU/PSRAM contexts, IROM/DROM metadata, tuning-driver table, best phase/delay state and SPI1 mutex. The addi -64 becoming mv at 4ff4f5e2 is the same string moving across 4ff68000. The unchanged store at 4ff4ed88 only has a disassembler nearest-symbol comment difference. Other differences are final text/rodata segment sizes and ends (48386380 / 489d7798). No added branch, different callback target, memory width or lock sequencing was found in the reviewed closure.

All eight tuning callback addresses remain identical to candidate 16, internal and pre-copy safe. The four indirect jalr and tail jr retain their dispatch sources and internal calibration targets. The six early PSRAM data references are linker boundary/copy metadata, not new pre-copy PSRAM payload loads. SPI1 cache-on mutex enter/release still balances the same protection path. SDK XIP input sources and configuration are unchanged. Rejected cache-off IRQ, auto-suspend and autonomous reload remain disabled.

BSS 4ffad25c is below 4ffaefc0. Application stays below c00000; data c00000..1000000 is preserved. XIP hashes expected after boot: bbb6f07b / 0cde17e3. This is linked-code review, not physical display or cold-power acceptance. No physical acceptance is inherited.
