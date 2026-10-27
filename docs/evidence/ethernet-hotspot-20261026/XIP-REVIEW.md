# Diagnostic candidate 5 final review

Both final ELF collectors pass. All production source hashes, SDK NAT/DHCP preparation and resolved configuration exactly match candidate 4; this build changes only the generated test identity and build metadata. Host tests are explicitly inherited; no runtime acceptance is inherited.

Manually reviewed XIP-IDENTITY-DIFF.txt: six instructions change only image length/segment-end immediates, and one line changes only a symbol comment. No early control-flow changes. The four jalr and tail jr targets, all eight calibration callbacks and both-core PMP/cache-on SPI1 mutex closure remain checked.

Image: 574bcd6aad8affb803a3a6440a1c1a34b0dab627f6cca0dd86de92d2f8793a82
ELF: b3e1739f1da9c874f271d363263d37adeff2db0c63d498e740d91ab34bf626ff
BSS: 0x4ffac100
BLE: E3:F6:CF:80:70:3F / OpenVela Mouse-CF80703F

Purpose: reproduce the existing networking failure with CPU-running system-bus metadata reads. This is not a fix or acceptance claim. App-only flash with full data preservation; fallback bf542ed8 retained. Physical display, phone Internet and fresh BLE pairing remain unverified.
