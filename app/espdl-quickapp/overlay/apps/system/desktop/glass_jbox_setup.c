/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_jbox_setup.h"
#include "c6_wifi_trial.h"
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "glass_network_recovery.h"
#include "glass_portal.h"
#include "jbox_wifi.h"
#include "jbox_wifi_psa.h"
#include "qpk_jbox.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static const uint8_t service_uuid[16] = {0x07, 0xed, 0x9b, 0x2d, 0x0f, 0x06,
                                         0x7c, 0x87, 0x9b, 0x43, 0x43, 0x6b,
                                         0x4d, 0x24, 0x75, 0x17};
static const char *service_name = "1775244d-6b43-439b-877c-060f2d9bed07";
struct candidate {
  struct glass_jbox_setup_device view;
  struct glass_ble_device peer;
  unsigned index, scan;
};
struct setup_service {
  struct glass_jbox_setup_snapshot view;
  struct candidate candidates[GLASS_JBOX_SETUP_LIMIT], selected;
  struct qpk_hw_progress progress;
  bool pending, scan;
  unsigned orphan;
};
struct setup_context {
  struct setup_service *service;
  struct glass_ble_state ble;
  struct glass_gatt_request request;
  struct glass_gatt_result result;
  struct jbox_wifi_crypto crypto;
  uint64_t deadline, wifi_generation;
  struct c6_desktop_result network;
  uint32_t generation;
  bool closing;
  int close_error;
  unsigned scan_generation;
  bool scan_closing;
  int scan_close_error;
  uint16_t session, config;
};
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static struct setup_service *setup;
static void wipe(void *p, size_t n) {
  volatile uint8_t *v = p;
  while (n--)
    *v++ = 0;
}
static uint64_t now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static int check(void *opaque) {
  struct setup_context *c = opaque;
  if (atomic_load(&c->service->progress.cancel))
    return -ECANCELED;
  if (c->wifi_generation) {
    int r = glass_wifi_service_read(&c->network);
    if (r)
      return r;
    if (c->network.busy || !c->network.link.associated ||
        !c->network.link.ipv4_ready ||
        c->network.link.generation != c->wifi_generation ||
        c6_wifi_trial_pending())
      return -ESTALE;
  }
  return now() >= c->deadline ? -ETIMEDOUT : 0;
}
static int pause_ms(void *opaque, unsigned ms) {
  for (unsigned elapsed = 0; elapsed < ms; elapsed += 20) {
    int r = check(opaque);
    if (r)
      return r;
    usleep(20000);
  }
  return check(opaque);
}
static void stage(struct setup_service *s, enum glass_jbox_setup_phase phase) {
  pthread_mutex_lock(&lock);
  s->view.phase = phase;
  pthread_mutex_unlock(&lock);
}
static bool present(struct setup_context *c, unsigned generation) {
  glass_ble_read(&c->ble);
  for (unsigned i = 0; i < c->ble.session_count && i < GLASS_BLE_CENTRAL_LIMIT;
       i++) {
    struct glass_ble_session *s = &c->ble.sessions[i];
    if (s->generation == generation &&
        (s->connected || s->connecting || s->disconnecting))
      return true;
  }
  return false;
}
static int ble_ready(struct setup_context *c) {
  int r = check(c);
  if (r)
    return r;
  r = glass_ble_start();
  if (r)
    return r;
  for (;;) {
    glass_ble_read(&c->ble);
    if (c->ble.ready)
      return 0;
    if (c->ble.error)
      return c->ble.error;
    r = pause_ms(c, 50);
    if (r)
      return r;
  }
}
static bool peer_equal(const struct glass_ble_device *a,
                       const struct glass_ble_device *b) {
  return a->type == b->type && !memcmp(a->address, b->address, 6);
}
static bool jbox_name(const char *s) {
  return strlen(s) == 11 && !strncmp(s, "JBOX-", 5) &&
         strspn(s + 5, "0123456789abcdefABCDEF") == 6;
}
static const char *json_text(cJSON *o, const char *key) {
  cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
static int close_scan(struct setup_context *c) {
  if (!c->scan_generation)
    return 0;
  glass_ble_read(&c->ble);
  if (!c->ble.scanning || c->ble.scan_generation != c->scan_generation)
    return 0;
  if (c->scan_closing)
    return c->scan_close_error ? c->scan_close_error : -EBUSY;
  c->scan_closing = true;
  int r = glass_ble_central_scan_stop(c->scan_generation);
  if (r && r != -ESTALE) {
    c->scan_close_error = r;
    return r;
  }
  uint64_t end = now() + 5000;
  do {
    glass_ble_read(&c->ble);
    if (!c->ble.scanning || c->ble.scan_generation != c->scan_generation)
      return 0;
    if (now() >= end) {
      c->scan_close_error = -ETIMEDOUT;
      return -ETIMEDOUT;
    }
    usleep(50000);
  } while (true);
}
static int scan_devices(struct setup_context *c) {
  struct setup_service *s = c->service;
  struct candidate *rows = calloc(GLASS_JBOX_SETUP_LIMIT, sizeof(*rows));
  if (!rows)
    return -ENOMEM;
  cJSON *out = cJSON_CreateObject();
  unsigned count = 0;
  uint32_t scan = 0;
  int r = out ? jbox_discover(out, 800, &s->progress) : -ENOMEM;
  if (!r) {
    cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(out, "devices")) {
      const char *id = json_text(v, "deviceId"), *host = json_text(v, "host");
      if (count == GLASS_JBOX_SETUP_LIMIT || strlen(id) != 12 ||
          strlen(host) > 15)
        continue;
      struct candidate *a = &rows[count++];
      strcpy(a->view.device_id, id);
      strcpy(a->view.host, host);
      snprintf(a->view.name, sizeof(a->view.name), "JBOX-%.6s", id + 6);
      a->view.pairing_open =
          cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "pairingOpen"));
    }
  }
  cJSON_Delete(out);
  /* A LAN-only target remains usable if BLE cannot start. Never stop another
   * page's scan or disconnect another central/HID owner. */
  if (check(c)) {
    r = check(c);
    goto done;
  }
  r = ble_ready(c);
  if (!r && c->ble.scanning)
    r = -EBUSY;
  if (!r)
    r = glass_ble_central_scan_session(service_uuid, &scan);
  if (!r) {
    c->scan_generation = scan;
    r = pause_ms(c, 2500);
    int stop = close_scan(c);
    if (stop)
      r = stop;
  }
  if (!r) {
    glass_ble_read(&c->ble);
    if (c->ble.scan_generation != scan) {
      r = -ESTALE;
      goto done;
    }
    for (unsigned i = 0; i < c->ble.count && i < GLASS_BLE_LIMIT &&
                         count < GLASS_JBOX_SETUP_LIMIT;
         i++) {
      struct glass_ble_device *p = &c->ble.devices[i];
      if (!jbox_name(p->name))
        continue;
      struct candidate *a = &rows[count++];
      a->view.bluetooth = true;
      a->peer = *p;
      a->index = i;
      a->scan = scan;
      snprintf(a->view.name, sizeof(a->view.name), "%s", p->name);
    }
  }
  if (count && r != -ECANCELED && r != -ETIMEDOUT && r != -ESTALE)
    r = 0;
done:
  if (!r) {
    pthread_mutex_lock(&lock);
    s->view.count = count;
    memcpy(s->candidates, rows, GLASS_JBOX_SETUP_LIMIT * sizeof(*rows));
    for (unsigned i = 0; i < count; i++)
      s->view.devices[i] = rows[i].view;
    pthread_mutex_unlock(&lock);
  }
  free(rows);
  return r;
}
static int gatt(struct setup_context *c, enum glass_gatt_operation operation) {
  int r = check(c);
  if (r)
    return r;
  c->request.operation = operation;
  c->request.generation = c->generation;
  r = glass_ble_gatt(&c->request, &c->result, &c->service->progress.cancel);
  if (!r && (c->result.generation != c->generation || c->result.truncated ||
             c->result.count > GLASS_GATT_ITEMS ||
             c->result.length > GLASS_GATT_BYTES))
    r = -ESTALE;
  return r ? r : check(c);
}
static int connect_ble(struct setup_context *c, const struct candidate *a) {
  int r = ble_ready(c);
  if (r)
    return r;
  glass_ble_read(&c->ble);
  if (c->ble.scanning || c->ble.scan_generation != a->scan ||
      a->index >= c->ble.count ||
      !peer_equal(&a->peer, &c->ble.devices[a->index]))
    return -ESTALE;
  r = check(c);
  if (r)
    return r;
  r = glass_ble_central_connect_session(a->index, a->scan, &c->generation);
  if (r)
    return r;
  for (;;) {
    bool connected = false;
    glass_ble_read(&c->ble);
    for (unsigned i = 0;
         i < c->ble.session_count && i < GLASS_BLE_CENTRAL_LIMIT; i++) {
      struct glass_ble_session *v = &c->ble.sessions[i];
      if (v->generation != c->generation)
        continue;
      if (!peer_equal(&v->peer, &a->peer))
        return -ESTALE;
      if (v->error)
        return v->error;
      connected = v->connected;
    }
    if (connected)
      break;
    if (!present(c, c->generation))
      return -ENOTCONN;
    r = pause_ms(c, 50);
    if (r)
      return r;
  }
  memset(&c->request, 0, sizeof(c->request));
  r = gatt(c, GLASS_GATT_MTU);
  if (r)
    return r;
  if (c->result.mtu < 128)
    return -EMSGSIZE;
  r = gatt(c, GLASS_GATT_SERVICES);
  if (r)
    return r;
  unsigned matches = 0;
  for (unsigned i = 0; i < c->result.count; i++)
    if (!strcasecmp(c->result.items[i].uuid, service_name)) {
      c->request.start = c->result.items[i].start;
      c->request.end = c->result.items[i].end;
      matches++;
    }
  if (matches != 1)
    return -EPROTO;
  r = gatt(c, GLASS_GATT_CHARACTERISTICS);
  if (r)
    return r;
  for (unsigned i = 0; i < c->result.count; i++) {
    struct glass_gatt_item *v = &c->result.items[i];
    if (!strcasecmp(v->uuid, "1775ff51-6b43-439b-877c-060f2d9bed07")) {
      if (c->session || !(v->properties & 2) || !(v->properties & 8))
        return -EPROTO;
      c->session = v->value;
    }
    if (!strcasecmp(v->uuid, "1775ff52-6b43-439b-877c-060f2d9bed07")) {
      if (c->config || !(v->properties & 2) || !(v->properties & 8))
        return -EPROTO;
      c->config = v->value;
    }
  }
  return c->session && c->config ? 0 : -EPROTO;
}
static int exchange(void *opaque, int session, const uint8_t *data, size_t n,
                    uint8_t *reply, size_t *length) {
  struct setup_context *c = opaque;
  if (n > 192)
    return -EMSGSIZE;
  memset(&c->request, 0, sizeof(c->request));
  c->request.handle = session ? c->session : c->config;
  c->request.response = true;
  c->request.length = n;
  memcpy(c->request.data, data, n);
  int r = gatt(c, GLASS_GATT_WRITE);
  wipe(c->request.data, sizeof(c->request.data));
  if (r)
    return r;
  r = gatt(c, GLASS_GATT_READ);
  if (r)
    return r;
  if (c->result.length > *length)
    return -EMSGSIZE;
  memcpy(reply, c->result.data, c->result.length);
  *length = c->result.length;
  wipe(c->result.data, sizeof(c->result.data));
  return 0;
}
static int keypair(void *p, uint8_t key[32]) {
  return jbox_wifi_crypto_keypair(&((struct setup_context *)p)->crypto, key);
}
static int cipher_start(void *p, const uint8_t key[32], const uint8_t iv[16]) {
  return jbox_wifi_crypto_start(&((struct setup_context *)p)->crypto, key, iv);
}
static int setup_crypt(void *p, const uint8_t *in, size_t n, uint8_t *out) {
  return jbox_wifi_crypto_update(&((struct setup_context *)p)->crypto, in, n,
                                 out);
}
static int close_ble(struct setup_context *c) {
  if (!c->generation || !present(c, c->generation))
    return 0;
  if (c->closing)
    return c->close_error ? c->close_error : -EBUSY;
  c->closing = true;
  int r = glass_ble_central_disconnect_generation(c->generation);
  if (r && r != -ENOTCONN && r != -ESTALE) {
    c->close_error = r;
    return r;
  }
  uint64_t end = now() + 5000;
  while (present(c, c->generation)) {
    if (now() >= end) {
      c->close_error = -ETIMEDOUT;
      return -ETIMEDOUT;
    }
    usleep(50000);
  }
  return 0;
}
static int resolve(struct setup_context *c,
                   struct glass_jbox_setup_device *target, bool require_open) {
  /* Poll only read-only discovery. Pair/configure/apply are never replayed. */
  for (unsigned attempt = 0; attempt < 60; attempt++) {
    int r = check(c);
    if (r)
      return r;
    cJSON *out = cJSON_CreateObject();
    if (!out)
      return -ENOMEM;
    r = jbox_discover_host(out, target->host, 500, &c->service->progress);
    cJSON *items = cJSON_GetObjectItemCaseSensitive(out, "devices");
    if (!r && cJSON_GetArraySize(items) == 1) {
      cJSON *v = cJSON_GetArrayItem(items, 0);
      const char *id = json_text(v, "deviceId");
      if (strlen(id) != 12 || strncasecmp(id + 6, target->name + 5, 6))
        r = -ESTALE;
      else if (target->device_id[0] && strcasecmp(id, target->device_id))
        r = -ESTALE;
      else {
        strcpy(target->device_id, id);
        target->pairing_open =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v, "pairingOpen"));
        if (!require_open || target->pairing_open) {
          cJSON_Delete(out);
          return 0;
        }
      }
    }
    cJSON_Delete(out);
    if (r)
      return r;
    r = pause_ms(c, 500);
    if (r)
      return r;
  }
  return -ETIMEDOUT;
}
static int commission(struct setup_context *c, struct candidate *a, char *ssid,
                      char *password) {
  struct setup_service *s = c->service;
  struct glass_jbox_setup_device target = a->view;
  int r;
  if (target.bluetooth) {
    r = glass_wifi_service_read(&c->network);
    if (r)
      return r;
    if (c->network.busy || !c->network.link.associated ||
        !c->network.link.ipv4_ready || !c->network.link.generation)
      return -ENETDOWN;
    c->wifi_generation = c->network.link.generation;
    r = c6_wifi_current_credentials(ssid, password);
    if (!r)
      r = check(c);
    if (r)
      return r;
    stage(s, JBOX_SETUP_BLE);
    r = connect_ble(c, a);
    if (r)
      return r;
    stage(s, JBOX_SETUP_WIFI);
    struct jbox_wifi_ops ops = {c,     check,    keypair, cipher_start,
                                setup_crypt, exchange, pause_ms};
    r = jbox_wifi_provision(&ops, ssid, password, target.host);
    wipe(ssid, 33);
    wipe(password, 65);
    jbox_wifi_crypto_free(&c->crypto);
    jbox_wifi_crypto_init(&c->crypto);
    if (r)
      return r;
    r = close_ble(c);
    if (r)
      return r;
    c->generation = 0;
    stage(s, JBOX_SETUP_FIND);
    r = resolve(c, &target, false);
    if (r)
      return r;
  }
  r = check(c);
  if (r)
    return r;
  struct portal_jbox_config saved = {0};
  bool bound = portal_jbox_load(&saved) == 0 && target.device_id[0] &&
               !strcasecmp(saved.device_id, target.device_id);
  wipe(&saved, sizeof(saved));
  if (!bound) {
    stage(s, JBOX_SETUP_FIND);
    r = resolve(c, &target, true);
    if (r)
      return r;
    stage(s, JBOX_SETUP_BIND);
    cJSON *args = cJSON_CreateObject(), *out = cJSON_CreateObject();
    void *unused = NULL;
    if (!args || !out) {
      cJSON_Delete(args);
      cJSON_Delete(out);
      return -ENOMEM;
    }
    cJSON_AddStringToObject(args, "host", target.host);
    cJSON_AddStringToObject(args, "deviceId", target.device_id);
    r = qpk_jbox_execute(&unused, "pair", args, out, &s->progress);
    qpk_jbox_destroy(unused);
    cJSON_Delete(args);
    cJSON_Delete(out);
    if (r)
      return r;
    pthread_mutex_lock(&lock);
    s->view.binding_saved = true;
    pthread_mutex_unlock(&lock);
  }
  r = check(c);
  if (r)
    return r;
  stage(s, JBOX_SETUP_VERIFY);
  cJSON *args = cJSON_CreateObject(), *out = cJSON_CreateObject();
  void *controller = NULL;
  if (!args || !out) {
    cJSON_Delete(args);
    cJSON_Delete(out);
    return -ENOMEM;
  }
  cJSON_AddStringToObject(args, "host", target.host);
  cJSON_AddStringToObject(args, "deviceId", target.device_id);
  r = qpk_jbox_execute(&controller, "connect", args, out, &s->progress);
  if (!r) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(out, "generation");
    if (!cJSON_IsNumber(v))
      r = -EPROTO;
    else {
      unsigned generation = v->valueint;
      cJSON_Delete(args);
      cJSON_Delete(out);
      args = cJSON_CreateObject();
      out = cJSON_CreateObject();
      if (!args || !out)
        r = -ENOMEM;
      else {
        cJSON_AddNumberToObject(args, "generation", generation);
        r = qpk_jbox_execute(&controller, "status", args, out, &s->progress);
      }
    }
  }
  /* Release the verification-only session so the normal assistant/application
   * can claim the device. READY means authenticated/read back, not a held TCP.
   */
  qpk_jbox_destroy(controller);
  cJSON_Delete(args);
  cJSON_Delete(out);
  if (!r && bound && !check(c)) {
    /* Only authenticated read-back may update a saved address. */
    struct portal_jbox_config config = {0};
    r = portal_jbox_load(&config);
    if (!r && strcasecmp(config.device_id, target.device_id))
      r = -ESTALE;
    if (!r && strcmp(config.host, target.host)) {
      strcpy(config.host, target.host);
      r = portal_jbox_save(&config);
      if (!r) {
        pthread_mutex_lock(&lock);
        s->view.binding_saved = true;
        pthread_mutex_unlock(&lock);
      }
    }
    wipe(&config, sizeof(config));
  }
  return r ? r : check(c);
}
static void *worker(void *opaque) {
  struct setup_service *s = opaque;
  for (;;) {
    pthread_mutex_lock(&lock);
    while (!s->pending)
      pthread_cond_wait(&wake, &lock);
    bool scan = s->scan;
    s->pending = false;
    struct candidate selected = s->selected;
    char ssid[33] = {0}, password[65] = {0};
    pthread_mutex_unlock(&lock);
    struct setup_context *c = calloc(1, sizeof(*c));
    int r = -ENOMEM, cleanup = 0;
    if (c) {
      c->service = s;
      c->deadline = now() + (scan ? 20000 : 110000);
      jbox_wifi_crypto_init(&c->crypto);
      r = check(c);
      if (!r)
        r = scan ? scan_devices(c) : commission(c, &selected, ssid, password);
      cleanup = close_ble(c);
      int scan_cleanup = close_scan(c);
      if (!cleanup)
        cleanup = scan_cleanup;
      jbox_wifi_crypto_free(&c->crypto);
    }
    wipe(ssid, sizeof(ssid));
    wipe(password, sizeof(password));
    pthread_mutex_lock(&lock);
    s->view.cleanup_error = cleanup;
    if (cleanup && c)
      s->orphan = c->generation;
    if (cleanup)
      r = cleanup;
    else if (atomic_load(&s->progress.cancel))
      r = -ECANCELED;
    s->view.error = r;
    s->view.phase = r ? (r == -ECANCELED ? JBOX_SETUP_CANCEL : JBOX_SETUP_ERROR)
                      : (scan ? JBOX_SETUP_SELECT : JBOX_SETUP_READY);
    s->view.busy = false;
    s->view.cancelling = false;
    pthread_mutex_unlock(&lock);
    if (c) {
      wipe(c, sizeof(*c));
      free(c);
    }
  }
  return NULL;
}
int glass_jbox_setup_init(void) {
  pthread_mutex_lock(&lock);
  if (setup) {
    pthread_mutex_unlock(&lock);
    return 0;
  }
  struct setup_service *s = calloc(1, sizeof(*s));
  if (!s) {
    pthread_mutex_unlock(&lock);
    return -ENOMEM;
  }
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  int r = pthread_attr_setstacksize(&attr, 16384);
  pthread_t thread;
  if (!r)
    r = pthread_create(&thread, &attr, worker, s);
  pthread_attr_destroy(&attr);
  if (r) {
    free(s);
    pthread_mutex_unlock(&lock);
    return -r;
  }
  pthread_detach(thread);
  setup = s;
  pthread_mutex_unlock(&lock);
  return 0;
}
int glass_jbox_setup_read(struct glass_jbox_setup_snapshot *out) {
  if (!out)
    return -EINVAL;
  pthread_mutex_lock(&lock);
  if (setup)
    *out = setup->view;
  else
    memset(out, 0, sizeof(*out));
  bool ready = setup != NULL;
  pthread_mutex_unlock(&lock);
  return ready ? 0 : -EAGAIN;
}
static int submit(bool scan, unsigned generation, unsigned index) {
  pthread_mutex_lock(&lock);
  struct setup_service *s = setup;
  int r = 0;
  if (!s) {
    r = -EAGAIN;
    goto done;
  }
  if (s->view.busy) {
    r = -EBUSY;
    goto done;
  }
  if (s->orphan) {
    struct setup_context *c = calloc(1, sizeof(*c));
    if (!c) {
      r = -ENOMEM;
      goto done;
    }
    bool active = present(c, s->orphan);
    free(c);
    if (active) {
      r = -EBUSY;
      goto done;
    }
    s->orphan = 0;
  }
  if (s->view.generation == UINT_MAX) {
    r = -EOVERFLOW;
    goto done;
  }
  if (!scan) {
    if (generation != s->view.generation || index >= s->view.count) {
      r = -ESTALE;
      goto done;
    }
    s->selected = s->candidates[index];
  }
  s->view.generation++;
  s->view.binding_saved = false;
  s->view.error = 0;
  s->view.cleanup_error = 0;
  s->view.busy = true;
  s->view.cancelling = false;
  s->view.phase = scan ? JBOX_SETUP_SCAN : JBOX_SETUP_BLE;
  if (scan)
    s->view.count = 0;
  atomic_store(&s->progress.cancel, false);
  s->scan = scan;
  s->pending = true;
  pthread_cond_signal(&wake);
done:
  pthread_mutex_unlock(&lock);
  return r;
}
int glass_jbox_setup_scan(void) { return submit(true, 0, 0); }
int glass_jbox_setup_start(unsigned generation, unsigned index) {
  return submit(false, generation, index);
}
void glass_jbox_setup_cancel(void) {
  pthread_mutex_lock(&lock);
  if (setup && setup->view.busy) {
    setup->view.cancelling = true;
    atomic_store(&setup->progress.cancel, true);
  }
  pthread_mutex_unlock(&lock);
}
