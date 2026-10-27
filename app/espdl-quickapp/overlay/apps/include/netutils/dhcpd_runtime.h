/* SPDX-License-Identifier: Apache-2.0 */

#ifndef __APPS_INCLUDE_NETUTILS_DHCPD_RUNTIME_H
#define __APPS_INCLUDE_NETUTILS_DHCPD_RUNTIME_H

#include <nuttx/compiler.h>
#include <stdbool.h>
#include <sys/types.h>

struct dhcpd_runtime_status_s
{
  bool running;                 /* Daemon is executing, including startup. */
  bool ready;                   /* Its current UDP 67 socket is bound. */
  int last_error;               /* Zero or negative errno. */
  pid_t pid;                    /* Child retained until its owner reaps it. */
};

/* Read-only coherent snapshot. task_create success is not DHCP readiness.
 * Start/stop must come from the same persistent parent task group so that
 * kill/wait retain the existing NuttX task ownership. A failed stop retains
 * resources and PID for that owner to recover; do not lower the interface.
 */

int dhcpd_runtime_status(FAR struct dhcpd_runtime_status_s *out);

#endif
