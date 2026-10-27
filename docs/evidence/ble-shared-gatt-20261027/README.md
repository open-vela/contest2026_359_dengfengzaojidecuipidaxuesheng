# Candidate 18 BLE shared GATT evidence

Current summary: RUNTIME.json. Full byte-verified runs: channel-c18-05.json (before warm reboot), channel-c18-07.json (after warm reboot). Native first pairing and existing-bond query have their own JSON. Correct disconnect check: channel-lifecycle-18b.json.

channel-lifecycle-18.json is INVALIDATED by channel-lifecycle-18-INVALIDATED.json: its old substring assertion incorrectly accepted -116 as -11. Preserve it only as raw history. It includes a genuine transient timeout (reason 520), followed by recovery; no claim that its cause was fixed. Follow-up exact-line check passed. Other failed test-client attempts are retained as described in RUNTIME.json.

Host logs replace NimBLE transport/callbacks or use pinned SDK; they do not prove three real links. App-only deployment readback and whole data-partition preservation passed. Full Flash backups/user data remain outside the repository. Helper scripts are historical copies from output/ble-channel-20261027 and retain that original directory convention.
