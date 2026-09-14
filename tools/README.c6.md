# Optional C6 Wireless Overlay

Current v3 integration status (2026-09-13): the composed C6 desktop backend,
worker, scan results, link state, RPC mailbox, serialized RPC ownership and
CMD53/SDIO fixes are integrated in the isolated ESP32-P4 Function-EV OpenVela
v3 recovery tree. `CONFIG_SYSTEM_C6_DESKTOP=y` resolved, and a clean target
compile plus final firmware link completed. The final ELF contains the desktop
worker/backend, C6 network/RPC, SC2336/MIPI-CSI camera and ESP-Claw symbols. The
3,816,784-byte ESP32-P4 image has a valid checksum and remains 369,328 bytes
below the `/data` partition at `0x400000`; its SHA-256 is
`5b3432c90032126751cdc1452bc18613fa0e707bd2911902c656877ac37e17af`.
Host-side C6 protocol/backend tests, the JavaScript bridge tests and the v3
LVGL headless desktop tests pass. This is still an offline integration
candidate: no flash, C6 radio, DHCP, live Wi-Fi association, camera sensor or
display hardware acceptance has been performed. BLE remains intentionally
excluded because the present Hosted BLE transport is explicitly restricted to
pre-v3 silicon. The entries below are the chronological development record;
earlier statements that the desktop backend was not integrated are superseded
by this status block.

Backend integration candidate: composed output now includes desktop_backend.c/.h
binding scan results and c6net_connect to the worker interface. The preparation
adapter separates c6net_prepare from association; a cold scan can prepare the
radio without explicitly issuing a connect RPC or requesting stored credentials.
Backend mock tests and composition tests passed; the composed c6net and backend
compiled for RISC-V. Preparation itself is not yet runtime-tested for absence of
slave-side auto-reconnect. The Wi-Fi page still does not start or call the worker,
and the build system does not yet compile these additional units. No active-tree
replacement, full link, DHCP integration, radio operation or flash was performed.

Desktop worker component: `c6/desktop_worker.c/.h` now implements one owned
thread, serialized scan/connect requests, busy rejection and copied results
without LVGL pointers. Credentials are wiped from queued and local buffers;
results never contain passwords. Connect success means request accepted only.
Host tests passed blocked-scan busy rejection, scan results, connection timeout,
failed-scan result clearing and credential clearing. The worker compiled with
the current v1 RISC-V headers; composed preparation includes both source files.
It is not yet bound to the real C6 backend, built into the application or called
by the Wi-Fi page. Stop waits for backend completion and must not run on the UI
thread; bounded backend timeouts and application-owned lifecycle are required.
No desktop radio functionality or hardware acceptance is claimed by these tests.

Composed preparation: `prepare_c6_desktop.py SOURCE OUTPUT` now combines RX
ownership, Wi-Fi event synchronization, scan results, daemon retry and link
state adaptations. SOURCE is a mailbox-adapted system/c6probe folder; OUTPUT
must be new and outside SOURCE. All transformations are checked before writing.
The output includes the public link snapshot declaration and SHA-256 manifest.
The preparation regression passed composition, hashes, rejection of existing
output/invalid inputs and source preservation against the active v1 source.
Generated RPC and c6net translation units compiled for RISC-V; the entire
transport and final firmware link were not tested in this step. This output is
still prerequisites only: desktop worker, password entry and DHCP/IP integration
are absent. No active-tree replacement, firmware flash or radio operation occurred.

Link integration candidate: `adapt_c6_link_state.py` runs after daemon-retry
adaptation and replaces c6net's legacy link fields with the synchronized state
component. Association callbacks publish events, ifup/ifdown update the state,
and the daemon reconciles carrier under the netdev lock. Stale publication
turns carrier off until the next iteration. The generated actual c6net source
compiled for RISC-V in a temporary directory (with the existing mutex-header
compatibility prerequisite). `test_link_integration.py` passes disconnect-during-
publication, recovery and ifdown checks on the generated carrier function.
This is not active-tree integration, a full netdev concurrency test or radio
acceptance. IP state, desktop worker wiring and initialization failure cleanup
still need completion before enabling desktop scan/connect.

Link-state prerequisite: `c6/link_state.h` provides mutex-protected snapshots
and generation-checked carrier publication. Disconnect, ifdown and stop clear
carrier readiness; an obsolete carrier result returns EAGAIN. The state lock
must never cover RPC or netdev calls. The owner must reconcile the actual
netdev carrier after a stale publication; this component alone does not order
hardware/netdev operations. `test_link_state.c` passes transition, stale-ticket,
concurrent snapshot and generation-exhaustion tests on the host and compiles
with the current v1 RISC-V toolchain/headers. It is not yet wired into c6net:
the old volatile fields remain there. No IP state, desktop worker, active-tree
integration, full firmware build or radio acceptance is claimed in this step.

Initialization prerequisite: `adapt_c6_daemon_retry.py` retains registration
ownership after task creation fails and retries only daemon startup, avoiding
memset/re-registration of a live netdev. Connect routes through the existing
initialization mutex. `test_daemon_retry.py` executes adapted initialization
with task-create failure injection and passes retry/reconnect without duplicate
registration. This is a host-only candidate, not active-tree integration or
hardware validation. Registration failure cleanup and shared volatile link
state remain unresolved; desktop scan/connect is still incomplete.

Event prerequisite: `adapt_c6_wifi_events.py` serializes callback/argument
registration, invocation and unregister with one mutex. Unregister waits for
an in-flight callback; callbacks must only publish state and must not reenter
RPC, registration or connection locks. `test_wifi_events.py` passed concurrent
callback replacement and dispatch, including no calls after unregister.
A temporary copy of the active mailbox-adapted RPC source accepted this adapter
and the scan-results adapter and compiled for RISC-V. No active tree files were
modified. c6net's volatile association/carrier/event flags still need a coherent
state contract; desktop scan/connect, IP acquisition and hardware acceptance
remain incomplete. These prerequisite tests do not constitute feature delivery.

RX prerequisite: `adapt_c6_rx_dispatch.py` prepares a separate RX ownership
mutex around read and dispatch, while releasing the bus before callbacks.
Competing pollers return zero instead of waiting behind a callback. Interface
registration shares the RX mutex, and dispatch rejects out-of-range interface
IDs. Callbacks must not register handlers or issue synchronous RPCs.
`test_rx_dispatch.py` compiles the actual adapted poll/register functions and
passes deterministic two-thread contention, bus-unlocked callback, read-error,
empty-read and disconnected cleanup checks. This is a host test, not a full
transport or target build. The adapter has not been applied to the active tree.
Wi-Fi event callback synchronization, connection state and desktop workers
remain unresolved; do not enable concurrent desktop networking on this basis.

Desktop prerequisite: `adapt_c6_scan_results.py` adds caller-owned scan records
through `c6/scan_results.h`, while retaining the diagnostic printing wrapper.
The response is bounded to ten APs and validates response payloads, record
pointers and SSID byte lengths before returning a count. SSIDs retain an
explicit byte length; they must not be assumed to be UTF-8 by the future UI.
The header boundary test and the adapted actual RPC source RISC-V compilation
passed in a temporary directory. This does not validate radio responses or
scan concurrency. The adapter has not been applied to the active build tree.
Desktop scan/connect remains disabled pending shared RX dispatch ownership,
worker lifecycle and association/IP status integration. This is not a working
desktop wireless feature and no device/network operation was performed.

RPC mailbox integration: tools/adapt_c6_rpc_mailbox.py now applies a real
mutex-protected response slot after the size and transaction-serialization
patches. Each transaction uses a new nonzero UID; IDs are not reused after
exhaustion. Duplicate, late and mismatched responses remain owned by RX and
are freed there. Closing the response window transfers accepted ownership
to the caller atomically; send/poll failure drains and frees accepted replies.
Two unused legacy busy assignments were removed. The actual RPC source passed
RISC-V compilation in a temporary copy; mailbox race and size regressions passed.
The adaptation was then applied to the isolated openvela tree; a second run
made no changes. It has not been included in a new flashed firmware.

Remaining concurrency work includes shared Hosted receive-buffer dispatch and
callback registration with multiple pollers. This mailbox alone does not make
simultaneous CLI/network-daemon/desktop operation safe. Structured scan-result
delivery and desktop worker integration remain pending.

RPC caller serialization: hosted-rpc-serialize.patch wraps complete RPC
transactions in a pthread mutex. The actual wrapper passed 200 calls across
four host threads, including failed transactions; target RISC-V compilation
and the serialization-size regression also passed. Apply after hosted-rpc-size.
This does not synchronize the asynchronous RX callback with timeout cleanup,
fix reused UIDs, or make the shared Hosted receive buffer safe with concurrent
pollers. RX/event callbacks must not recursively issue synchronous RPCs.
Do not enable concurrent desktop/c6net/CLI operation until these remaining
ownership issues are resolved. No new firmware was flashed in this step.

RPC hardening: hosted-rpc-size.patch checks rpc__get_packed_size before
serializing into the 256-byte request buffer, and checks the returned packed
length before TLV composition. The previous order wrote before checking.
test_rpc_size.py exercises the actual serialization prefix with mocked
protobuf operations: 257-byte rejection, 256-byte boundary and empty input
passed ASan/UBSan. The patched RPC source also compiled for RISC-V against
the current isolated openvela tree. No new firmware was flashed for this fix.

The WSL integration tree still exists; an earlier Windows-only path check
incorrectly reported it missing. Desktop C6 status/scan worker integration is
not complete. The c6_status files in the external reference directory remain
unintegrated placeholders and must not be treated as synchronized live status.

Latest build: the explicit openvela-cmd53-read-errors.patch checks receive
setup and command submission results, cancels waiting and unlocks on failure.
The actual-function regression covers ENOSYS, send failure and success.
The independent v1 build completed with DMA enabled; its exported candidate
is artifacts/c6-wifi-v1-dma-cmd53-20260911 in the parent workspace, 3,707,424
bytes. Image checksum, configuration, symbols and hashes passed. It has not
been flashed or radio-tested. Earlier board timeout results concern older
non-DMA firmware and must not be attributed to this new candidate.

Latest correction: configure_c6.py now enables and verifies both
ESP32P4_SDMMC_DMA and SDIO_DMA. The former non-DMA setting was incompatible
with openvela's CMD53 helper, which calls DMA setup and ignores ENOSYS when
DMA is disabled. The v1 180 MHz DMA candidate fully compiled and was exported
to artifacts/c6-wifi-v1-dma-20260911 in the parent workspace. Its final ELF
contains both DMA setup functions; image checksum and file hashes passed.
It has not been flashed or radio-tested. Earlier non-DMA instructions below
are historical, not the current required configuration.

Status: source delivery only, not a working Wi-Fi/Bluetooth release.

2026-09-11: full v1 build succeeded in the isolated integration tree after
also applying openvela-locks.patch (nuttx) and openvela-net-lock.patch (apps).
These add the real spinlock/mutex declarations, without changing lock semantics.
The final ELF contains c6probe_main, esp_hosted_initialize and c6net_initialize;
the 180 MHz BIN is 3,706,116 bytes. Export is in the parent workspace's
artifacts/c6-wifi-v1-20260911. Image checksum and file hashes passed.
No firmware was flashed and no C6 command was executed. The compatibility
patches remain explicit separate steps, not automatically managed by the base
overlay script. TLS archives must be revalidated for the changed configuration
before online TLS acceptance. Bluetooth HCI stack adaptation remains pending.

Configuration progress: configure_c6.py resolves SDMMC, c6probe and Ethernet
dependencies in the isolated v1 workspace. It verifies Function-EV pins and
sets MMCSD_MULTIBLOCK_LIMIT=128, as required by the reference driver; DMA is
disabled for initial bring-up. The command registry now contains c6probe.
The openvela tree uses debug.h, not nuttx/debug.h: apply the separate
patches/c6/openvela-debug.patch after the reference overlays (check with
git apply --check first). This compatibility patch is not yet automatically
managed by apply_c6_overlays.py. It was applied to the isolated v1 workspace.
After these corrections, top-level pass2dep succeeded. Full compilation,
linking, radio handshake and Wi-Fi/BLE operation remain unverified.

Example configuration (WSL):
```
python3 tools/configure_c6.py /path/to/isolated-workspace \
  --toolchain /path/to/riscv32-esp-elf/bin --configure
```

Pinned streetartist feature/esp-hosted-c6 patches are stored in patches/c6
with commit identities and SHA256 in manifest.json. The kernel patch includes
only SDMMC source/header and Kconfig/Make integration. The applications patch
contains system/c6probe, Hosted RPC and c6net. Camera, Flash and desktop changes
from the reference branch are intentionally excluded.

Run `python3 tools/apply_c6_overlays.py /path/to/workspace` for check-only mode.
The workspace must contain nuttx and apps. Both patches are verified and
preflighted before writes. Explicit --apply applies them; it does not configure,
build, flash or start the radio. Do not run concurrent workspace edits.

The Make.defs patch now anchors on the SMP block rather than the preceding
CHIP_CSRCS line, preserving openvela's esp_atomic64.c addition. Both patches
passed preflight and were applied to the isolated glass-claw-v1-20260910
workspace. Repeated checks passed reverse application, confirming the applied
state. Three offline tests cover hashes/scope, read-only checks and preservation
of the atomic source through a real patch application. Reference protobuf
sources retain upstream trailing-whitespace warnings.

Configuration resolution, NuttX API compatibility and compilation still need
validation before enabling the radio. This optional overlay is not silently
added to apply_final_overlays.sh. No c6probe command or C6 reset was executed.

Reference wiring: SDIO slot 1, CMD19, CLK18, D0..D3=14..17, C6 reset GPIO54.
The reference follows ESP-Hosted 2.12.12. c6probe initialization resets C6;
it is not a read-only query. Installed C6 firmware and wiring remain to be
verified. Wi-Fi scan/association code exists; Bluetooth has HCI framing but no
registered NuttX controller adapter, so BLE is not yet available.

No new BIN, radio connection, or board verification is claimed. Run offline
delivery checks with `python3 tools/tests/test_c6_overlays.py`.

The board test returned `R4: -22` before this correction. The SDMMC
`recvshort()` callback was assigned to R4 but its debug validation accepted
only R3/R7; the CRC short-response callback also omitted R5. The separate
`openvela-sdio-responses.patch` adds valid R4/R5 response types while keeping
timeout and CRC checks. The extracted-function regression passes legal R4/R5,
wrong-type rejection, timeout and CRC failure cases. Apply it before the next
build and repeat the probe; no radio result is inferred from this host test.
