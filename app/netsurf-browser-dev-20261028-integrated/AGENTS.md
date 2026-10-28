# Browser workspace rules

Read `WORK_AREAS.md` before editing. The canonical source root is
`C:/Users/11937/Desktop/netsurf-windows`; the dedicated Vela browser directory
is an integration copy updated by tools, including this instruction file.

- Reuse the existing portable patches. Keep Windows code, portable core changes,
  OpenVela adapters, App UI and validation changes within their documented areas.
- Do not edit the frozen `dist/releases/esp32p4-0.5.0/` snapshot or replace user
  Vela changes. New outputs go under a named feature workarea.
- Shared headers and App version metadata belong to the integration task.
  A build must not mix source changes made while it runs.
- A module-only candidate must match the verified loader ABI and export set.
  Use a separate firmware workarea if additional exports or config are needed.
- Only one validation task may own COM4/the board at a time. Keep a rollback,
  validate downloads before replacing files, and restore the normal App entry.
- Distinguish host tests, cross-link checks, hardware execution and physical
  display observations. Do not infer arbitrary modern-site compatibility from
  a test page. No automatic commits, pushes, resets or workspace cleanup.

These are code ownership and output boundaries; they do not request delegation.
