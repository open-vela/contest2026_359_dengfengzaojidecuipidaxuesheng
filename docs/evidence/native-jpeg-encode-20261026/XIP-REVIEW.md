# JPEG encode candidate 3 final-link review

Image bf542ed81a6b7b2799be54dcaae25609782e733b9fc465c978a3aafb0219cfd4.
ELF f492e37350a6e57b7e58c2f9dd4ce93bf89a4d6ad60fa561128aaf0521e1b9ab.

Both final collectors pass normal startup/copy closure, real image offsets,
PMP boundaries, BSS and source hashes. The four jalr and tail jr, eight
internal callbacks, seven runtime/linker inputs and 20 HAL tuning sources
remain as in accepted/network artifacts. Kconfig/Make.defs contain the
reviewed codec integration and match their own recorded hashes. Fatal
abort/assert handling remains excluded from this proof, as previously.

Manual encode2-to-encode3 focused diff only changes image size/boundary
immediates: text +0x180 and rodata +0x60. No startup/copy/tuning control-flow
change is present. All six early PSRAM references are lengths/boundaries.
Both CPU protection calls and the SPI1 cache-on mutex remain linked.

Text 0x48000000..0x4837f900, rodata 0x48380020..0x489c673c; application
end 0x9ff900 remains below 0xc00000, BSS 0x4ffabe5c below 0x4ffaefc0.
Resolved configuration equals decode5 and encode2. Actual c6probe sources
are unchanged, and NAT/forwarding remain disabled. No C6 firmware, eFuse,
security provisioning or storage-layout change occurs.

New BLE identity D2:08:3F:1A:74:C7 / OpenVela Mouse-3F1A74C7. Pixel checks,
real CSI photo persistence, physical screen/Flash, pairing and cold power
cycling are separate acceptance items; phone testing remains skipped.
