# ESP32-P4 browser verification, 2026-10-21

The click-freeze fix is installed on COM4, P4 revision v3.2, Flash16 MiB/PSRAM32 MiB.
The original full Flash and exact app rollback remain local; no user data partition
was erased or migrated. This is basic browser acceptance, not full modern-site acceptance.

## Fixed causes

1. Synchronous insmod blocked the LVGL owner. Loading now runs in a separate,
   lower-priority 64 KiB worker; only the UI timer attaches QuickJS/LVGL objects.
2. The RV32 relocator asserts on legal zero-distance branch relocations. GNU ld
   pre-resolves invariant local control flow and verifies it at two load bases.
3. RELA store immediates were added to already encoded addends, producing an
   illegal instruction at page_info_init. Package-time normalization preserves
   explicit addends and clears only the affected instruction immediate fields.
4. One unsupported CA caused the entire 35-root PEM file to be rejected.
   Individual parsing retains 34 supported trusted anchors; strict TLS peer and
   hostname verification remain enabled.
5. Refresh used an unavailable glyph; the App now displays 刷新/停止.

## Evidence

- `build/board-async-open-close.log`: first click returns in 0.21 s;
  UI operations and back remain responsive while the worker loads.
- `build/board-async-reopen.log`: 64 KiB worker below UI priority;
  reopening shares the same load. The older module's later fault is also recorded.
- `build/board-normalized-load.log`: actual HTML/CSS/PNG and JavaScript fetch.
  `build/board-first-page.png` is a complete lossless board framebuffer readback
  from that earlier 0.5 revision, before the CA filtering update.
- `build/board-touch-link-current.log`: touch navigates to P4 next page, Done (0.7s).
- `build/flash-trust-filter.log`: final app-only write and device hash verification.
- `build/boot-final-trust-filter.log`: existing XIP verification and executable-pool
  PMP checks pass on both CPUs; ordinary/uncached heap stays nonexecutable.
- `build/board-final-https.log`: final module accepts34/skips1 root,
  loads https://example.com/, title Example Domain, Done (17.8s).
- `build/board-fresh-html-event.log` and `build/board-final-onclick.log`:
  final module loads a new HTTP HTML document and executes its real onclick,
  changing title to P4 touch passed. Default native button handling still warns.
- `build/board-release-final-state.log`: temporary instrumented entry is removed;
  the normal address UI and Chinese refresh button are restored.
- `build/module-async-test.log`, `build/exec-pool-test.log`,
  `build/transport-tls-test.log`, `build/trust-filter-test.log`:
  loader lifetime, PMP, HTTP/TLS rejection and ASan/UBSan trust-filter tests pass.
- `build/patch-reuse-audit.json`: 52 pre-existing patches touch108 portable files;
  106 are byte-identical to the prepared ESP source. The other2 are newline-only
  and CODE/LIBCSS_CODE macro naming. This is source identity, not a claim that
  every existing patch has separately passed a hardware regression.

The public HTTPS framebuffer export lost62 serial bytes and was freed; it is
not presented as a complete screenshot. HTTPS acceptance uses the continuous
runtime evidence above. No physical no-flicker observation was performed.

## Final artifacts

- `dist/live-native-app/NetSurfBrowser-0.5.0.zip`, about1.57 MB.
- Module3,321,028 bytes; host firmware10,695,040 bytes;219 imports resolved.
- `dist/live-native-firmware/verification.json`: structural/link audit.
- `dist/live-native-firmware/board-verification.json`: granular hardware evidence
  with exact current hashes. The structural builder retains deployable=false;
  broader modern-site, form input, compressed responses, modern images and
  history-restored script compatibility are not fully accepted.

The original52 portable patches are reused. Remaining platform work belongs
in the OpenVela frontend/transport/loader; do not reimplement already patched
NetSurf DOM/CSS/JS behavior without checking the existing source first.
