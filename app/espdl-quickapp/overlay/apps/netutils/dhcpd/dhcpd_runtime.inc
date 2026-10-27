/* SPDX-License-Identifier: Apache-2.0 */

#include <pthread.h>
#include <stdatomic.h>

#include "netutils/dhcpd_runtime.h"

#if ATOMIC_INT_LOCK_FREE != 2
#  error "DHCP stop signal requires lock-free integer atomics"
#endif

#if defined(CONFIG_BUILD_KERNEL) || defined(CONFIG_DISABLE_SIGNALS)
#  error "DHCP shared runtime requires task-mode signals"
#endif

/* The upstream server is a singleton. Lifecycle operations and snapshots
 * use separate locks: the parent may wait for the child while that child
 * publishes startup/exit. Signal context only changes atomic ds_state.
 */

static pthread_mutex_t g_dhcpd_control = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_dhcpd_status_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_dhcpd_status_cond = PTHREAD_COND_INITIALIZER;
static struct dhcpd_runtime_status_s g_dhcpd_status = {false, false, 0, -1};
static pid_t g_dhcpd_child = -1;
static bool g_dhcpd_starting;
static bool g_dhcpd_can_signal;

int dhcpd_runtime_status(FAR struct dhcpd_runtime_status_s *out)
{
  int ret;

  if (out == NULL)
    {
      return -EINVAL;
    }

  ret = pthread_mutex_lock(&g_dhcpd_status_lock);
  if (ret != 0)
    {
      return -ret;
    }

  *out = g_dhcpd_status;
  if (g_dhcpd_daemon.ds_state == DHCPD_STOP_REQUESTED)
    {
      out->ready = false;
    }

  pthread_mutex_unlock(&g_dhcpd_status_lock);
  return 0;
}

static void dhcpd_runtime_listener(bool ready, int error)
{
  pthread_mutex_lock(&g_dhcpd_status_lock);
  g_dhcpd_status.ready = ready;
  g_dhcpd_status.last_error = error;
  pthread_mutex_unlock(&g_dhcpd_status_lock);
}

static int dhcpd_runtime_enter(bool managed)
{
  int ret = pthread_mutex_lock(&g_dhcpd_status_lock);

  if (ret != 0)
    {
      return -ret;
    }

  if (g_dhcpd_status.running || (!managed && g_dhcpd_starting))
    {
      ret = -EBUSY;
    }
  else
    {
      g_dhcpd_starting = true;
      g_dhcpd_status.running = true;
      g_dhcpd_status.ready = false;
      g_dhcpd_status.last_error = 0;
      g_dhcpd_status.pid = getpid();
      g_dhcpd_daemon.ds_state = DHCPD_STARTED;
      ret = 0;
    }

  pthread_mutex_unlock(&g_dhcpd_status_lock);
  return ret;
}

static void dhcpd_runtime_started(void)
{
  int expected = DHCPD_STARTED;

  /* Do not overwrite a signal delivered immediately after installation. */

  atomic_compare_exchange_strong(&g_dhcpd_daemon.ds_state, &expected,
                                 DHCPD_RUNNING);
  pthread_mutex_lock(&g_dhcpd_status_lock);
  g_dhcpd_starting = false;
  g_dhcpd_can_signal = true;
  pthread_cond_broadcast(&g_dhcpd_status_cond);
  pthread_mutex_unlock(&g_dhcpd_status_lock);
}

static void dhcpd_runtime_exited(int error)
{
  pthread_mutex_lock(&g_dhcpd_status_lock);
  g_dhcpd_status.running = false;
  g_dhcpd_status.ready = false;
  g_dhcpd_status.last_error = error;
  g_dhcpd_starting = false;
  g_dhcpd_can_signal = false;
  pthread_cond_broadcast(&g_dhcpd_status_cond);
  pthread_mutex_unlock(&g_dhcpd_status_lock);
}
