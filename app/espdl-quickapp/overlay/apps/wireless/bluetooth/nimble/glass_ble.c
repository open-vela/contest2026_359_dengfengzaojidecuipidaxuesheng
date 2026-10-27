/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/sched.h>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include "glass_ble.h"
#include "glass_ble_candidate.h"
#include "glass_ble_hid_internal.h"
#include "glass_ble_store.h"
#include "glass_ble_central.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_register.h"
#include "syscfg/syscfg.h"

#if MYNEWT_VAL_BLE_MAX_CONNECTIONS != CONFIG_NIMBLE_BLE_MAX_CONN
#  error "NimBLE connection capacity does not match NuttX configuration"
#endif

unsigned glass_ble_capacity(void)
{
  return MYNEWT_VAL(BLE_MAX_CONNECTIONS);
}

const char *glass_ble_identity_name(void)
{
  return GLASS_BLE_CANDIDATE_NAME;
}

const char *glass_ble_identity_address(void)
{
  return GLASS_BLE_CANDIDATE_ADDRESS_TEXT;
}

static pthread_mutex_t g_host_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_started, g_ready;
static int g_error;
/* Central reserves one slot per active/pending/closing physical link.
 * HID reserves one only after central has physically paused. The two roles
 * are mutually exclusive even when the controller has spare capacity.
 * Claims are idempotent. Owners hold their state lock through claim/release;
 * this lock never calls back into an owner. Closing links retain their slot.
 */
static unsigned g_roles;
static unsigned g_central_reservations;
int glass_ble_central_reserve(unsigned count)
{
  int ret = 0;
  if (count > GLASS_BLE_CENTRAL_LIMIT) return -EINVAL;
  pthread_mutex_lock(&g_host_lock);
  if (count && (g_roles & GLASS_BLE_ROLE_HID)) ret = -EBUSY;
  else if (count > glass_ble_capacity())
    ret = -ENOSPC;
  if (!ret)
    {
      g_central_reservations = count;
      if (count) g_roles |= GLASS_BLE_ROLE_CENTRAL;
      else g_roles &= ~GLASS_BLE_ROLE_CENTRAL;
    }
  pthread_mutex_unlock(&g_host_lock);
  return ret;
}
int glass_ble_role_claim(unsigned role)
{
  unsigned used;
  int ret = 0;

  if (role != GLASS_BLE_ROLE_CENTRAL && role != GLASS_BLE_ROLE_HID)
    return -EINVAL;

  pthread_mutex_lock(&g_host_lock);
  used = g_central_reservations +
         !!(g_roles & GLASS_BLE_ROLE_HID);
  if ((role == GLASS_BLE_ROLE_HID && g_central_reservations) ||
      (role == GLASS_BLE_ROLE_CENTRAL && (g_roles & GLASS_BLE_ROLE_HID)))
    ret = -EBUSY;
  else if (!(g_roles & role) && used >= glass_ble_capacity()) ret = -ENOSPC;
  if (!ret)
    {
      g_roles |= role;
      if (role == GLASS_BLE_ROLE_CENTRAL && !g_central_reservations)
        g_central_reservations = 1;
    }
  pthread_mutex_unlock(&g_host_lock);
  return ret;
}
void glass_ble_role_release(unsigned role)
{
  if (role != GLASS_BLE_ROLE_CENTRAL && role != GLASS_BLE_ROLE_HID) return;
  pthread_mutex_lock(&g_host_lock);
  g_roles &= ~role;
  if (role == GLASS_BLE_ROLE_CENTRAL) g_central_reservations = 0;
  pthread_mutex_unlock(&g_host_lock);
}

void glass_ble_capacity_read(struct glass_ble_capacity_state *state)
{
  if (!state) return;
  pthread_mutex_lock(&g_host_lock);
  state->capacity = glass_ble_capacity();
  state->central_reserved = !!(g_roles & GLASS_BLE_ROLE_CENTRAL);
  state->hid_reserved = !!(g_roles & GLASS_BLE_ROLE_HID);
  state->central_reservations = g_central_reservations;
  state->reserved = g_central_reservations +
                    (unsigned)state->hid_reserved;
  pthread_mutex_unlock(&g_host_lock);
}
extern void ble_hci_sock_ack_handler(void *arg);

static void reset(int reason)
{
  pthread_mutex_lock(&g_host_lock);
  g_ready = false;
  g_error = -EIO;
  pthread_mutex_unlock(&g_host_lock);
  glass_ble_hid_reset(reason);
  glass_ble_central_reset();
}

static void synced(void)
{
  static const uint8_t identity[6] = GLASS_BLE_CANDIDATE_ADDRESS;
  uint8_t actual[6];
  uint8_t address;
  int nrpa;
  int ret = ble_hs_util_ensure_addr(0);
  if (!ret) ret = ble_hs_id_set_rnd(identity);
  if (!ret) ret = ble_hs_id_copy_addr(BLE_ADDR_RANDOM, actual, &nrpa);
  if (!ret && (nrpa || memcmp(actual, identity, sizeof(actual)))) ret = BLE_HS_EINVAL;
  if (!ret) ret = ble_hs_id_infer_auto(0, &address);
  if (!ret && address != BLE_OWN_ADDR_RANDOM) ret = BLE_HS_EINVAL;
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
static void *command_thread(void *unused)
{
  (void)unused;
  /* Shares the host's descriptor table. NPL's mq descriptor cannot be
   * posted by callers in the desktop/NSH task group. NimBLE GAP/GATT APIs
   * take their own host lock; callbacks still run on the event thread. */
  for(;;){glass_ble_central_wait();glass_ble_central_poll();}
  return NULL;
}

/* A NuttX task owns the host and HCI pthread. Unlike a detached pthread
 * created by c6ble_main, this task survives the short-lived NSH command. */
static int host_main(int argc, char **argv)
{
  pthread_attr_t attr;
  pthread_t hci,commands;
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
      if (!ret) ret = glass_ble_central_init();
      if (!ret)
        {
          ble_hs_cfg.sync_cb = synced;
          ble_hs_cfg.reset_cb = reset;
          ret = pthread_attr_init(&attr);
          if (!ret)
            {
              ret = pthread_attr_setstacksize(&attr, 32768);
              if (!ret) ret = pthread_create(&hci, &attr, hci_thread, NULL);
              if (!ret) ret = pthread_attr_setstacksize(&attr, 8192);
              if (!ret) ret = pthread_create(&commands, &attr, command_thread, NULL);
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
  if (!state) return;
  memset(state, 0, sizeof(*state));
  pthread_mutex_lock(&g_host_lock);
  state->started = g_started;
  state->ready = g_ready;
  state->error = g_error;
  pthread_mutex_unlock(&g_host_lock);
  glass_ble_central_read(state);
  state->store_error = glass_ble_store_last_error();
  if (state->store_error)
    {
      state->bonded = false;
      state->peer.bonded = false;
      for (unsigned i = 0; i < state->session_count &&
           i < GLASS_BLE_CENTRAL_LIMIT; ++i)
        {state->sessions[i].bonded = false;state->sessions[i].peer.bonded = false;}
    }
}
int glass_ble_scan(void) { return glass_ble_central_scan(); }
int glass_ble_connect(unsigned index) { return glass_ble_central_connect(index,0); }
int glass_ble_cancel(void) { return glass_ble_central_cancel(); }
int glass_ble_disconnect(void) { return glass_ble_central_disconnect(); }
