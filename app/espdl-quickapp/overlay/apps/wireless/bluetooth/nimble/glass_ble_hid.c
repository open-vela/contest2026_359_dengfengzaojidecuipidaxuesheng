/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble.h"
#include "glass_ble_candidate.h"
#include "glass_ble_hid_internal.h"
#include "glass_ble_store.h"
#include "glass_ble_central.h"
#include "glass_ble_channel.h"
#include "glass_ble_hid_link.h"
#include "mouse_core.h"
#include <errno.h>
#include <pthread.h>
#include <syslog.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"

#define HID_UUID 0x1812
#define HID_INFO 0x2a4a
#define HID_MAP 0x2a4b
#define HID_CONTROL 0x2a4c
#define HID_REPORT 0x2a4d
#define HID_PROTOCOL 0x2a4e
#define HID_BOOT_MOUSE 0x2a33
#define HID_REF 0x2908
#define PAIR_WINDOW_MS 120000
#define RPC_TIMEOUT_SEC 10
#define READ_SEC (BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_api = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done = PTHREAD_COND_INITIALIZER;
static struct mouse_core g_mouse;
static uint16_t g_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_report_handle, g_boot_handle;
static uint8_t g_address;
static bool g_initialized, g_ready, g_advertising, g_stop_pending;
static bool g_user_enabled;
static bool g_bond_pending;
static unsigned g_diagnostic_count;
static uint32_t g_stop_generation;
static int g_ble_error, g_stop_result;
static struct ble_npl_callout g_watchdog;
static const char g_name[] = GLASS_BLE_CANDIDATE_NAME;

enum operation { OP_START, OP_MOVE, OP_RELEASE };
static struct request
{
  bool pending, done, cancelled;
  enum operation op;
  int buttons, x, y, wheel, result;
  uint32_t epoch, stop_generation;
} g_request;

static uint64_t now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static struct glass_ble_hid_link g_link;
static uint32_t g_input_session, g_input_counter, g_input_epoch;
static uint64_t g_request_until;
static uint64_t g_business_poll_at;
/* Retain the previous link's bounded, non-secret diagnostics after automatic
 * reconnect resets mouse_core and g_link. Counts include intentional stops. */
static uint32_t g_disconnects,g_unexpected_disconnects,g_last_disconnect_epoch;
static int g_last_disconnect_reason;
static unsigned g_last_parameter_attempts,g_last_parameter_updates;
static uint16_t g_last_interval;
static uint64_t g_last_secure_age_ms;
static void hid_disconnect_record(int reason)
{
  g_disconnects++;
  if(reason!=BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_LOCAL))g_unexpected_disconnects++;
  g_last_disconnect_reason=reason;g_last_disconnect_epoch=g_mouse.epoch;
  g_last_parameter_attempts=g_link.attempts;g_last_parameter_updates=g_link.changed;
  g_last_interval=g_link.interval;
  g_last_secure_age_ms=g_link.secured_at?now_ms()-g_link.secured_at:0;
}

static void input_cancel_locked(void)
{
  g_input_session=0;
  mouse_core_cancel_input(&g_mouse,now_ms());
}

/* Only called by NimBLE event-queue callbacks, never by command threads.
 * notify_custom consumes the mbuf even on failure. */
static int transport_send(void *arg, const uint8_t *report, size_t length,
                          bool boot)
{
  struct os_mbuf *packet;
  int ret;
  (void)arg;
  packet = ble_hs_mbuf_from_flat(report, (uint16_t)length);
  if (!packet) return -ENOMEM;
  ret = ble_gatts_notify_custom(g_conn,
                                boot ? g_boot_handle : g_report_handle, packet);
  g_ble_error = ret;
  if(!ret)return 0;
  if(ret==BLE_HS_ENOMEM)return -ENOMEM;
  if(ret==BLE_HS_EBUSY||ret==BLE_HS_EAGAIN)return -EAGAIN;
  if(ret==BLE_HS_ENOTCONN)return -ENOTCONN;
  if(ret==BLE_HS_EAUTHEN||ret==BLE_HS_EAUTHOR||ret==BLE_HS_EENCRYPT)return -EACCES;
  return -EIO; /* Unknown/ambiguous failures must not be replayed. */
}

static int stop_advertising(void)
{
  int ret = 0;
  if (ble_gap_adv_active()) ret = ble_gap_adv_stop();
  if (ret == BLE_HS_EALREADY) ret = 0;
  g_ble_error = ret;
  if (!ret) g_advertising = false;
  return ret ? -EIO : 0;
}

static int disconnect_if_needed(void)
{
  int ret;
  if (!g_mouse.disconnect_required || g_conn == BLE_HS_CONN_HANDLE_NONE)
    return 0;
  ret = ble_gap_terminate(g_conn, BLE_ERR_REM_USER_CONN_TERM);
  if (!ret || ret == BLE_HS_ENOTCONN || ret == BLE_HS_EALREADY)
    {
      g_mouse.disconnect_required = false;
      return 0;
    }
  g_ble_error = ret;
  g_mouse.error = -EIO;
  return -EIO; /* Watchdog retries; input stays disabled. */
}

static int append(struct os_mbuf *om, const void *data, size_t length)
{
  return os_mbuf_append(om, data, length) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static int hid_access(uint16_t conn, uint16_t handle,
                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
  static const uint8_t info[] = {0x11, 0x01, 0, 0x02};
  static const uint8_t reference[] = {MOUSE_REPORT_ID, 1};
  uintptr_t kind = (uintptr_t)arg;
  uint8_t value, report[4] = {0};
  uint16_t copied = 0;
  int ret = BLE_ATT_ERR_UNLIKELY;
  (void)handle;
  pthread_mutex_lock(&g_lock);
  if (g_conn == BLE_HS_CONN_HANDLE_NONE || conn != g_conn)
    {
      pthread_mutex_unlock(&g_lock);
      return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_DSC && kind == HID_REF)
    ret = append(ctxt->om, reference, sizeof(reference));
  else if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR)
    {
      if (kind == HID_INFO) ret = append(ctxt->om, info, sizeof(info));
      else if (kind == HID_MAP)
        ret = append(ctxt->om, mouse_report_map, mouse_report_map_size);
      else if (kind == HID_PROTOCOL)
        {
          value = g_mouse.boot ? 0 : 1;
          ret = append(ctxt->om, &value, 1);
        }
      else if (kind == HID_REPORT || kind == HID_BOOT_MOUSE)
        {
          /* Reading a relative report must not replay old movement. */
          report[0] = g_mouse.buttons;
          ret = append(ctxt->om, report, kind == HID_BOOT_MOUSE ? 3 : 4);
        }
    }
  else if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
           (kind == HID_PROTOCOL || kind == HID_CONTROL))
    {
      if (OS_MBUF_PKTLEN(ctxt->om) != 1)
        ret = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
      else if (!ble_hs_mbuf_to_flat(ctxt->om, &value, 1, &copied) &&
               copied == 1 && value <= 1)
        {
          input_cancel_locked();
          int result = kind == HID_PROTOCOL ?
            mouse_core_mode(&g_mouse, value, transport_send, NULL) :
            mouse_core_suspend(&g_mouse, value, transport_send, NULL);
          ret = result ? BLE_ATT_ERR_UNLIKELY : 0;
          (void)disconnect_if_needed();
        }
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

static struct ble_gatt_dsc_def g_descriptors[] = {
  {.uuid = BLE_UUID16_DECLARE(HID_REF),
   .att_flags = BLE_ATT_F_READ | BLE_ATT_F_READ_ENC,
   .access_cb = hid_access, .arg = (void *)(uintptr_t)HID_REF}, {0}
};
static const struct ble_gatt_chr_def g_characteristics[] = {
  {.uuid = BLE_UUID16_DECLARE(HID_INFO), .access_cb = hid_access,
   .arg = (void *)(uintptr_t)HID_INFO, .flags = READ_SEC},
  {.uuid = BLE_UUID16_DECLARE(HID_MAP), .access_cb = hid_access,
   .arg = (void *)(uintptr_t)HID_MAP, .flags = READ_SEC},
  {.uuid = BLE_UUID16_DECLARE(HID_PROTOCOL), .access_cb = hid_access,
   .arg = (void *)(uintptr_t)HID_PROTOCOL,
   .flags = READ_SEC | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
  {.uuid = BLE_UUID16_DECLARE(HID_CONTROL), .access_cb = hid_access,
   .arg = (void *)(uintptr_t)HID_CONTROL,
   .flags = BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC},
  {.uuid = BLE_UUID16_DECLARE(HID_REPORT), .access_cb = hid_access,
   .arg = (void *)(uintptr_t)HID_REPORT, .descriptors = g_descriptors,
   .flags = READ_SEC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_report_handle},
  {.uuid = BLE_UUID16_DECLARE(HID_BOOT_MOUSE), .access_cb = hid_access,
   .arg = (void *)(uintptr_t)HID_BOOT_MOUSE,
   .flags = READ_SEC | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_boot_handle}, {0}
};
static const struct ble_gatt_svc_def g_services[] = {
  {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(HID_UUID),
   .characteristics = g_characteristics}, {0}
};

static int advertise(void);

static int hid_repeat_pairing(uint16_t conn)
{
  struct ble_gap_conn_desc desc;
  bool owned;
  int ret;

  pthread_mutex_lock(&g_lock);
  owned = g_conn != BLE_HS_CONN_HANDLE_NONE && conn == g_conn;
  pthread_mutex_unlock(&g_lock);
  if (!owned)
    {
      return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }

  ret = ble_gap_conn_find(conn, &desc);
  if (!ret)
    {
      ret = ble_store_util_delete_peer(&desc.peer_id_addr);
    }

  return ret ? BLE_GAP_REPEAT_PAIRING_IGNORE : BLE_GAP_REPEAT_PAIRING_RETRY;
}

/* g_lock is held by the GAP callback. In the pinned NimBLE implementation,
 * ble_sm_connection_broken() emits ENOTCONN before the final DISCONNECT
 * callback. It is a teardown notification, not a new authentication failure.
 * Do not send an extra terminate for a controller handle already removed. */
static void hid_encryption_changed(uint16_t connection, int status)
{
  struct ble_gap_conn_desc desc;
  int ret;

  if (connection != g_conn) return;
  if (status == BLE_HS_ENOTCONN)
    {
      g_mouse.encrypted = false;
      g_bond_pending = false;
      return;
    }
  ret = ble_gap_conn_find(g_conn, &desc);
  g_mouse.encrypted = !ret && !status && desc.sec_state.encrypted &&
                      desc.sec_state.key_size == 16;
  if (!g_mouse.encrypted)
    {
      if (g_diagnostic_count++ < 8)
        syslog(LOG_WARNING,
               "[ble-hid] encryption find=%d status=%d key_size=%u\n",
               ret, status, ret ? 0u : (unsigned)desc.sec_state.key_size);
      g_ble_error = ret ? ret : status;
      g_mouse.error = -EACCES;
      g_mouse.enabled = false;
      g_mouse.disconnect_required = true;
      (void)disconnect_if_needed();
    }
  else if (desc.sec_state.bonded)
    {
      /* SMP persists keys after this callback returns. Only the event-queue
       * watchdog may consume this flag after SMP exits. */
      g_bond_pending = true;
    }
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
  int ret;
  (void)arg;
  /* notify_custom can synchronously emit NOTIFY_TX. Do not take g_lock
   * for events which cannot change our state (notably NOTIFY_TX). */
  switch (event->type)
    {
      case BLE_GAP_EVENT_CONNECT:
      case BLE_GAP_EVENT_DISCONNECT:
      case BLE_GAP_EVENT_ENC_CHANGE:
      case BLE_GAP_EVENT_SUBSCRIBE:
      case BLE_GAP_EVENT_ADV_COMPLETE:
      case BLE_GAP_EVENT_CONN_UPDATE:
        break;
      case BLE_GAP_EVENT_REPEAT_PAIRING:
        return hid_repeat_pairing(event->repeat_pairing.conn_handle);
      default: return 0;
    }
  pthread_mutex_lock(&g_lock);
  switch (event->type)
    {
      case BLE_GAP_EVENT_CONNECT:
        g_advertising = false;
        if (event->connect.status)
          {
            g_mouse.enabled = false;
            g_mouse.error = -ECONNREFUSED;
            g_ble_error = event->connect.status;
            break;
          }
        g_conn = event->connect.conn_handle;
        g_bond_pending = false;
        mouse_core_connect(&g_mouse);
        if (!g_mouse.enabled || g_stop_pending)
          {
            g_mouse.disconnect_required = true;
            (void)disconnect_if_needed();
            break;
          }
        ret = ble_gap_security_initiate(g_conn);
        if (ret && ret != BLE_HS_EALREADY)
          {
            if (g_diagnostic_count++ < 8)
              syslog(LOG_WARNING, "[ble-hid] security-start result=%d\n", ret);
            g_ble_error = ret;
            g_mouse.error = -EACCES;
            g_mouse.enabled = false;
            g_mouse.disconnect_required = true;
            (void)disconnect_if_needed();
          }
        break;
      case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == g_conn)
          {
            hid_disconnect_record(event->disconnect.reason);
            if (g_diagnostic_count++ < 8)
              syslog(LOG_WARNING, "[ble-hid] disconnect reason=%d encrypted=%u\n",
                     event->disconnect.reason, (unsigned)g_mouse.encrypted);
            bool reconnect = g_user_enabled && !g_stop_pending;
            mouse_core_disconnect(&g_mouse);
            g_conn = BLE_HS_CONN_HANDLE_NONE;
            g_bond_pending = false;
            g_input_session = 0;
            g_advertising = false;
            if (reconnect) (void)advertise();
          }
        break;
      case BLE_GAP_EVENT_ENC_CHANGE:
        hid_encryption_changed(event->enc_change.conn_handle,
                               event->enc_change.status);
        break;
      case BLE_GAP_EVENT_CONN_UPDATE:
        glass_ble_hid_link_changed(&g_link,event->conn_update.conn_handle,
                                   event->conn_update.status);
        break;
      case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle != g_conn) break;
        glass_ble_channel_subscribe(g_conn,event->subscribe.attr_handle,event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == g_report_handle)
          g_mouse.report_notify = event->subscribe.cur_notify;
        else if (event->subscribe.attr_handle == g_boot_handle)
          g_mouse.boot_notify = event->subscribe.cur_notify;
        if (g_mouse.buttons && !(g_mouse.boot ? g_mouse.boot_notify :
                                               g_mouse.report_notify))
          {
            (void)mouse_core_stop(&g_mouse, transport_send, NULL);
            (void)disconnect_if_needed();
          }
        break;
      case BLE_GAP_EVENT_ADV_COMPLETE:
        g_advertising = false;
        if (!g_mouse.connected)
          {
            g_mouse.enabled = false;
            if (!glass_ble_store_has_hid_bond()) g_user_enabled = false;
          }
        break;
    }
  glass_ble_hid_link_state(&g_link,g_conn,g_mouse.epoch,g_mouse.connected,
                           g_mouse.encrypted,now_ms());
  glass_ble_channel_update(g_conn,g_mouse.epoch,g_mouse.connected&&g_mouse.enabled,g_mouse.encrypted);
  pthread_cond_broadcast(&g_done);
  if(!g_mouse.connected&&!g_advertising)glass_ble_role_release(GLASS_BLE_ROLE_HID);
  pthread_mutex_unlock(&g_lock);
  return 0;
}

static int advertise(void)
{
  static const ble_uuid16_t uuid = BLE_UUID16_INIT(HID_UUID);
  struct ble_hs_adv_fields fields = {0}, response = {0};
  struct ble_gap_adv_params params = {0};
  int ret;
  if (!g_ready) return -EAGAIN;
  if (g_stop_pending) return -EBUSY;
  if (g_mouse.connected) return g_mouse.enabled ? 0 : -EBUSY;
  if (ble_gap_adv_active()) return 0;
  ret = glass_ble_role_claim(GLASS_BLE_ROLE_HID);
  if(ret)return ret;
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids16 = &uuid;
  fields.num_uuids16 = 1;
  fields.uuids16_is_complete = 1;
  fields.appearance = 0x03c2; /* Mouse. */
  fields.appearance_is_present = 1;
  response.name = (const uint8_t *)g_name;
  response.name_len = sizeof(g_name) - 1;
  response.name_is_complete = 1;
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  ret = ble_gap_adv_set_fields(&fields);
  if (!ret) ret = ble_gap_adv_rsp_set_fields(&response);
  if (!ret) ret = ble_gap_adv_start(g_address, NULL,
                                   glass_ble_store_has_hid_bond() ?
                                   BLE_HS_FOREVER : PAIR_WINDOW_MS,
                                   &params, gap_event, NULL);
  g_ble_error = ret;
  g_mouse.enabled = g_advertising = ret == 0;
  if (!ret) g_user_enabled = true;
  g_mouse.error = ret ? -EIO : 0;
  if(ret)glass_ble_role_release(GLASS_BLE_ROLE_HID);
  return g_mouse.error;
}

static void process_request(void)
{
  int ret = -ECANCELED;
  if (!g_request.pending) return;
  if (g_request.pending && !g_request.cancelled &&
      g_request.stop_generation == g_stop_generation)
    {
      if(g_request.op!=OP_START&&now_ms()>=g_request_until){
        input_cancel_locked();
        g_request.result=-ETIMEDOUT;g_request.done=true;g_request.pending=false;
        pthread_cond_broadcast(&g_done);return;
      }
      if(g_request.op!=OP_START&&now_ms()<g_mouse.retry_at&&
         !g_mouse.release_pending)return;
      if (g_request.op == OP_START) ret = advertise();
      else if (g_request.epoch != g_mouse.epoch) ret = -ESTALE;
      else if (g_input_session && g_request.op == OP_MOVE) ret=-EBUSY;
      else if (g_request.op == OP_RELEASE)
        ret = mouse_core_release(&g_mouse, transport_send, NULL);
      else
        ret = mouse_core_move(&g_mouse, g_request.epoch, g_request.buttons,
                              g_request.x, g_request.y, g_request.wheel,
                              now_ms(), transport_send, NULL);
      (void)disconnect_if_needed();
    }
  if(mouse_core_transient(ret)&&g_request.op!=OP_START&&!g_input_session&&
     g_mouse.enabled&&!g_mouse.disconnect_required&&!g_request.cancelled){
    if(now_ms()<g_request_until)return;
    ret=-ETIMEDOUT;
    input_cancel_locked();
  }
  g_request.result = ret;
  g_request.done = true;
  g_request.pending = false;
  pthread_cond_broadcast(&g_done);
}

static void process_stop(void)
{
  int release_ret, adv_ret, disconnect_ret;
  if (!g_stop_pending) return;
  input_cancel_locked();
  g_user_enabled = false;
  release_ret = mouse_core_stop(&g_mouse, transport_send, NULL);
  adv_ret = stop_advertising();
  disconnect_ret = disconnect_if_needed();
  g_stop_result = release_ret ? release_ret : adv_ret ? adv_ret : disconnect_ret;
  g_stop_pending = false;
  if(!g_mouse.connected&&!g_advertising)glass_ble_role_release(GLASS_BLE_ROLE_HID);
  pthread_cond_broadcast(&g_done);
}

void glass_ble_hid_poll(void)
{
  pthread_mutex_lock(&g_lock);
  /* The command pthread and callout both belong to the persistent host task
   * group. g_lock serializes them; outside callers only publish requests. */
  process_request();
  process_stop();
  uint32_t cancelled=g_mouse.cancelled;
  (void)mouse_core_tick(&g_mouse, now_ms(), transport_send, NULL);
  if(g_mouse.cancelled!=cancelled)g_input_session=0;
  (void)disconnect_if_needed();
  if (!g_mouse.enabled && g_advertising) (void)stop_advertising();
  if (!g_mouse.connected && !g_advertising)
    glass_ble_role_release(GLASS_BLE_ROLE_HID);
  glass_ble_channel_update(g_conn,g_mouse.epoch,g_mouse.connected&&g_mouse.enabled,g_mouse.encrypted);
  bool business=!g_mouse.queued&&!g_mouse.release_pending&&!g_request.pending;
  business=business||now_ms()-g_business_poll_at>=20;
  if(business)g_business_poll_at=now_ms();
  pthread_mutex_unlock(&g_lock);
  if(business)glass_ble_channel_poll();
}
static void hid_persist_purpose(void)
{
  struct ble_gap_conn_desc desc;
  int ret;

  if (!g_bond_pending) return;
  g_bond_pending = false;
  if (!g_ready || !g_mouse.connected || !g_mouse.encrypted ||
      g_conn == BLE_HS_CONN_HANDLE_NONE) return;
  ret = ble_gap_conn_find(g_conn, &desc);
  if (!ret && desc.sec_state.encrypted && desc.sec_state.bonded &&
      desc.sec_state.key_size == 16)
    {
      ret = glass_ble_store_mark_hid_peer(desc.peer_id_addr.type,
                                         desc.peer_id_addr.val);
    }
  if (ret) g_mouse.error = ret < 0 ? ret : -EIO;
}

static void hid_link_poll(void)
{
  pthread_mutex_lock(&g_lock);
  struct glass_ble_hid_link copy=g_link;
  bool allowed=g_mouse.enabled&&!g_stop_pending;
  pthread_mutex_unlock(&g_lock);
  if(!allowed)return;
  /* Read the central's negotiated parameters outside the owner lock.
   * Stale results cannot update a reused link. No active update is issued. */
  glass_ble_hid_link_poll(&copy,now_ms());
  pthread_mutex_lock(&g_lock);
  if(g_link.connected&&g_link.connection==copy.connection&&g_link.epoch==copy.epoch){
    g_link.sampled=copy.sampled;
    g_link.result=copy.result;
    if(g_link.changed==copy.changed){g_link.interval=copy.interval;
      g_link.latency=copy.latency;g_link.timeout=copy.timeout;}
  }
  pthread_mutex_unlock(&g_lock);
}

static void watchdog(struct ble_npl_event *event)
{
  (void)event;
  /* This callback runs on the same queue as SMP. The command pthread also
   * calls hid_poll, so deferred persistence must stay outside hid_poll. */
  pthread_mutex_lock(&g_lock);
  hid_persist_purpose();
  pthread_mutex_unlock(&g_lock);
  glass_ble_hid_poll();
  hid_link_poll();
  /* Mailbox traffic must not postpone this SMP-after-callback deadline. */
  ble_npl_callout_reset(&g_watchdog, ble_npl_time_ms_to_ticks32(MOUSE_REPORT_MS));
}

int glass_ble_hid_service_init(void)
{
  int ret = ble_svc_gap_device_name_set(g_name);
  if (ret) return ret;
  ret = ble_svc_gap_device_appearance_set(0x03c2);
  if (ret) return ret;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_mitm = 0;
  ret = ble_gatts_count_cfg(g_services);
  if (!ret) ret = ble_gatts_add_svcs(g_services);
  if (!ret) ret = glass_ble_channel_init();
  if (ret) return ret;
  ble_npl_callout_init(&g_watchdog, nimble_port_get_dflt_eventq(), watchdog, NULL);
  pthread_mutex_lock(&g_lock);
  g_initialized = true;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void glass_ble_hid_synced(uint8_t address)
{
  pthread_mutex_lock(&g_lock);
  g_address = address;
  g_ready = true;
  if (glass_ble_store_has_hid_bond())
    {
      g_user_enabled = true;
      (void)advertise();
    }
  pthread_mutex_unlock(&g_lock);
  ble_npl_callout_reset(&g_watchdog, ble_npl_time_ms_to_ticks32(MOUSE_REPORT_MS));
}

void glass_ble_hid_reset(int reason)
{
  pthread_mutex_lock(&g_lock);
  mouse_core_disconnect(&g_mouse);
  g_mouse.error = -EIO;
  g_ble_error = reason;
  g_ready = g_advertising = false;
  g_user_enabled = false;
  g_conn = BLE_HS_CONN_HANDLE_NONE;
  g_bond_pending = false;
  g_input_session = 0;
  glass_ble_hid_link_state(&g_link,g_conn,g_mouse.epoch,false,false,now_ms());
  glass_ble_channel_update(g_conn,g_mouse.epoch,false,false);
  /* Controller reset invalidates this owner's link and advertisement. */
  glass_ble_role_release(GLASS_BLE_ROLE_HID);
  ++g_stop_generation;
  g_request.cancelled = true;
  pthread_cond_broadcast(&g_done);
  pthread_mutex_unlock(&g_lock);
}

void glass_ble_hid_read(struct glass_ble_hid_state *state)
{
  if (!state) return;
  pthread_mutex_lock(&g_lock);
  memset(state, 0, sizeof(*state));
  state->enabled = g_mouse.enabled;
  state->advertising = g_advertising;
  state->connected = g_mouse.connected;
  state->encrypted = g_mouse.encrypted;
  state->mouse_notify = g_mouse.boot ? g_mouse.boot_notify : g_mouse.report_notify;
  state->suspended = g_mouse.suspended;
  state->boot_mode = g_mouse.boot;
  state->buttons = g_mouse.buttons;
  state->epoch = g_mouse.epoch;
  state->error = g_mouse.error;
  state->ble_error = g_ble_error;
  state->host_ready = g_ready;
  pthread_mutex_unlock(&g_lock);
  state->store_error = glass_ble_store_last_error();
}

/* Caller holds g_api across a whole click. No stack pointers are queued.
 * Timeout cancels a queued command rather than allowing delayed input. */
static int rpc(enum operation op, int buttons, int x, int y, int wheel,
               uint32_t epoch, uint32_t generation)
{
  struct timespec deadline;
  int ret = 0;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += RPC_TIMEOUT_SEC;
  pthread_mutex_lock(&g_lock);
  if (!g_ready) ret = -EAGAIN;
  else if (g_request.pending || g_stop_pending) ret = -EBUSY;
  else if (generation != g_stop_generation) ret = -ECANCELED;
  if (!ret)
    {
      g_request = (struct request){.pending = true, .op = op,
        .buttons = buttons, .x = x, .y = y, .wheel = wheel,
        .epoch = epoch, .stop_generation = generation};
      g_request_until=now_ms()+(op==OP_RELEASE?MOUSE_RELEASE_MS:MOUSE_INPUT_MS);
      /* A one-shot NPL callout may be delayed/lost in this port. Wake the
       * existing host command pthread using its descriptor-free mailbox. */
      glass_ble_central_hid_wake();
      while (!g_request.done && !ret)
        ret = pthread_cond_timedwait(&g_done, &g_lock, &deadline);
      if (g_request.done) ret = g_request.result;
      else
        {
          /* Do not leave a timed-out request blocking the next touch event.
           * The host callout will observe pending=false and discard it. */
          g_request.cancelled = true;
          g_request.pending = false;
          g_request.done = true;
          g_request.result = -ETIMEDOUT;
          pthread_cond_broadcast(&g_done);
          ret = -ret;
          /* If a prior click press was sent, the watchdog still releases it. */
        }
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

static void snapshot(uint32_t *epoch, uint32_t *generation)
{
  pthread_mutex_lock(&g_lock);
  *epoch = g_mouse.epoch;
  *generation = g_stop_generation;
  pthread_mutex_unlock(&g_lock);
}

int glass_ble_hid_start(void)
{
  uint32_t epoch, generation;
  int ret;
  if (pthread_mutex_trylock(&g_api)) return -EBUSY;
  snapshot(&epoch, &generation);
  ret = glass_ble_host_wait();
  if (!ret) ret = glass_ble_central_mouse_pause();
  if (!ret) ret = rpc(OP_START, 0, 0, 0, 0, epoch, generation);
  if (ret) (void)glass_ble_central_mouse_resume();
  pthread_mutex_unlock(&g_api);
  return ret;
}

int glass_ble_hid_stop(void)
{
  struct timespec deadline;
  int ret = 0;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += RPC_TIMEOUT_SEC;
  pthread_mutex_lock(&g_lock);
  ++g_stop_generation;
  g_mouse.enabled = false; /* Immediate gate, even when a click owns g_api. */
  if (g_initialized)
    {
      if (!g_stop_pending)
        {
          g_stop_pending = true;
          glass_ble_central_hid_wake();
        }
      while ((g_stop_pending || g_mouse.connected || g_advertising) && !ret)
        ret = pthread_cond_timedwait(&g_done, &g_lock, &deadline);
      ret = ret ? -ret : g_stop_result;
    }
  bool idle = !g_stop_pending && !g_mouse.connected && !g_advertising;
  pthread_mutex_unlock(&g_lock);
  if (idle)
    {
      int restore = glass_ble_central_mouse_resume();
      if (!ret) ret = restore;
    }
  return ret;
}

int glass_ble_hid_mouse(int buttons, int x, int y, int wheel)
{
  uint32_t epoch, generation;
  int ret;
  if (buttons < 0 || buttons > 7 || x < -127 || x > 127 ||
      y < -127 || y > 127 || wheel < -127 || wheel > 127) return -EINVAL;
  if (pthread_mutex_trylock(&g_api)) return -EBUSY;
  snapshot(&epoch, &generation);
  ret = rpc(OP_MOVE, buttons, x, y, wheel, epoch, generation);
  pthread_mutex_unlock(&g_api);
  return ret;
}

int glass_ble_hid_release(void)
{
  uint32_t epoch, generation;
  int ret;
  if (pthread_mutex_trylock(&g_api)) return -EBUSY;
  pthread_mutex_lock(&g_lock);input_cancel_locked();pthread_mutex_unlock(&g_lock);
  snapshot(&epoch, &generation);
  ret = rpc(OP_RELEASE, 0, 0, 0, 0, epoch, generation);
  pthread_mutex_unlock(&g_api);
  return ret;
}

int glass_ble_hid_click(unsigned button)
{
  uint32_t epoch, generation;
  int ret;
  bool held;
  if (button != 1 && button != 2 && button != 4) return -EINVAL;
  if (pthread_mutex_trylock(&g_api)) return -EBUSY;
  snapshot(&epoch, &generation);
  pthread_mutex_lock(&g_lock);
  held = g_mouse.buttons != 0;
  pthread_mutex_unlock(&g_lock);
  ret = held ? -EBUSY : rpc(OP_MOVE, (int)button, 0, 0, 0, epoch, generation);
  if (!ret)
    {
      usleep(20000);
      ret = rpc(OP_RELEASE, 0, 0, 0, 0, epoch, generation);
    }
  pthread_mutex_unlock(&g_api);
  /* If release was cancelled/timed out, stop has either already inhibited
   * input or will inhibit it now. Never replay release on a new epoch. */
  if(ret&&!held){pthread_mutex_lock(&g_lock);if(!g_input_session)input_cancel_locked();pthread_mutex_unlock(&g_lock);}
  return ret;
}

int glass_ble_hid_input_begin(uint32_t epoch,uint32_t *session)
{
  if(!session||!epoch)return -EINVAL;
  pthread_mutex_lock(&g_lock);int ret=0;
  if(g_input_session||g_request.pending||g_mouse.buttons||g_mouse.queued)ret=-EBUSY;
  else if(epoch!=g_mouse.epoch)ret=-ESTALE;
  else if(!g_mouse.connected)ret=-ENOTCONN;
  else if(!g_mouse.enabled||!g_mouse.encrypted||g_stop_pending)ret=-EACCES;
  else if(g_mouse.suspended||g_mouse.release_pending||
          !(g_mouse.boot?g_mouse.boot_notify:g_mouse.report_notify))ret=-EAGAIN;
  else if(g_input_counter==UINT32_MAX)ret=-EOVERFLOW;
  else{g_input_session=++g_input_counter;g_input_epoch=epoch;*session=g_input_session;}
  pthread_mutex_unlock(&g_lock);return ret;
}
static int input_owned_locked(uint32_t session)
{
  if(!session||session!=g_input_session||g_input_epoch!=g_mouse.epoch)return -ESTALE;
  return 0;
}
int glass_ble_hid_input_report(uint32_t session,int buttons,int x,int y,int wheel)
{
  pthread_mutex_lock(&g_lock);int ret=input_owned_locked(session);
  if(!ret)ret=mouse_core_enqueue(&g_mouse,g_input_epoch,buttons,x,y,wheel,now_ms());
  if(!ret)glass_ble_central_hid_wake();
  pthread_mutex_unlock(&g_lock);return ret;
}
int glass_ble_hid_input_click(uint32_t session,unsigned button)
{
  pthread_mutex_lock(&g_lock);int ret=input_owned_locked(session);
  if(!ret)ret=mouse_core_enqueue_click(&g_mouse,g_input_epoch,button,now_ms());
  if(!ret)glass_ble_central_hid_wake();
  pthread_mutex_unlock(&g_lock);return ret;
}
int glass_ble_hid_input_keepalive(uint32_t session)
{
  pthread_mutex_lock(&g_lock);int ret=input_owned_locked(session);
  if(!ret)ret=mouse_core_keepalive(&g_mouse,g_input_epoch,now_ms());
  pthread_mutex_unlock(&g_lock);return ret;
}
int glass_ble_hid_input_end(uint32_t session)
{
  pthread_mutex_lock(&g_lock);int ret=input_owned_locked(session);
  if(!ret){input_cancel_locked();glass_ble_central_hid_wake();}
  pthread_mutex_unlock(&g_lock);return ret;
}
void glass_ble_hid_input_read(struct glass_ble_hid_input_state *s)
{
  if(!s)return;
  pthread_mutex_lock(&g_lock);
  *s=(struct glass_ble_hid_input_state){.session=g_input_session,.epoch=g_mouse.epoch,
    .accepted=g_mouse.accepted,.submitted=g_mouse.submitted,.merged=g_mouse.merged,
    .retries=g_mouse.retries,.discarded=g_mouse.discarded,.queued=g_mouse.queued,
    .releasing=g_mouse.release_pending,.interval=g_link.interval,.latency=g_link.latency,
    .supervision_timeout=g_link.timeout,.parameter_result=g_link.result,
    .parameter_status=g_link.status,.parameter_attempts=g_link.attempts,
    .parameter_updates=g_link.changed,.disconnects=g_disconnects,
    .unexpected_disconnects=g_unexpected_disconnects,.last_disconnect_epoch=g_last_disconnect_epoch,
    .last_disconnect_reason=g_last_disconnect_reason,.last_parameter_attempts=g_last_parameter_attempts,
    .last_parameter_updates=g_last_parameter_updates,.last_interval=g_last_interval,
    .last_secure_age_ms=g_last_secure_age_ms};
  pthread_mutex_unlock(&g_lock);
}
