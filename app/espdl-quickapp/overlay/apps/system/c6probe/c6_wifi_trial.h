/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __APPS_SYSTEM_C6PROBE_C6_WIFI_TRIAL_H
#define __APPS_SYSTEM_C6PROBE_C6_WIFI_TRIAL_H

#include <stdbool.h>

/* The persistent network owner suspends automatic reconnect before begin.
 * begin snapshots the complete old STA configuration and connects using RAM
 * storage. Only finish(true), after DHCP succeeds, may persist the candidate.
 * finish(false) restores the old configuration without reconnecting it.
 * An uncertain failure retains the snapshot; call pending before recovery.
 * Neither operation logs credentials or performs automatic retries.
 */

int c6_wifi_trial_begin(const char *ssid, const char *password);
int c6_wifi_trial_finish(bool commit);
bool c6_wifi_trial_pending(void);

#endif
