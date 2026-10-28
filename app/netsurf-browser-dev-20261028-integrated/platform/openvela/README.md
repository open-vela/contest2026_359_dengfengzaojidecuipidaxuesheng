# NetSurf independent Vela Quick App

The current 0.4.0 native App distribution, including its separately installed
engine, is documented in [`../openvela_module/README.md`](../openvela_module/README.md).
The embedded-engine candidate described below is retained as the legacy
integration/reference path; its reports do not describe the current native App.

The app at `quickapp/netsurf_browser/` is installed as its own package,
`com.netsurf.browser`. The desktop launches its JavaScript toolbar and exposes
`system.netsurf` only to this package. HTML/CSS and images are rendered locally
by the native NetSurf core.

## Implemented

- Address/search input, history, reload/stop, saved address and touch forwarding.
- One browser window, root-page and nested scrolling, touch links, viewport resize.
- LVGL 9 RGB565 double buffering and bounded 32-row XRGB8888 scratch rendering.
- FreeType text, PNG through libpng, baseline JPEG through the desktop decoder,
  SVG through the existing NetSurf core.
- HTTP(S) NetSurf fetchers: two worker threads, eight 1 KiB mailbox slots per
  request, at most 16 pending request contexts, 4 MiB response and decoded-image
  limits, 64 KiB request-body limit and bounded headers.
- NuttX webclient transport with the existing `glass_https` mbedTLS service.
  Hostname and chain verification remain required. NetSurf handles redirects;
  cookies and status callbacks run on the browser thread.
- QPK install/stop hooks, with a persistent browser owner thread. Closing the
  app detaches its LVGL image before the QPK page is deleted. Reopening reuses
  the core because this NetSurf fork cannot register its global tables twice.
- Optional namespaced QuickJS-ng webpage VM, independently linked from the
  desktop's older QuickJS. The 0.3.0 candidate enables it: inline/external
  scripts, DOM/style changes, button events, Promise/timers and fetch are tested.
  VM memory is limited to 6 MiB, VM stack to 64 KiB and bytecode tasks to 750 ms.
  Navigation/stop/close atomically interrupt running scripts. The target core
  thread runs below UI priority. Blocking native calls are not preempted by
  the VM interrupt callback and still require bounded native implementations.

Modern web compatibility, multipart uploads, progressive JPEG, GIF/WebP, downloads,
font fallback and modern-site compatibility are still incomplete. Requests
ask for identity content encoding and reject compressed responses. The memory
limits above do not impose a quota on the entire DOM/CSS heap.

## Installation and runtime dependencies

Copy the `com.netsurf.browser` folder from the distribution to `/sdcard/qpk/`
or `/data/qpk/`, then refresh the desktop app list. The launcher reads top-level
`entry: app.js` from the manifest. The ZIP is a transport archive; extract it
before copying to the device.

For the legacy embedded-engine package, the firmware must include the native
service. The current native App instead requires the matching module loader.
Installing a JavaScript-only package on unrelated firmware is insufficient.
The defaults are:

- Font: `/sdcard/fonts/VelaGlobal-Regular.ttf`.
- PEM CA bundle: `/sdcard/netsurf/ca-bundle.pem`.
- Network already connected and the device clock set correctly for TLS.

Paths can be changed with `OV_NETSURF_FONT_PATH` and `OV_NETSURF_CA_PATH` when
compiling `openvela_desktop.c`. Missing trust material makes HTTPS fail closed.
The app initially opens `about:blank` and restores its last entered address.

## Verification

JavaScript syntax and mock Vela API checks pass. The real host NetSurf + LVGL
test verifies RGB565 page/image pixels, PNG alpha and palette, JPEG,
oversized/truncated image rejection, backpressure, scrolling, resize, touch
links, redirects, history, cancellation, errors and reopening.
The native host test also uses real QuickJS + LVGL to verify startup, native
API calls and four complete close/reopen cycles on the background core thread.

The production HTTP adapter with the original NuttX webclient passes loopback
HTTP, chunked transfer, redirect interception, compressed-response rejection,
trusted TLS, bad CA and wrong hostname cases. Host TLS uses an OpenSSL shim;
these tests do not prove the device mbedTLS transport.

ESP32-P4 RV32/ILP32F object compilation and full isolated firmware linking pass.
`dist/firmware-js/verification.json` is an older link/layout snapshot; its
JavaScript feature marker predates the metadata fix and is not current release
evidence. Current reports are under `dist/native-loader-firmware/` and
`dist/validation-native/`. No on-board test
has been run for this interactive Quick App. The candidate uses the previous
isolated offline-renderer baseline, not the newest desktop changes in the
user's active Vela tree. It is an untested development image. Its static
internal-memory margin is small; consult the report before further additions.

## Reproduce with the existing local prepared trees

Run in Debian WSL from the NetSurf source root. Dependencies remain read-only.

```sh
python3 tools/check_openvela.py \
  --core /home/flash/vela-p4/netsurf-port-cross \
  --target /home/flash/vela-p4/integration-netsurf-port \
  --cc /home/flash/vela-p4/toolchains/gcc14/bin/riscv32-esp-elf-gcc \
  --output "$PWD/build/openvela-cross"

python3 tools/test_openvela.py \
  --core /home/flash/vela-p4/netsurf-port-host \
  --lvgl /home/flash/vela-p4/integration-netsurf-port/apps/graphics/lvgl/lvgl \
  --quickjs /home/flash/vela-p4/integration-netsurf-port/apps/interpreters/quickjs/quickjs \
  --output "$PWD/build/openvela-host"

python3 tools/test_openvela_transport.py \
  --target /home/flash/vela-p4/integration-netsurf-port \
  --output "$PWD/build/openvela-transport"

node quickapp/netsurf_browser/test_app.cjs
python3 tools/package_quickapp.py --interface-only --output dist/interface-only
```

To enable webpage JS, first run `tools/build_openvela_webjs.py --core <prepared-core>
--output <vm-output>` (plus `--target <nuttx/apps-root> --cc <cross-gcc>` for P4).
Pass `--webjs <vm-output>` to the host test and `--webjs` to frontend compilation.
The builder compresses the original runtime source and namespaces 322 exported
VM symbols; the QPK runtime keeps its existing ABI and native capabilities.
`tools/build_openvela_small_core.py` builds the same prepared core with `-Os`
into an independent output directory. Give that `obj/` path as `--core-objects`
and the VM directory as `--webjs` to `rebuild_openvela.py` when upgrading the
candidate. Static builds need their original none-backend objects; omitting
the flags does not downgrade an archive that already contains webpage JS.

The 0.3.0 candidate fits the unchanged Flash layout through size optimization
and runtime-source compression. It leaves about 116 KiB of Flash space. This
is still an untested hardware candidate, not evidence of arbitrary modern-site
compatibility. The older `dist/firmware/` files describe the 0.2.0 static build.

Host tests use GCC, FFmpeg, OpenSSL and the prepared core. The transport test
never contacts an external site. `openvela_common.py` prepares a private
libpng copy with the linked zlib version recorded in `pnglibconf.h`.

`tools/integrate_openvela.py` creates a new independent firmware tree, patches
QPK lifecycle hooks, packages PSRAM objects and optionally runs a clean build.
It refuses an existing target. `tools/rebuild_openvela.py` refreshes only a
candidate created this way. Neither script flashes a device. The candidate is
at `/home/flash/vela-p4/integration-netsurf-quickapp-20261021`.

## Ownership and concurrency

QuickJS and all LVGL object operations belong to the UI thread. They publish
requests to `openvela_netsurf_service`. NetSurf, its font engine, cookies,
history, decoder and plotters belong to one core thread. HTTP worker callbacks
only enqueue immutable chunks; the core consumes them during fetcher polling.
The view has a separate frame mutex. Viewport changes update core dimensions
and scratch memory first, then the UI image while rendering is paused.

The service persists until desktop termination. Failed core initialization
requires restarting the desktop; registration is not retried. Worker teardown
can wait for the transport timeout and blocking DNS; network workers are never
joined on the UI thread.
# Single-line webpage input candidate

`form-input` adds an owner-thread input request/commit channel. A tap opens the
trusted App's system keyboard for text/password controls, with at most 120
Unicode characters and 511 UTF-8 bytes. Page maxlength is checked again at
commit. Password values are never returned by `takeInput()`; cancel retains
the original value and an empty confirmed string clears it. Readonly/disabled
controls are rejected. Textarea needs a separate multiline input surface.

`system.netsurf.takeInput()` consumes `{id,value,password,maxLength}` once;
`submitInput(id,string)` queues an edit and `submitInput(id,null)` cancels.
Acceptance means queued, not committed: the owner verifies the focused DOM
node, content identity, password mode, original value, current limits and UTF-8.
Navigation/close revokes old requests. DOM/widget synchronization uses the
existing core form implementation. Input/change dispatch keeps only a DOM
reference across webpage handlers, which may replace the box tree.

The existing Vela `prompt.input` silently ignores cancel. The independent
loader prepared by `prepare_openvela_input_loader.py` adds an opt-in
`notifyCancel:true` option and advertises `prompt.cancelNotification===true`.
Only opted-in callers receive `null` on cancel; legacy callers retain their
behavior. The App enables webpage input only with this capability and
serializes address/form prompts. This requires the explicit form-input loader,
even though native module imports and ABI remain compatible with 0.5.0.
Tests/candidate artifacts are under `build/openvela/form-input` and
`dist/candidates/form-input`; these are not release acceptance evidence by
themselves. Do not overwrite the frozen 0.5.0 snapshot.

# Network connectivity candidate

`network-connectivity` reuses the existing glass_https connector, certificate
and hostname verification, abort flag and progress-based timeout support.
The initial TLS connection budget remains 30 seconds. Successful TLS send/read
progress renews a 60-second idle deadline, matching the existing WinHTTP
receive-timeout approach instead of expiring a healthy response after 30
seconds overall. Existing 4 MiB wire/decoded quotas and cancellation remain.

Only GET may retry once after a timeout/reset/refusal explicitly reported by
the TLS connector in its tcp/handshake stage. No HTTP request has been sent at
that point. POST, certificate errors, cancellation, send/receive failures and
partial responses are never replayed. Reinitialize webclient per attempt.

`--network-trace` on check/build tools enables diagnostic host, stage, status,
byte counts and elapsed time. It omits credentials, paths, queries and bodies.
This is a separate candidate from form-input and the frozen 0.5.0 baseline;
the core archive and its existing portable patches are reused unchanged.

Refresh on timeout/fetch-error documents dispatches the core's existing retry
form handler instead of reloading an internal about: URL without its required
data. Certificate/auth query handlers are excluded. A real-core regression
first times out, then presses the App reload operation and verifies a second
request to the original URL completes.

The owner also caches `status.errorPage` for generated timeout/fetch/certificate/
auth documents. The App displays the failure title instead of the error
document's own misleading `Done (...)` status. A normal website title alone
does not trigger this indicator.

# Direct timeouts candidate

`direct-timeouts` uses the existing nonblocking webclient API for plain HTTP.
Connect/request headers have a 15-second initial budget; received headers/body
renew the 60-second idle budget. Poll waits check cancellation every 100 ms.
Timeout/cancel/poll failures abort only an in-progress EAGAIN context, releasing
the socket exactly once. DNS still uses the system's blocking resolver.
HTTPS uses the same 15-second initial budget and existing verified TLS connector.
One pre-HTTP GET retry also covers explicit mbedTLS net BIO send/recv/reset
failures during handshake, provided certificate verification has not failed.
TLS protocol errors, POST and failures after HTTP send are not retried.

This requires a separately prepared loader exporting `poll`, `webclient_abort`
and `webclient_get_poll_info`. `prepare_openvela_network_loader.py` clones the
board-verified form-input loader, changes only its generated export object,
preserves runtime/TLS/configuration and audits the final firmware/module pair.
Do not install this module with an older loader missing these exports.

`direct-connect-retry` adds a separate opt-in TLS ops table with a 5-second
DNS/TCP budget inside the original 15-second handshake budget. It preserves the
old request layout and TLS ops API for existing callers and rollback modules.
Pre-HTTP GET failures may use at most three attempts with short cancellable
backoff, within a 30-second aggregate connection budget. Certificate, POST,
TLS protocol and response failures retain their previous non-replay policy.
The explicit source transformation is `tools/openvela_tcp_budget.py` and is
applied only to the independent candidate loader. Its export table retains
all old entries, including the original TLS ops getter.

