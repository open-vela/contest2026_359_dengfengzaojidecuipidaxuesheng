# Current board runtime and desktop integration

The final version represented by this branch is candidate109, deployed image SHA256 `09699b5ba452150e913f5062bf8fd9b344410833c85ecc634e7ddcc7c0bb9085`. The first integration commit carries candidate108's runtime prerequisites and UI; the following performance commit advances ASR to109. Current defaults retain the independent original-chat and control9000 models, Medium thinking, Low/Medium/High maximum4/5/6 model rounds and early completion.

The runtime includes full raw Q4/Q8 ASR residency, capture/compute ownership and release before AI/TTS, PIE and SMP operators, staged console/basic-UI startup, background resource verification and reclaimable material caches. It preserves the newer music artwork cache, independent favorite downloads/offline audio, seek-prefix replay and existing stored tracks. The locked four wallpaper inputs, refined lock pages and the 42-frame small-bird renderer/motion data are included. Wallpaper decoding and font/material ownership changes follow the current compressed SD implementation rather than restoring the earlier full-screen retained buffer.

The board109 image was not rebuilt or reflashed by this source publication. `docs/evidence/board109-20261104/SOURCE-MANIFEST.json` maps the recorded build inputs to published contents. Git text files are normalized to LF and the desktop Makefile's redundant final blank lines are trimmed; original build-byte SHA256 values remain recorded separately. Binary inputs and resource payloads keep exact hashes. This distinction avoids treating whitespace normalization as a different implementation or mixing hash policies silently.

## SD resources and models

Copy the **contents** of `materials/board109-ui/desktop-assets/v2/` to `/sdcard/desktop-assets/v2/`, preserving subdirectories. The 74 files are exactly those named by the runtime catalogue; the additional pinyin fallback record is built in. `materials/board109-ui/MANIFEST.json` binds every file to its SHA256. The artwork and dictionary pack is data only, not executable code or user configuration. Keep existing user photos, recordings, playlists and model directories.

ASR uses the user-selected old mixed Q4/Q8 Zipformer14M package at `/sdcard/local-voice/models/zipformer14m-q48-ram-20261104`. The three raw weight files total11,609,444 bytes. `SELECTED-ASR-MODEL.json` in the evidence directory lists the complete package hashes. Model weights are not duplicated in this code PR. Existing local voice releases, original chat weights, control9000 and TTS model packages remain prerequisites; their root defaults and installation layout are in `app/espdl-quickapp/LOCAL-VOICE.md` and the runtime headers. The generic builder's ABI-capability labels do not override this selected deployment package.

`tools/pack_seagull.py` regenerates the packed RGB565/A8 planes from `app/espdl-quickapp/assets/seagull/*.rgba` into an explicit output file. Its decoded-plane round trip and comparison against the published C arrays pass for all42 frames. The four wallpaper JPEGs and provenance remain in `app/espdl-quickapp/assets/lockscreen/`; they are now consumed through the versioned material package, not a regenerated built-in wallpaper header. Pinned Material Symbols SVGs, their license and the missing authored-shape helper are provided for the existing icon generation tools; compiled icon assets remain the image-bound source of truth.

## Build and checks

Use `tools/build_board109.py --help` for the accepted feature profile. Supply a compatible prepared P4 openvela baseline, toolchain, speech/ESP-TTS/wake SDK inputs and a separate build workspace/output directory. The wrapper delegates to `tools/build_bilibili_v3.py`, preserves BSS/partition/XIP guards and never flashes hardware. Prepared dependency acquisition is documented in `docs/P4_BUILD_REPRO_2026-10-27.md`. Each new build generates a distinct BLE test identity, so a rebuild is not claimed to reproduce the historical image byte-for-byte.

```sh
python3 tools/verify_board109_submission.py
python3 tools/pack_seagull.py --output /tmp/seagull-data.c \
  --check app/espdl-quickapp/overlay/apps/system/desktop/glass_seagull_data.c
python3 tools/test_material_cache.py --output /tmp/material-cache
python3 tools/test_asr_resident_ops.py --output /tmp/asr-resident
python3 tools/test_music_background.py --apps /path/to/prepared/apps
python3 tools/test_music_artwork_cache.py
python3 tools/test_lock_photo.py
python3 tools/test_local_wake_defaults.py --output /tmp/wake-defaults
```

Host checks require GCC, FFmpeg and the dependencies required by each script. They use isolated temporary inputs and do not actuate household devices. Additional native LVGL harnesses for lock pages, small bird, music artwork and local voice are supplied under `app/espdl-quickapp/tests/`; the matching historical results and current board regression receipts are in the evidence directory.

## Acceptance boundary

For the same5.153-second file sample,108 used107.12seconds of ASR computation and137.53seconds total;109 uses86.01seconds and116.72seconds. Two public sample transcriptions remain identical to the existing Q4/Q8 reference. Complete weight residency, zero inference weight reads, cancellation cleanup, dual-AI switching and default Medium are verified. Source publication separately reruns music/cover/lock-photo/wake and material/bird checks.

ASR remains slower than real time. These file samples do not establish broad recognition accuracy or human microphone quality. Reset boot, startup-order and lock-page commands are verified; physical display flicker, cold power cycling and long-term operation are not accepted by this109 test. Historical UI/artwork acceptance is scoped to its own older candidate. Candidate107's console timeout and verified rollback are retained as failed evidence with root cause unconfirmed. No private configuration backups, credentials, raw network RPC payloads or unrelated chat histories are included in the public test logs.
