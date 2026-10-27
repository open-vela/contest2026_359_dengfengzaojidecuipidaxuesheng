/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __APPS_SYSTEM_C6PROBE_C6_OTA_RELEASE_H
#define __APPS_SYSTEM_C6PROBE_C6_OTA_RELEASE_H

/* Release gate for the currently audited P4/C6 protocol pair. Do not accept
 * arbitrary ESP32-C6 images merely because their chip ID/hash is valid.
 * Expanding this gate requires matching host/co-processor protocol evidence.
 * This is an application allowlist, not ROM-enforced secure boot.
 */

#define C6_OTA_RELEASE_SHA256 \
  "087112e8e08ada7f91ee109de1e0f81553466d22482787df7d3c41178439adaf"
#define C6_OTA_RELEASE_BYTES 1113728u
#define C6_OTA_RELEASE_HOSTED "2.0.13"

#endif
