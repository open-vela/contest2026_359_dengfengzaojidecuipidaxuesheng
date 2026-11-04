/* SPDX-License-Identifier: Apache-2.0 */
/* Production persistent worker, isolated radio/network/storage substitutes. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "glass_ble.h"
#include "glass_ble_central.h"
#include "glass_jbox_setup.h"
#include "glass_network_recovery.h"
#include "glass_portal.h"
#include "jbox_wifi.h"
#include "jbox_wifi_psa.h"
#include "qpk_jbox.h"
static atomic_ulong milliseconds;
static atomic_int blocked, release_core, entered_close, release_close;
static int fault, paired, connected, readbacks, closed, ble_connects,
    ble_closes, provisions, saves;
static struct glass_ble_state radio;
static struct portal_jbox_config binding;
static bool bound;
static void real_sleep(void) {
#ifdef _WIN32
  Sleep(1);
#else
  struct timespec t = {0, 1000000};
  nanosleep(&t, NULL);
#endif
}
static int setup_test_clock_gettime(clockid_t id, struct timespec *out) {
  (void)id;
  unsigned long n = atomic_load(&milliseconds);
  out->tv_sec = n / 1000;
  out->tv_nsec = (n % 1000) * 1000000;
  return 0;
}
static int setup_test_sleep(unsigned usec) {
  atomic_fetch_add(&milliseconds, usec / 1000);
  return 0;
}
#define clock_gettime setup_test_clock_gettime
#define usleep setup_test_sleep
#include "glass_jbox_setup.c"
#undef clock_gettime
#undef usleep

int glass_ble_start(void) {
  radio.ready = true;
  return 0;
}
void glass_ble_read(struct glass_ble_state *out) { *out = radio; }
int glass_ble_central_scan_session(const uint8_t uuid[16], uint32_t *generation) {
  assert(!memcmp(uuid, service_uuid, 16));
  radio.scanning = true;
  radio.scan_generation = 77;
  *generation = 77;
  return 0;
}
int glass_ble_central_scan_stop(uint32_t generation) {
  assert(generation == 77);
  radio.scanning = false;
  return 0;
}
int glass_ble_central_connect_session(unsigned index, uint32_t scan,
                                      uint32_t *generation) {
  assert(index == 0 && scan == 77);
  assert(!atomic_load(&setup->progress.cancel));
  ble_connects++;
  *generation = 901;
  radio.sessions[1] = (struct glass_ble_session){
      .generation = 901, .connected = true, .peer = radio.devices[0]};
  radio.session_count = 2;
  return 0;
}
int glass_ble_central_disconnect_generation(uint32_t generation) {
  assert(generation == 901);
  ble_closes++;
  if (fault == 8) {
    atomic_store(&entered_close, 1);
    while (!atomic_load(&release_close))
      real_sleep();
  }
  if (fault == 12)
    return -EIO;
  radio.sessions[1].connected = false;
  return 0;
}
int glass_ble_gatt(const struct glass_gatt_request *request,
                   struct glass_gatt_result *out, atomic_bool *cancel) {
  assert(request->generation == 901 && !atomic_load(cancel));
  memset(out, 0, sizeof(*out));
  out->generation = 901;
  out->mtu = 185;
  if (fault == 10) {
    out->generation = 999;
    return 0;
  }
  if (request->operation == GLASS_GATT_SERVICES) {
    out->count = 1;
    strcpy(out->items[0].uuid, service_name);
    out->items[0].start = 1;
    out->items[0].end = 10;
  }
  if (request->operation == GLASS_GATT_CHARACTERISTICS) {
    assert(request->start == 1 && request->end == 10);
    out->count = 2;
    strcpy(out->items[0].uuid, "1775ff51-6b43-439b-877c-060f2d9bed07");
    strcpy(out->items[1].uuid, "1775ff52-6b43-439b-877c-060f2d9bed07");
    out->items[0].value = 3;
    out->items[1].value = 5;
    out->items[0].properties = out->items[1].properties = 10;
  }
  return 0;
}
void jbox_wifi_crypto_init(struct jbox_wifi_crypto *c) {
  memset(c, 0, sizeof(*c));
}
void jbox_wifi_crypto_free(struct jbox_wifi_crypto *c) {
  memset(c, 0, sizeof(*c));
}
int jbox_wifi_crypto_keypair(struct jbox_wifi_crypto *c, uint8_t out[32]) {
  (void)c;
  memset(out, 0, 32);
  return 0;
}
int jbox_wifi_crypto_start(struct jbox_wifi_crypto *c, const uint8_t p[32],
                           const uint8_t iv[16]) {
  (void)c;
  (void)p;
  (void)iv;
  return 0;
}
int jbox_wifi_crypto_update(struct jbox_wifi_crypto *c, const uint8_t *in,
                            size_t n, uint8_t *out) {
  (void)c;
  memcpy(out, in, n);
  return 0;
}
int jbox_wifi_provision(const struct jbox_wifi_ops *ops, const char *ssid,
                        const char *password, char ip[16]) {
  provisions++;
  assert(!strcmp(ssid, "unit-network") && !strcmp(password, "unit-password"));
  if (fault == 8) {
    atomic_store(&blocked, 1);
    while (!atomic_load(&release_core))
      real_sleep();
  }
  int r = ops->check(ops->context);
  if (r)
    return r;
  if (fault == 6)
    return -ETIMEDOUT;
  strcpy(ip, "192.168.1.72");
  return 0;
}
bool c6_wifi_trial_pending(void) { return fault == 14; }
int glass_wifi_service_read(struct c6_desktop_result *out) {
  memset(out, 0, sizeof(*out));
  out->link.associated = out->link.ipv4_ready = fault != 13;
  out->link.generation = fault == 15 && provisions ? 2 : 1;
  return 0;
}
int c6_wifi_current_credentials(char ssid[33], char password[65]) {
  if (c6_wifi_trial_pending()) return -EBUSY;
  strcpy(ssid, "unit-network");
  strcpy(password, "unit-password");
  return 0;
}
static void add_device(cJSON *out, const char *id, const char *host,
                       bool open) {
  cJSON *array = cJSON_GetObjectItemCaseSensitive(out, "devices");
  if (!array)
    array = cJSON_AddArrayToObject(out, "devices");
  cJSON *d = cJSON_CreateObject();
  cJSON_AddStringToObject(d, "deviceId", id);
  cJSON_AddStringToObject(d, "host", host);
  cJSON_AddBoolToObject(d, "pairingOpen", open);
  cJSON_AddItemToArray(array, d);
}
int jbox_discover(cJSON *out, unsigned timeout,
                  struct qpk_hw_progress *progress) {
  (void)timeout;
  (void)progress;
  add_device(out, "aabbcc112233", "192.168.1.72", true);
  add_device(out, "aabbcc445566", "192.168.1.73", true);
  return 0;
}
int jbox_discover_host(cJSON *out, const char *host, unsigned timeout,
                       struct qpk_hw_progress *progress) {
  (void)timeout;
  (void)progress;
  assert(!strcmp(host, "192.168.1.72"));
  add_device(out, fault == 4 ? "aabbcc999999" : "aabbcc112233", host,
             fault != 3);
  return 0;
}
int portal_jbox_load(struct portal_jbox_config *out) {
  memset(out, 0, sizeof(*out));
  if (!bound)
    return -ENOENT;
  *out = binding;
  return 0;
}
int portal_jbox_save(const struct portal_jbox_config *in) {
  saves++;
  binding = *in;
  bound = true;
  return 0;
}
int qpk_jbox_execute(void **state, const char *op, const cJSON *args,
                     cJSON *out, struct qpk_hw_progress *progress) {
  if (!strcmp(op, "pair")) {
    paired++;
    assert(!atomic_load(&progress->cancel));
    struct portal_jbox_config config = {0};
    strcpy(config.host,
           cJSON_GetObjectItemCaseSensitive(args, "host")->valuestring);
    strcpy(config.device_id, "aabbcc112233");
    memset(config.key, 'a', 64);
    portal_jbox_save(&config);
    if (fault == 9)
      atomic_store(&progress->cancel, true);
    cJSON_AddBoolToObject(out, "saved", true);
    return 0;
  }
  if (!strcmp(op, "connect")) {
    connected++;
    if (fault == 5)
      return -EACCES;
    assert(!*state);
    *state = malloc(1);
    cJSON_AddNumberToObject(out, "generation", 123);
    return 0;
  }
  assert(!strcmp(op, "status") && *state);
  assert(cJSON_GetObjectItemCaseSensitive(args, "generation")->valueint == 123);
  readbacks++;
  return fault == 7 ? -EPROTO : 0;
}
void qpk_jbox_destroy(void *state) {
  if (state) {
    closed++;
    free(state);
  }
}
static void reset(void) {
  assert(!setup->view.busy);
  bound = false;
  fault = paired = connected = readbacks = closed = ble_connects = ble_closes =
      provisions = saves = 0;
  atomic_store(&blocked, 0);
  atomic_store(&release_core, 0);
  atomic_store(&entered_close, 0);
  atomic_store(&release_close, 0);
  atomic_store(&milliseconds, 0);
  memset(&radio, 0, sizeof(radio));
  radio.ready = true;
  radio.scan_generation = 77;
  radio.count = 1;
  strcpy(radio.devices[0].name, "JBOX-112233");
  radio.devices[0].address[0] = 42;
  radio.session_count = 1;
  radio.sessions[0].generation = 900;
  radio.sessions[0].connected = true;
  memset(&setup->view, 0, sizeof(setup->view));
  setup->view.generation = 10;
  setup->view.count = 2;
  setup->orphan = 0;
  struct candidate *a = &setup->candidates[0];
  memset(a, 0, sizeof(*a));
  strcpy(a->view.name, "JBOX-112233");
  strcpy(a->view.host, "192.168.1.72");
  strcpy(a->view.device_id, "aabbcc112233");
  a->view.pairing_open = true;
  setup->view.devices[0] = a->view;
  a = &setup->candidates[1];
  memset(a, 0, sizeof(*a));
  a->view.bluetooth = true;
  strcpy(a->view.name, "JBOX-112233");
  a->peer = radio.devices[0];
  a->scan = 77;
  a->index = 0;
  setup->view.devices[1] = a->view;
}
static struct glass_jbox_setup_snapshot finish(void) {
  struct glass_jbox_setup_snapshot view;
  for (unsigned i = 0; i < 10000; i++) {
    assert(!glass_jbox_setup_read(&view));
    if (!view.busy) {
      assert(radio.sessions[0].generation == 900 &&
             radio.sessions[0].connected);
      return view;
    }
    real_sleep();
  }
  assert(!"worker stalled");
  return view;
}
static struct glass_jbox_setup_snapshot run(unsigned index) {
  assert(!glass_jbox_setup_start(10, index));
  return finish();
}
int main(void) {
  struct glass_jbox_setup_snapshot v;
  assert(glass_jbox_setup_scan() == -EAGAIN);
  assert(!glass_jbox_setup_init());
  reset();
  assert(glass_jbox_setup_start(9, 1) == -ESTALE);
  assert(!setup->view.busy);
  assert(glass_jbox_setup_start(10, 2) == -ESTALE);
  reset();
  v = run(1);
  assert(v.phase == JBOX_SETUP_READY && v.binding_saved && !v.error);
  assert(provisions == 1 && paired == 1 && connected == 1 && readbacks == 1 &&
         closed == 1 && ble_closes == 1);
  puts("SETUP_WIFI_BIND_VERIFY_OK");
  for (int f = 13; f <= 15; f++) {
    reset(); fault = f; v = run(1);
    assert(v.phase == JBOX_SETUP_ERROR && !paired);
    if (f != 15) assert(!ble_connects && !provisions);
    else assert(v.error == -ESTALE && provisions == 1);
  }
  puts("SETUP_CURRENT_WIFI_ONLY_AND_NETWORK_CHANGE_OK");
  reset();
  bound = true;
  strcpy(binding.device_id, "aabbcc112233");
  strcpy(binding.host, "192.168.1.20");
  fault = 3;
  v = run(1);
  assert(v.phase == JBOX_SETUP_READY && !paired && saves == 1 &&
         !strcmp(binding.host, "192.168.1.72"));
  puts("SETUP_EXISTING_BINDING_NO_PAIR_WINDOW_OK");
  reset();
  bound = true;
  strcpy(binding.device_id, "aabbcc112233");
  strcpy(binding.host, "192.168.1.20");
  fault = 5;
  v = run(0);
  assert(v.phase == JBOX_SETUP_ERROR && !saves &&
         !strcmp(binding.host, "192.168.1.20"));
  reset();
  fault = 3;
  v = run(0);
  assert(v.phase == JBOX_SETUP_ERROR && v.error == -ETIMEDOUT && !paired);
  reset();
  fault = 4;
  v = run(1);
  assert(v.error == -ESTALE && !paired && ble_closes == 1);
  reset();
  radio.scan_generation = 78;
  v = run(1);
  assert(v.error == -ESTALE && !ble_connects && !provisions);
  reset();
  fault = 6;
  v = run(1);
  assert(v.error == -ETIMEDOUT && provisions == 1 && !paired &&
         ble_closes == 1);
  reset();
  fault = 7;
  v = run(0);
  assert(v.phase == JBOX_SETUP_ERROR && readbacks == 1 && closed == 1);
  reset();
  fault = 9;
  v = run(0);
  assert(v.phase == JBOX_SETUP_CANCEL && v.binding_saved && paired == 1 &&
         !connected);
  reset();
  fault = 10;
  v = run(1);
  assert(v.error == -ESTALE && !provisions && ble_closes == 1);
  reset();
  fault = 12;
  v = run(1);
  assert(v.cleanup_error == -EIO && v.phase == JBOX_SETUP_ERROR &&
         ble_closes == 1);
  assert(glass_jbox_setup_scan() == -EBUSY);
  radio.sessions[1].connected = false;
  reset();
  fault = 8;
  assert(!glass_jbox_setup_start(10, 1));
  while (!atomic_load(&blocked))
    real_sleep();
  glass_jbox_setup_cancel();
  atomic_store(&release_core, 1);
  while (!atomic_load(&entered_close))
    real_sleep();
  assert(!glass_jbox_setup_read(&v) && v.busy && v.cancelling);
  atomic_store(&release_close, 1);
  v = finish();
  assert(v.phase == JBOX_SETUP_CANCEL && ble_closes == 1 && !paired);
  puts("SETUP_CANCEL_WAIT_CLEANUP_OWNER_OK");
  reset();
  assert(!glass_jbox_setup_scan());
  v = finish();
  assert(v.phase == JBOX_SETUP_SELECT && v.count == 3 && !paired &&
         !provisions && !ble_connects);
  puts("SETUP_MULTI_DEVICE_EXPLICIT_SELECTION_OK");
  puts("JBOX_SETUP_HOST_OK");
  return 0;
}
