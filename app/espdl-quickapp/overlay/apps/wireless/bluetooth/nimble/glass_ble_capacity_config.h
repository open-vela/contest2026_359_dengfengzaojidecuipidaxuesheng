/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
/* Normal images retain two sessions. The isolated eight-peer experiment
 * replaces this header in its prepared tree; every consumer shares it. */
#ifndef GLASS_BLE_CENTRAL_LIMIT
#  define GLASS_BLE_CENTRAL_LIMIT 2
#endif
#if GLASS_BLE_CENTRAL_LIMIT < 1 || GLASS_BLE_CENTRAL_LIMIT > 8
#  error Unsupported ordinary BLE session capacity
#endif
