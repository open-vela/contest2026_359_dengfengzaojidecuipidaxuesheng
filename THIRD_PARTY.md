# Third-party provenance

This file records the third-party source material used by the ESP32-P4
overlay. It does not replace the license files shipped by the corresponding
upstream projects.

## ProgrammableMatrix / JBOX-S3

- Source: https://github.com/streetartist/ProgrammableMatrix, branch `main`, commit `2ca78dcb9c9b825b943da3b36e7baf19f558d91d`.
- Imported on 2026-11-04 into `app/ProgrammableMatrix`, with upstream tracked files unchanged.
- Scope: ESP32-S3 ESP-IDF firmware, Python client/GUI, JBoxScript, documentation and tests. This is an independently built companion-board project.
- Per-file SHA-256 values and Git object IDs: `app/ProgrammableMatrix/UPSTREAM.json`.
- The pinned upstream tree has no standalone LICENSE or NOTICE file; no additional license is assigned by this import. Existing source notices are retained.

## ESP-DSP ASR vector subset (candidate101)

- Upstream: https://github.com/espressif/esp-dsp at
  `3c8ac0fdfec83740b783e200862c8d0c056de0ad`, Apache-2.0.
- Original C float add/sub/mul kernels and their headers are retained without
  byte changes in `app/espdl-quickapp/experimental/esp_dsp/`; `UPSTREAM.json`
  records SHA256 and Git blob IDs, and the upstream `LICENSE` is included.
- Private NuttX compatibility headers and the bounded broadcast adapter are
  repository-native changes. Compiler symbol prefixes prevent collisions
  with existing speech libraries. No model requantization, HWLP backend or
  ESP-IDF/FreeRTOS replacement is introduced.

## Google Material Symbols Rounded

- Source: https://github.com/google/material-design-icons, revision `10ecc14790612ca8fd164e3f4ffe9c16fc50d5b7`.
- 121 symbols, outlined and filled variants (242 SVGs); Apache License 2.0.
- Original SVGs, license and per-file SHA-256 records: `materials/material-symbols/`.
- Adaptations: normalized 24-unit geometry, compound-path component grouping,
  two-color active states, finite state-triggered LVGL animations, and six raster sizes.
- Microsoft's AnimatedIcon documentation supplies state-design guidance; no Microsoft
  animation asset, WinUI code or Lottie runtime is included.

## CherryUSB board interface classes

- Source: https://github.com/cherry-embedded/CherryUSB, tag `v1.6.1`, commit `c9625ffa773ad10b8824d1b5361bca2ccc1f3d1e`.
- License: Apache-2.0, copyright notices retained in source.
- Imported MSC/SCSI, HID, CDC ACM, device core and DWC2 device controller into the existing `camera_usb/cherryusb` vendor tree.
- Original source hashes and archive digest are in `camera_usb/cherryusb/INTERFACES-UPSTREAM.json`.
- Local integration validates MSC short transfers and CSW identity/residue, bounds HID descriptor parsing, routes HID controls to the correct interface, and integrates NuttX heap-backed host pools and the class registry. Board UAC1/CDC/VFS adapters are maintained by this project.

## JIZURA complete reference application

- Source: https://github.com/852wa/JIZURA
- Version: 0.10.1, commit `fc16bfe43ea4a6c25a21f1caf04d17326de14f00`.
- Copyright: hakoniwa; license: MIT.
- Imported unchanged into `app/jizura/upstream`; per-file SHA-256 values are in `app/jizura/upstream.lock.json`.
- Preserve its `LICENSE` and `THIRD_PARTY_NOTICES.md`, including the bundled mp4-muxer notice. Runtime fonts retain their own font licenses.
- Purpose: complete browser reference before ESP migration; this import does not indicate that its engine or editor runs on ESP hardware.

## Apache NuttX and openvela apps

- Components: NuttX kernel, drivers, board framework, and NuttX apps.
- Release bases: NuttX `2f1387d56eb04ad2599baca58a3fa2380cdaaedb` and
  apps `88827afd368d4bbb4802b96ed44d9582f85b2f92`.
- License: Apache License 2.0; retain upstream copyright, NOTICE, and SPDX
  declarations.
- Use in this repository: `tools/patches/0001` through `0015` are applied to
  those bases by `tools/apply_final_overlays.sh`.
- Limitation: these release bases are imported snapshots. Their exact public
  Apache NuttX/openvela ancestry still requires an upstream commit mapping;
  see `UPSTREAM_PLAN.md`.

## Espressif ESP HAL

- Source: `https://github.com/espressif/esp-hal-3rdparty`.
- Commit: `78c092909fca38d1e2ccf767b5eff66bddc5c789`.
- License: Apache License 2.0.
- Local changes: the two reviewable patches installed by
  `0013-esp32p4-hal-reproducibility.patch`; no Python in-place rewrite is part
  of the release procedure.

## Espressif SC2336 register sequence

- Source repository: `https://github.com/espressif/esp-video-components`.
- Source commit: `3620887638419f7afbe4aa3a909422c640b14061`.
- Source path:
  `esp_cam_sensor/sensors/sc2336/private_include/sc2336_mipi_2lane_24Minput_1024x600_raw8_30fps.h`.
- Upstream copyright: Espressif Systems (Shanghai) CO LTD.
- License: Apache License 2.0.
- Transformation: the NuttX port keeps the fixed 1024x600, RAW8, two-lane,
  30 fps initialization sequence and wraps it in the NuttX image-sensor API.
  The source commit and path are also recorded beside the generated table by
  patch `0015-third-party-provenance-comments.patch`.
- Hardware note: Espressif documents that sensor initialization tables may
  originate from the sensor vendor/FAE. Redistribution remains under the
  copyright and license declarations carried by Espressif's source.

## Espressif EK79007 panel sequence

- Source repository: `https://github.com/espressif/esp-iot-solution`.
- Source commit: `c8c4726501e13849b785f2cb9ae55ec1266b44dc`.
- Source path:
  `components/display/lcd/esp_lcd_ek79007/esp_lcd_ek79007.c`.
- Upstream copyright: Espressif Systems (Shanghai) CO LTD.
- License: Apache License 2.0.
- Transformation: the board driver keeps the two-lane vendor command sequence
  and implements reset, sleep-out, display-on, and framebuffer binding through
  NuttX APIs. Patch `0015` records the immutable source beside the table.
- Hardware limitation: the physical panel-controller marking must still be
  confirmed for every supported board batch.

## LVGL

- Source: `https://github.com/lvgl/lvgl`.
- Commit: `59a6b61c9580b65089010c5273f2fcdd6c4d2aae` (v9.2.1 source).
- License: MIT.
- Integration: NuttX apps LVGL port plus local framebuffer and allocation
  fixes. The Kconfig version was corrected to 9.2.1 by patch `0012`.

## QuickJS

- Source: `https://github.com/bellard/quickjs`.
- Commit: `6e2e68fd0896957f92eb6c242a2e048c1ef3cae0`.
- Archive SHA-256:
  `d4542882686eefa8c0c80493dfb30d858588c0553ffe17a058189132607ff24e`.
- License: MIT.
- Integration: NuttX compatibility patches and the contest QPK runtime. Patch
  `0012` fixes the archive name and verifies the archive before extraction.

## Grok Ball replacement for the OuO entry

- Location: `ouo/`.
- Upstream: https://github.com/tycoding/grok-ball, commit
  `de368ce3acdc5a70e871ee46ebbe8f60b11d3d6a`.
- Original engine, 25 eye contours and 32 emotion definitions: MIT, copyright
  (c) 2026 tycoding. The unmodified engine and full license are retained under
  `ouo/vendor/`; source hashes are in `ouo/vendor/UPSTREAM.json`.
- The SVG scene adapter, LVGL contour renderer and native app integration
  are Apache-2.0, as declared by `ouo/LICENSE`. Generated app and embedded C
  resources include the upstream MIT notice and declare both licenses.

## Dafeiyu desktop pet

- Source: `https://github.com/1190fasheqi/dafeiyu-pet`.
- Upstream license: MIT; the original license text is retained at
  `app/espdl-quickapp/overlay/apps/system/desktop/dafeiyu/LICENSE.upstream`.
- Integration: the local dialogue and behavior are adapted for the native
  LVGL overlay and Quick App API; newly written device-side code declares
  Apache-2.0. The pet source has host tests but no new firmware or board
  validation yet.

## Release obligations

Before submission, run a license/SPDX scan over the materialized NuttX and apps
trees, retain all upstream license and NOTICE files, and update this ledger if
any source commit or register table changes. A repository-wide root LICENSE and
NOTICE remain release-level P0 deliverables and are intentionally tracked
separately from this P1 provenance record.

## Local and live media players

- OpenH264 2.6.0: Cisco BSD license, installed from a SHA-256 pinned archive.
  The installer preserves the upstream LICENSE and applies decoder-only RV32,
  bounded scratch storage, allocation guards and worker ownership patches.
- minimp4: CC0, revision and source hash in
  `app/bilibili_live/local_vendor/UPSTREAM.json`; upstream notice stays in header.
- Helix AAC via arduino-libhelix: original RealNetworks license retained as
  `app/bilibili_live/local_vendor/HELIX-UPSTREAM-LICENSE.txt`; revision and local
  adaptations are recorded in the same manifest. No Arduino wrapper is linked.
- TLS remains the existing desktop glass_https / mbedTLS implementation.

## Native music language assets

- Noto Sans SC/JP/KR and Noto Sans: SIL Open Font License 1.1; fixed Google Fonts
  commit and source/notice hashes in `app/espdl-quickapp/assets/music-fonts.lock.json`.
  `tools/prepare_global_font.py` instantiates regular weight, retains all source
  codepoints and emits the renamed Vela Global derivative plus four OFL notices
  for SD deployment. Font binaries are SD assets, not linked firmware arrays.
- OpenCC t2s dictionaries: Apache-2.0, commit
  `556ed22496d650bd0b13b6c163be9814637970ae` (`ver.1.1.9`). Original TSCharacters,
  TSPhrases, license and hashes remain in `app/espdl-quickapp/assets/opencc/`.
  The generated read-only table is used in the lyric worker; it is not the
  OpenCC runtime library.
- The private SD TinyTTF instance includes the existing pinned LVGL TinyTTF/STB
  sources with private entry-point names; their upstream notices stay in those
  dependencies. Global LVGL font configuration is unchanged.

## SD speech and lyric font supplements (2026-10-27)

- `app/espdl-quickapp/assets/sd-pack/speech.zip` contains the three official
  ESP-TTS voices and MultiNet7 Chinese model from ESP-SR v2.4.6, revision
  `cfdbb52172d4f1b3e5f7469d6c3c6e5dd0738143`. The original ESP-SR license is
  retained in the archive; each upstream binary is checked against the existing
  `speech/upstream.lock.json` or `speech/esp-tts/upstream.lock.json`.
- The same archive includes the existing repository's bootstrap command grammar
  and custom wake model, without replacing or retraining the model. Model and
  training provenance remain under `app/espdl-quickapp/models/`.
- `app/espdl-quickapp/assets/sd-pack/lyric-fonts.zip` contains six renamed,
  static Vela Lyric fonts derived from Noto Sans SC, Noto Serif SC, Ma Shan Zheng,
  ZCOOL KuaiLe, ZCOOL QingKe HuangYou and ZCOOL XiaoWei. Source commit
  `47d54ca1ebb14fd864695f1d39dc734f39017c33`, hashes and URLs are pinned in
  `assets/lyric-fonts.lock.json`; generation is implemented in
  `tools/prepare_lyric_fonts.py`. Each font's original SIL OFL 1.1 notice is
  retained alongside the derivative. Runtime sizes and FNV checksums match the
  existing generated `glass_lyric_font_assets.h`.
- `app/espdl-quickapp/assets/sd-pack/browser-ca.zip` contains the unmodified
  public Mozilla CA bundle and original MPL-2.0 notice from certifi tag
  `2026.07.22`, commit `f4bc676bc101fe2235846e37044e8c693d6cbaf4` at
  `https://github.com/certifi/python-certifi`. Both files were fetched from
  that immutable revision and checked by SHA-256. The device's Windows-exported
  trust store and additional local trust certificates are not published.

## Album PNG decoder

- LodePNG: https://github.com/lvandeve/lodepng, commit
  `e584a8490db1b76b81bf298c11c09d3b55d9c270`, zlib license.
- Unmodified upstream source is under `app/bilibili_live/local_vendor/lodepng/`;
  `lodepng.cpp` is stored as `lodepng.c` and compiled as C. Original notices,
  license and per-file SHA-256 hashes are preserved beside the source.
- `album_png.c` enables decoding only, supplies bounded allocation and converts
  RGBA to RGB565 in the album worker. HEIC/AVIF recognition does not include
  those codecs or imply playback support.


## Local voice adapters

- The user-supplied EdgeIdiot SFT2, V2 acoustic/vocoder checkpoint, and Edge
  Impulse deployment retain their own provenance and original notices. The
  runtime exporters do not assign a new license to these trained weights.
- Edge Impulse MFCC/EON and its bundled TensorFlow Lite Micro/FFT sources are
  compiled from the supplied SDK. The prepared `wake-sdk/original` tree stays
  unchanged; externalization is confined to a generated copy and a private
  NuttX float-overload compatibility header. Upstream file notices are retained.
- The SD Chinese pronunciation tables use pypinyin 0.55.0. Its original MIT
  notice is included as `notices/pypinyin-LICENSE.txt` in the resource package.
- The custom Llama, tokenizer, and V2 C kernels are repository-native code;
  numerical references use the original supplied Python models. The Zipformer
  evaluation includes a repository-native experimental C graph/fbank/greedy
  runtime under `app/espdl-quickapp/experimental`. It is not linked into the
  accepted candidate 7 firmware and is not claimed as board ASR acceptance.
- The fixed Chinese Zipformer 14M source archive SHA-256 is
  `2cbd71b640d9c37d3784f29367333a4577b0398b62e9deeed418170b081cba8b`.
  Its original model README declares Apache-2.0 and points to the upstream
  csukuangfj Hugging Face model. Experimental mixed-precision assets retain
  that provenance, original model README and Apache license text.
- Independent C fbank and greedy-search implementations follow the documented
  behavior of kaldi-native-fbank and sherpa-onnx (Apache-2.0, Xiaomi Corporation
  and Kaldi authors). No upstream ASR/fbank implementation source is vendored;
  the separately listed DSP vector kernels are the exception for generic math.
  Host comparison uses kaldi-native-fbank 1.22.5 and sherpa-onnx 2.0.1;
  their native runtime libraries are host-only reference dependencies, not
  firmware components. Reference links are in experimental/ASR-SOURCES.txt.
