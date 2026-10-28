# Independent browser App 0.5.0

The QPK directory contains the NetSurf engine in `native/browser.elf`, with a
separate namespaced webpage VM. The matching Vela firmware contains the native
loader and shared LVGL/QPK/TLS services. An unmodified Vela image cannot load it.

## Current P4 candidate

- App: `dist/live-native-app/NetSurfBrowser-0.5.0.zip` (about 1.57 MB).
- Native module: about 3.32 MB on SD; static code/constants/data/BSS about 2.57 MB.
- Loader firmware: `dist/live-native-firmware/Vela-NativeLoader-P4-UNTESTED.bin`,
  10,695,040 bytes. The filename deliberately retains the broader acceptance gate.
- Hardware baseline: `/home/flash/vela-p4/camera-ui-20261021`, proven identical to
  the original board image before changes. Candidate builds in the isolated
  `/home/flash/vela-p4/integration-netsurf-live-20261021` directory.
- P4 revision v3.2, 16 MiB Flash, 32 MiB PSRAM. The baseline already uses PSRAM
  XIP; the builder never implicitly enables it. A dedicated 3 MiB executable
  pool is reserved at `0x48a10000..0x48d10000`; ordinary heap remains nonexecutable.
- Original app/data boundary remains `0xc00000`; original backup and exact
  app-only rollback are retained locally. Never include private Flash backups
  in App distributions or source exports.

## Click-freeze fixes

ELF loading runs in a detached 64 KiB worker below the UI priority. The worker
never calls QuickJS or LVGL. A UI timer publishes the API when loading completes.
Closing deletes the timer and releases every JS/UI reference immediately;
reopening shares an in-flight load and attaches only to the current page.
Validated code stays resident until reboot because owner/HTTP threads outlive a
page. Reboot after replacing an already-loaded module.

NuttX's RISC-V relocator asserts on valid zero-distance branches and adds some
RELA immediate fields instead of replacing them. Packaging uses GNU ld with
relaxation disabled to resolve invariant local branches/calls, verifies the
result at two distinct bases, then removes their runtime relocations. External
calls and absolute pointers remain relocatable. Store/CALL immediate fields are
normalized so explicit RELA addends are applied once. The production module has
108,883 local control-flow relocations pre-resolved and 58,125 hints removed.
No NuttX assertion or memory protection is disabled to make this work.

## Build and verify

Use `tools/integrate_openvela_native.py` with the explicit camera-ui baseline,
`--text-pool`, the prepared core archive, and recorded baseline link command.
Use `--resume --incremental` only for this tool's existing candidate. The exact
command is retained in the candidate's `build-command.json`.

Run `test_openvela_module.py`, `test_openvela_local_relocs.py`,
`test_openvela_exec_pool.py`, `test_openvela_transport.py`, and the Quick App
smoke test. `verify_openvela_native.py` audits all 218 imports, partition/BSS
boundaries and the reserved executable pool. `package_quickapp.py --native-module`
packages the engine; `--interface-only` does not produce a standalone browser.

## Install

Install the complete `com.netsurf.browser` directory under `/sdcard/qpk/`.
Keep `app.js`, `manifest.json`, `native/browser.elf`, and `native/host.abi` together.
Download updates to temporary files, verify their size/digest, then rename.
Prefer SD; do not overwrite user apps or move existing internal Flash data.
Shared font: `/sdcard/fonts/VelaGlobal-Regular.ttf`.
Verified HTTPS also requires a supported PEM trust bundle at
`/sdcard/netsurf/ca-bundle.pem`, networking, and correct time.

The loader checks the exact installed origin, host ABI, native descriptor and
cache synchronization before invoking module code. This is compatibility
checking, not a sandbox for arbitrary native code.

## Hardware evidence and limits

See `VERIFICATION-20261021.md` for precise accepted checks. Cold boot passes the
existing XIP verifier and both-core PMP pool checks. Background loading remains
responsive; missing modules show an error; close/reopen works. The native module
executes, and a real HTTP page renders Chinese text, CSS and PNG. Webpage
JavaScript fetch updates the DOM. A real touch link navigates to the next page.

This is basic webpage browsing, not complete modern-browser compatibility.
Public HTTPS and inline button handlers still require acceptance. Compressed
HTTP responses, modern image formats, form text input, downloads and modern
web APIs remain incomplete. Static screen readback does not prove physical
flicker-free operation. Build metadata retains `deployable=false` until the
broader acceptance gates pass.
