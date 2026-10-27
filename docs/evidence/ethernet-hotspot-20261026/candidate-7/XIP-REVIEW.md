# Candidate 7 final ELF review

Image 44fac9c8e0a9ebe827101a7dd51931e6a5e7f2346bd3c1b947135200afd73ab7.
ELF df11264c4bca0e767ad9219f2913ae9bf214d03f42d99b781e0d4678ec313cf5.
BSS end 0x4ffac3e0. Full application/data boundaries passed both collectors.
BLE D5:D7:66:6F:C0:42 / OpenVela Mouse-666FC042; prior bonds preserved.

Manually reviewed complete XIP-PREVIOUS-DIFF.txt: exactly three text size/end
immediates differ from C6; no control-flow/data-context changes. Four jalr,
one tail jr, eight calibration callbacks, two-core PMP and SPI1 cache-on
mutex closure verified. Rejected IRQ/cache-off/autosuspend/reload remain off.
Expected runtime FNV hashes: text 9df4bef3, rodata 8456c6d0.

Exact production delta from C6: c6net.c and c6_softap_net.inc only.
TX notifications atomically mark pending; the existing permanent daemon
drains STA/AP outside caller socket locks. RX draining is capped at 16 polls
per turn so pending TX is not starved. No new thread or buffer allocation.
Configuration and SDK preparation identical; bounded 512-entry PSRAM NAT retained.

Two live C6 snapshots show stable PID20 -> PID48 -> PID20 socket/list ABBA,
with both CPUs running and NAT lock free. Old real TX callback reproduces
the cycle in controlled pthread test; fixed callback passes, including
coalescing/requeue/down cases. Full SoftAP, wire dispatch, STA trial and
RV32 target compilation passed. No real phone or display pass inherited.
Physical transient blue screen remains unresolved, outside this fix claim.
