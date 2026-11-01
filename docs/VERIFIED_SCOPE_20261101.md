# Verified source snapshot — 2026-11-01

This branch records the frozen source of ESP32-P4 candidate 56, based on commit
`5c4310c3`. It does not include the later candidate 57 upsample-pair experiment.
The candidate 56 application image SHA-256 is
`6b688b19bfa5075ba9350bd9ee9eaf1888f8ee5841845cddec58a03690f1058b`.

## Recorded acceptance

- Firmware build, ELF/XIP review, application-only flash, independent readback,
  data-partition preservation and warm runtime checks passed.
- Host waveform and bounded memory checks passed. Board TTS comparisons used
  generated samples without speaker playback. The user confirmed screen and
  backlight stability after boot and SD recovery.
- On 2026-11-01 the user accepted the status bar UI and the new 59-class Mijia
  icon UI. The standalone `status-island` candidate image was not flashed; its
  UI acceptance is recorded separately from deployment status.

## Remaining limits

The intermittent SD read `-EIO` is unresolved. Speaker listening and actual
first sound, live microphone-to-ASR-to-answer-to-TTS, human wake and false
trigger trials, cold-power boot, physical SD removal and new BLE identity
pairing/encryption/HID/reboot remain unaccepted. INT8, reduce, FMA and LUT
experiments remain disabled. Candidate 57 is excluded from this snapshot.

Evidence is in `docs/evidence/verified-candidate56/`. Related AI Coding
conversations are under `logs/flash555588/2026-10-28/` and
`logs/flash555588/2026-11-01/`, with entries in the contest log manifest.
