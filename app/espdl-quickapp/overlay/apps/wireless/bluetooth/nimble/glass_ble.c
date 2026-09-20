/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "glass_ble.h"
#include "glass_ble_hid_internal.h"
#include "glass_ble_store.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_register.h"

static pthread_mutex_t g_host_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_started, g_ready;
static int g_error;
extern void ble_hci_sock_ack_handler(void *arg);

static void reset(int reason)
{
  pthread_mutex_lock(&g_host_lock);
  g_ready = false;
  g_error = -EIO;
  pthread_mutex_unlock(&g_host_lock);
  glass_ble_hid_reset(reason);
}

static void synced(void)
{
  uint8_t address;
  int ret = ble_hs_util_ensure_addr(0);
  if (!ret) ret = ble_hs_id_infer_auto(0, &address);
  if (!ret) glass_ble_hid_synced(address);
  pthread_mutex_lock(&g_host_lock);
  g_ready = ret == 0;
  g_error = ret ? -EIO : 0;
  pthread_mutex_unlock(&g_host_lock);
}

static void *hci_thread(void *unused)
{
  ble_hci_sock_ack_handler(unused);
  return NULL;
}

/* A NuttX task owns the host and HCI pthread. Unlike a detached pthread
 * created by c6ble_main, this task survives the short-lived NSH command. */
static int host_main(int argc, char **argv)
{
  pthread_attr_t attr;
  pthread_t hci;
  int ret;
  (void)argc;
  (void)argv;
  ret = c6_ble_register("/dev/ttyHCI0");
  if (!ret)
    {
      nimble_port_init();
      ble_svc_gap_init();
      ble_svc_gatt_init();
      glass_ble_store_init();
      ret = glass_ble_hid_service_init();
      if (!ret)
        {
          ble_hs_cfg.sync_cb = synced;
          ble_hs_cfg.reset_cb = reset;
          ret = pthread_attr_init(&attr);
          if (!ret)
            {
              ret = pthread_attr_setstacksize(&attr, 32768);
              if (!ret) ret = pthread_create(&hci, &attr, hci_thread, NULL);
              pthread_attr_destroy(&attr);
              if (!ret) nimble_port_run();
            }
        }
    }
  pthread_mutex_lock(&g_host_lock);
  /* NimBLE cannot safely be initialized twice after a partial failure. */
  g_error = ret < 0 ? ret : -EIO;
  g_ready = false;
  pthread_mutex_unlock(&g_host_lock);
  return 1;
}

int glass_ble_start(void)
{
  int ret = 0;
  pthread_mutex_lock(&g_host_lock);
  if (!g_started)
    {
      if (task_create("ble-mouse", 100, 32768, host_main, NULL) < 0)
        ret = -errno;
      else
        g_started = true;
    }
  else ret = g_error;
  pthread_mutex_unlock(&g_host_lock);
  return ret;
}

int glass_ble_host_wait(void)
{
  int ret = glass_ble_start();
  if (ret) return ret;
  /* Hosted C6 startup can include controller firmware/RPC initialization.
   * Keep this off the UI thread and allow the first boot to settle. */
  for (unsigned i = 0; i < 1500; ++i)
    {
      bool ready;
      pthread_mutex_lock(&g_host_lock);
      ready = g_ready;
      ret = g_error;
      pthread_mutex_unlock(&g_host_lock);
      if (ready || ret) return ret;
      usleep(10000);
    }
  return -ETIMEDOUT;
}

void glass_ble_read(struct glass_ble_state *state)
{
  struct glass_ble_hid_state hid;
  if (!state) return;
  memset(state, 0, sizeof(*state));
  glass_ble_hid_read(&hid);
  pthread_mutex_lock(&g_host_lock);
  state->started = g_started;
  state->ready = g_ready;
  state->error = g_error;
  pthread_mutex_unlock(&g_host_lock);
  state->connected = hid.connected;
}
int glass_ble_scan(void) { return -ENOTSUP; }
int glass_ble_connect(unsigned index) { (void)index; return -ENOTSUP; }
int glass_ble_cancel(void) { return 0; }
int glass_ble_disconnect(void) { return glass_ble_hid_stop(); }
