/* SPDX-License-Identifier: Apache-2.0 */
#ifndef NIMBLE_NUTTX_CALLOUT_H
#define NIMBLE_NUTTX_CALLOUT_H

#include <stdint.h>

/* NuttX port diagnostics, not an additional scheduler or NimBLE GAP API.
 * Dispatched counts handoff to an event queue/direct callback, not its
 * eventual application completion. Values are a mutex-protected snapshot. */
struct ble_npl_nuttx_callout_stats
{
    uint32_t expirations;
    uint32_t dispatched;
    uint32_t coalesced;
    uint32_t pending;
    uint32_t highwater;
};

void ble_npl_nuttx_callout_read(struct ble_npl_nuttx_callout_stats *stats);

#endif
