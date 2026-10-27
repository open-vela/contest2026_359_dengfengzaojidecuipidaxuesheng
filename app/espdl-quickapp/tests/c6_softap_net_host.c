/* SPDX-License-Identifier: Apache-2.0 */
#ifdef HOST_BAD_DOWN
#  include "net-under-test-bad-down.c"
#elif defined(HOST_BAD_SELECTOR)
#  include "net-under-test-bad.c"
#else
#  include "net-under-test.c"
#endif
#include <assert.h>
#include <inttypes.h>

struct esp_hosted_header_s
{
  uint8_t if_type : 4;
  uint8_t if_num : 4;
  uint8_t flags;
  uint16_t len, offset, checksum, seq_num;
  uint8_t throttle, pkt_type;
} __attribute__((packed));
static struct
{
  esp_hosted_rx_cb_t rx_cb[8];
  void *rx_arg[8];
} g_hosted;
#define rxcb g_hosted.rx_cb
#define rxarg g_hosted.rx_arg
static void hosted_parse_init(const uint8_t *data, uint16_t len)
{ (void)data; (void)len; assert(!"private INIT not expected in this fixture"); }
static int hosted_send_host_config(void)
{ assert(!"private INIT not expected in this fixture"); return 0; }
#include "transport-dispatch.inc"
static unsigned calls, fail_at, sent_type, inputs, expected_type, connects;
static unsigned initializes, wifi_starts;
static unsigned offline_notifications, queued_forwarders;
int netdev_ifdown(struct net_driver_s *dev)
{
  if (dev->d_flags & IFF_UP)
    {
      int ret = dev->d_ifdown(dev);
      if (ret) return ret;
      offline_notifications++;
      queued_forwarders = 0; /* Native NETDEV_DOWN retires callback owners. */
    }
  return 0;
}
static int send_error;
static bool fail_all, trial_pending, trial_fail;
static bool trial_event, trial_reject;
static int radio_mode = 1;
static int reported_mode = -1;
static bool radio_started = true, ap_configured;
static struct c6_wifi_phy_config_s radio_phy[2];
static unsigned phy_calls, phy_fail_at, phy_writes;
static bool phy_fail_rest, phy_disconnect, phy_unclean;

static int phy_step(void)
{
  phy_calls++;
  return phy_fail_at && (phy_calls == phy_fail_at ||
         (phy_fail_rest && phy_calls > phy_fail_at)) ?
         (phy_unclean ? -EUCLEAN : -ETIMEDOUT) : 0;
}
int esp_hosted_rpc_phy_storage_restore(void) { return phy_step(); }
int esp_hosted_rpc_phy_get_protocol(int iface, uint8_t *value)
{ int ret = phy_step(); if (!ret) *value = radio_phy[iface].protocol; return ret; }
int esp_hosted_rpc_phy_get_bandwidth(int iface, uint8_t *value)
{ int ret = phy_step(); if (!ret) *value = radio_phy[iface].bandwidth; return ret; }
int esp_hosted_rpc_phy_set_protocol(int iface, uint8_t value)
{
  assert(iface == 1); phy_writes++;
  assert(!(value & C6_WIFI_PROTOCOL_AX) || radio_phy[1].bandwidth == C6_WIFI_BW20);
  radio_phy[iface].protocol = value;
  return phy_step();
}
int esp_hosted_rpc_phy_set_bandwidth(int iface, uint8_t value)
{
  assert(iface == 1); phy_writes++;
  assert(value != C6_WIFI_BW40 || !(radio_phy[1].protocol & C6_WIFI_PROTOCOL_AX));
  radio_phy[iface].bandwidth = value;
  return phy_step();
}
int esp_hosted_rpc_phy_clients(uint8_t *count, uint8_t *he)
{ int ret = phy_step(); if (!ret) { *count = 2; *he = 1; } return ret; }
int esp_hosted_rpc_phy_sta(uint8_t *mode)
{
  int ret = phy_step();
  if (!ret) *mode = C6_WIFI_PHY_HE20;
  if (phy_disconnect) c6net_wifi_event(NULL, false);
  return ret;
}
static pthread_mutex_t netlock = PTHREAD_MUTEX_INITIALIZER;

static void dispatch_frame(uint8_t type, uint8_t instance, uint16_t offset)
{
  uint8_t wire[12 + 64] = {0};
  struct esp_hosted_header_s *h = (void *)wire;
  _Static_assert(sizeof(*h) == 12, "Hosted wire header ABI");
  h->if_type = type; h->if_num = instance;
  h->offset = offset; h->len = 64;
  wire[12 + 12] = 8; /* IPv4 EtherType at Ethernet header offset 12. */
  assert(wire[0] == (uint8_t)(type | (instance << 4)));
  hosted_dispatch(wire, sizeof(wire));
}

static unsigned legacy_received;
static void legacy_receive(void *arg, uint8_t selector, const uint8_t *data,
                           uint16_t len, uint8_t packet_type)
{
  assert(selector == *(uint8_t *)arg);
  assert(data && len == 64 && packet_type == 0);
  legacy_received++;
}

static int step(void)
{
  calls++;
  return (calls == fail_at || fail_all) ? -ETIMEDOUT : 0;
}

size_t strlcpy(char *dst, const char *src, size_t cap)
{
  size_t n = strlen(src);
  if (cap) { size_t m = n < cap - 1 ? n : cap - 1; memcpy(dst, src, m); dst[m] = 0; }
  return n;
}

void netdev_lock(struct net_driver_s *dev) { (void)dev; assert(!pthread_mutex_lock(&netlock)); }
void netdev_unlock(struct net_driver_s *dev) { (void)dev; assert(!pthread_mutex_unlock(&netlock)); }
void netdev_carrier_on(struct net_driver_s *dev) { dev->d_flags |= IFF_RUNNING; }
void netdev_carrier_off(struct net_driver_s *dev) { dev->d_flags &= ~IFF_RUNNING; }
int netdev_register(struct net_driver_s *dev, int type)
{
  assert(type == NET_LL_ETHERNET);
  const char *name = !strcmp(dev->d_ifname, "wlan%d") ? "wlan0" : "ap0";
  strlcpy(dev->d_ifname, name, sizeof(dev->d_ifname));
  return step();
}
int devif_poll(struct net_driver_s *dev, int (*cb)(struct net_driver_s *)) { return cb(dev); }
static void input(struct net_driver_s *dev)
{
  struct c6net_state_s *priv = dev->d_private;
  assert(priv->if_type == expected_type);
  inputs++;
  dev->d_len = 14; /* Immediate stack response belongs to the same interface. */
}
void ipv4_input(struct net_driver_s *dev) { input(dev); }
void arp_input(struct net_driver_s *dev) { input(dev); }
pid_t task_create(const char *n, int p, int s, int (*f)(int, char **), char **a)
{ (void)n; (void)p; (void)s; (void)f; (void)a; return 42; }
int esp_hosted_initialize(bool verbose) { (void)verbose; initializes++; return step(); }
int esp_hosted_rpc_wifi_init(void) { return step(); }
int esp_hosted_rpc_wifi_set_mode(int mode)
{
  int ret = step();
  if (!ret) radio_mode = c6_softap_preserve_mode(mode);
  return ret;
}
int esp_hosted_rpc_wifi_start(void)
{
  wifi_starts++;
  int ret = step();
  if (!ret) {
    /* No default/old AP can be broadcast before validated configuration. */
    assert(radio_mode != 3 || ap_configured);
    radio_started = true;
  }
  return ret;
}
int esp_hosted_rpc_wifi_stop(void)
{
  int ret = step();
  if (!ret) radio_started = false;
  return ret;
}
int esp_hosted_rpc_softap_configure(const char *s, const char *p)
{
  assert(!radio_started && radio_mode == 3 && s && p);
  int ret = step(); if (!ret) ap_configured = true; return ret;
}
int esp_hosted_rpc_softap_verify(const char *s, const char *p)
{ assert(!radio_started && ap_configured && s && p); return step(); }
int esp_hosted_rpc_wifi_get_mode(int *mode)
{ int ret = step(); if (!ret) *mode = reported_mode >= 0 ? reported_mode : radio_mode; return ret; }
int esp_hosted_rpc_get_mac(int iface, uint8_t mac[6])
{ int ret = step(); memset(mac, 0, 6); mac[0] = 0x98; mac[5] = (uint8_t)(0x90 + iface); return ret; }
int esp_hosted_rpc_set_wifi_event_cb(esp_hosted_wifi_event_cb_t cb, void *arg)
{ assert(cb && arg); return 0; }
int esp_hosted_register(uint8_t type, esp_hosted_rx_cb_t cb, void *arg)
{ rxcb[type] = cb; rxarg[type] = arg; return 0; }
int esp_hosted_send(uint8_t type, uint8_t num, const uint8_t *p, uint16_t n)
{ assert(num == 0 && p && n); sent_type = type; return send_error; }
int esp_hosted_poll(void) { return 0; }
bool esp_hosted_rpc_is_waiting(void) { return false; }
int esp_hosted_rpc_wifi_connect(const char *s, const char *p)
{ assert(s && p); connects++; return step(); }
int esp_hosted_rpc_wifi_disconnect(void) { return step(); }
bool c6_wifi_trial_pending(void) { return trial_pending; }
int c6_wifi_trial_begin(const char *s, const char *p)
{
  assert(s && p);
  if (trial_reject) return -EINVAL;
  trial_pending = true;
  if (trial_event) {
    c6net_wifi_event(NULL, false);
    c6net_wifi_event(NULL, true);
  }
  return trial_fail ? -EIO : 0;
}
int c6_wifi_trial_finish(bool commit)
{ (void)commit; if (trial_fail) return -EIO; trial_pending = false; return 0; }

static void clean(void)
{
  fail_at = calls = connects = 0;
  fail_all = trial_pending = trial_fail = false;
  trial_event = trial_reject = false;
  g_c6net_trial_owned = false;
  g_c6net_reconnect = true;
  radio_mode = 1; radio_started = true; ap_configured = false;
  reported_mode = -1;
  phy_calls = phy_fail_at = phy_writes = 0;
  phy_fail_rest = phy_disconnect = phy_unclean = false;
  g_c6ap_he_preferred = true;
  radio_phy[0] = (struct c6_wifi_phy_config_s){0x47, C6_WIFI_BW20};
  radio_phy[1] = (struct c6_wifi_phy_config_s){0x07, C6_WIFI_BW40};
  if (g_c6ap) { c6_softap_offline(g_c6ap); memset(&g_c6ap->state, 0, sizeof(g_c6ap->state)); }
}

static void test_remote_reconcile(void)
{
  struct c6_softap_snapshot_s state;
  struct c6_softap_private_s *saved = g_c6ap;
  unsigned init_before = initializes;
  unsigned start_before = wifi_starts;

  /* Simulate a new P4 lifetime while C6 still owns an AP. No local AP heap
   * object, transport reset, Wi-Fi start, or implicit STA reconnect allowed.
   */
  g_c6ap = NULL; g_c6ap_mode_error = -EAGAIN;
  g_c6net_reconnect = false;
  assert(!c6_softap_status(&state) && state.state_unknown && state.error == -EAGAIN);
  radio_mode = 3;
  assert(!c6_softap_stop() && radio_mode == 1);
  assert(!c6_softap_status(&state) && !state.state_unknown && !state.active);
  assert(!g_c6ap && !g_c6net_reconnect && !connects);
  radio_mode = 2;
  assert(!c6_softap_stop() && radio_mode == 0);
  radio_started = false;
  unsigned old = calls;
  assert(!c6_softap_stop() && radio_mode == 0 && calls == old + 1);
  assert(!radio_started);

  /* A lost query or a rejected mode change cannot report AP-off. */
  radio_mode = 3; fail_at = calls + 1;
  assert(c6_softap_stop() == -ETIMEDOUT && radio_mode == 3);
  assert(!c6_softap_status(&state) && state.state_unknown && state.error == -ETIMEDOUT);
  fail_at = calls + 2;
  assert(c6_softap_stop() == -ETIMEDOUT && radio_mode == 3);
  assert(!c6_softap_status(&state) && state.state_unknown);
  fail_at = 0; reported_mode = 4;
  assert(c6_softap_stop() == -EPROTO);
  reported_mode = 3;
  assert(c6_softap_stop() == -EPROTO); /* Setter succeeds, readback disagrees. */
  reported_mode = -1;
  assert(!c6_softap_stop());
  assert(!c6_softap_status(&state) && !state.state_unknown);
  assert(initializes == init_before && wifi_starts == start_before);
  assert(!connects && !g_c6net_reconnect);

  g_c6ap = saved;
  clean();
}

static void test_phy(void)
{
  struct c6_wifi_phy_snapshot_s state;
  clean();
  assert(!c6_softap_start("Unit", "test-password"));
  assert(phy_calls == 12 && phy_writes == 2);
  assert(!c6_softap_phy_read(&state));
  assert(state.preferred && state.configured && state.valid);
  assert(state.ap.protocol == 0x47 && state.ap.bandwidth == C6_WIFI_BW20);
  assert(state.before_ap.bandwidth == C6_WIFI_BW40);
  assert(c6_softap_phy_prefer(false) == -EBUSY);
  uint32_t generation = state.generation;
  c6net_wifi_event(NULL, true);
  assert(!c6_softap_phy_refresh());
  assert(!c6_softap_phy_read(&state));
  assert(state.clients_valid && state.clients == 2 && state.he_clients == 1);
  assert(state.sta_valid && state.sta_phy == 6);
  phy_disconnect = true;
  assert(!c6_softap_phy_refresh());
  assert(!c6_softap_phy_read(&state) && !state.sta_valid);
  assert(!c6_softap_stop());
  assert(!c6_softap_phy_read(&state) && !state.valid && !state.clients_valid);
  assert(!c6_softap_phy_prefer(false));
  assert(!c6_softap_start("Unit", "test-password"));
  assert(!c6_softap_phy_read(&state) && state.generation == generation + 1);
  assert(!state.preferred && state.valid && state.ap.protocol == 7);
  assert(radio_phy[0].protocol == 0x47);
  assert(!c6_softap_stop());

  /* Every initial PHY transaction may lose its reply after execution. */
  for (unsigned i = 1; i <= 12; i++)
    {
      clean(); phy_fail_at = i;
      int ret = c6_softap_start("Unit", "test-password");
      assert(!c6_softap_phy_read(&state));
      if (ret == 0)
        {
          assert(state.valid && g_c6ap->state.active);
          assert(state.configured || (state.apply_error && !state.restore_error));
          assert(c6_phy_equal(&state.ap, &radio_phy[1]));
          assert(!c6_softap_stop());
        }
      else
        {
          assert(!g_c6ap->state.active && !state.valid && radio_mode == 1);
          assert(radio_phy[1].protocol == 7 && radio_phy[1].bandwidth == C6_WIFI_BW40);
        }
      assert(radio_phy[0].protocol == 0x47 && radio_phy[0].bandwidth == C6_WIFI_BW20);
    }
  clean(); phy_fail_at = 7; phy_fail_rest = true;
  assert(c6_softap_start("Unit", "test-password") < 0);
  assert(!c6_softap_phy_read(&state) && state.restore_error && !state.valid);
  assert(!g_c6ap->state.active);
  assert(phy_writes == 3);
  clean(); phy_fail_at = 5; phy_fail_rest = phy_unclean = true;
  assert(c6_softap_start("Unit", "test-password") == -EUCLEAN);
  assert(!c6_softap_phy_read(&state) && state.restore_error == -EUCLEAN);
  assert(!state.valid && g_c6ap->state.state_unknown);
  assert(c6_softap_stop() == -EUCLEAN && g_c6ap->state.state_unknown);
  phy_fail_at = 0;
  assert(!c6_softap_stop() && !g_c6ap->state.state_unknown);
  clean();
  assert(!c6_softap_start("Unit", "test-password"));
  assert(!c6_softap_stop());
  puts("C6_WIFI_PHY_NET_OK: HE20/legacy, readback, lost replies, bounded rollback, STA isolation, generations");
}

static void test_adapter_stats(void)
{
  struct c6net_stats_s sta, ap, before;
  unsigned io_before = calls, input_before = inputs;
  assert(c6net_stats_read(false, NULL) == -EINVAL);
  assert(!c6net_stats_read(false, &sta) && !c6net_stats_read(true, &ap));
  assert(calls == io_before); /* No RPC, registration or radio I/O on read. */
  assert(sta.rx_packets >= 1 && ap.rx_packets >= 1 && ap.rx_invalid >= 1);
  assert(sta.tx_packets >= 1 && ap.tx_packets >= 1);
  assert(ap.rx_down_drops >= 1);
  before = sta;
  expected_type = ESP_HOSTED_IF_STA;
  send_error = -ETIMEDOUT;
  dispatch_frame(ESP_HOSTED_IF_STA, 0, 12);
  send_error = 0;
  assert(!c6net_stats_read(false, &sta));
  assert(sta.tx_errors == before.tx_errors + 1 && sta.tx_last_error == -ETIMEDOUT);
  assert(sta.tx_packets == before.tx_packets && sta.rx_packets == before.rx_packets + 1);
  assert(inputs == input_before + 1);
  uint32_t saved = g_c6net_stats[0].tx_bytes;
  g_c6net_stats[0].tx_bytes = UINT32_MAX;
  c6net_stat_add(&g_c6net_stats[0].tx_bytes, 1);
  assert(!c6net_stats_read(false, &sta) && sta.tx_bytes == 0);
  g_c6net_stats[0].tx_bytes = saved;
  puts("C6NET_STATS_OK: separate STA/AP, no radio I/O, ingress rejection, send error, accepted counts, rollover");
}

int main(void)
{
  struct c6_softap_snapshot_s initial;
  assert(!c6_softap_status(&initial) && initial.state_unknown);
  /* Initial setup must not publish an initialized interface until mode1
   * has actually been read back, even after P4 state loss.
   */
  radio_mode = 3; fail_at = 4;
  assert(c6net_prepare() == -ETIMEDOUT && !c6net_is_initialized());
  assert(wifi_starts == 0);
  assert(!c6_softap_status(&initial) && initial.state_unknown);
  fail_at = 0; calls = 0; reported_mode = 3;
  assert(c6net_prepare() == -EPROTO && !c6net_is_initialized());
  assert(!c6_softap_status(&initial) && initial.state_unknown);
  reported_mode = -1;
  assert(!c6net_prepare());
  assert(radio_mode == 1);
  assert(!c6_softap_status(&initial) && !initial.state_unknown);
  assert(g_c6net.if_type == ESP_HOSTED_IF_STA);
  assert(rxcb[ESP_HOSTED_IF_STA] && !rxcb[ESP_HOSTED_IF_AP]);
  clean();
  test_remote_reconcile();
  assert(c6_softap_start("", "test-password") == -EINVAL && !calls);
  assert(c6_softap_start("Unit", "short") == -EINVAL && !calls);
  assert(!c6_softap_start("Unit", "test-password"));
  assert(radio_mode == 3 && radio_started && g_c6ap->state.active);
  assert(!strcmp(g_c6ap->state.ifname, "ap0"));
  assert(g_c6ap->state.sta_interrupted && connects == 1);
  assert(rxcb[ESP_HOSTED_IF_AP] && rxarg[ESP_HOSTED_IF_AP] != rxarg[ESP_HOSTED_IF_STA]);
  unsigned old = calls;
  assert(c6_softap_start("Unit", "test-password") == -EALREADY && calls == old);
  assert(c6_softap_preserve_mode(1) == 3);
  unsigned init_before = initializes, start_before = wifi_starts;
  assert(!c6_link_update(&g_c6link, C6_LINK_STOPPED));
  assert(!c6net_prepare() && radio_mode == 3 && g_c6ap->state.active);
  assert(initializes == init_before && wifi_starts == start_before);
  uint8_t frame[64] = {0}; frame[12] = 8;
  expected_type = ESP_HOSTED_IF_AP;
  dispatch_frame(ESP_HOSTED_IF_AP, 0, 12);
  assert(inputs == 1 && sent_type == ESP_HOSTED_IF_AP);
  expected_type = ESP_HOSTED_IF_STA;
  dispatch_frame(ESP_HOSTED_IF_STA, 0, 12);
  assert(inputs == 2 && sent_type == ESP_HOSTED_IF_STA);
  dispatch_frame(ESP_HOSTED_IF_AP, 1, 12);
  dispatch_frame(ESP_HOSTED_IF_STA, 1, 12);
  dispatch_frame(ESP_HOSTED_IF_AP, 0, 11);
  dispatch_frame(ESP_HOSTED_IF_AP, 0, 13);
  dispatch_frame(15, 0, 12);
  rxcb[ESP_HOSTED_IF_AP](rxarg[ESP_HOSTED_IF_AP], ESP_HOSTED_IF_STA,
                        frame, sizeof(frame), 0);
  assert(inputs == 2);
  uint8_t hci_selector = ESP_HOSTED_IF_HCI;
  uint8_t serial_selector = ESP_HOSTED_IF_SERIAL;
  assert(!esp_hosted_register(hci_selector, legacy_receive, &hci_selector));
  assert(!esp_hosted_register(serial_selector, legacy_receive, &serial_selector));
  dispatch_frame(hci_selector, 0, 12);
  dispatch_frame(serial_selector, 0, 12);
  assert(legacy_received == 2); /* Existing HCI/RPC selector contract retained. */
  unsigned down_before = offline_notifications;
  queued_forwarders = 4;
  assert(!c6_softap_stop() && radio_mode == 1);
  assert(offline_notifications == down_before + 1 && queued_forwarders == 0);
  dispatch_frame(ESP_HOSTED_IF_AP, 0, 12);
  assert(inputs == 2 && c6_softap_preserve_mode(1) == 1);
  old = calls; assert(!c6_softap_stop() && calls == old + 1);
  /* Retained local allocation is no stronger proof than a missing one. */
  radio_mode = 3;
  init_before = initializes; start_before = wifi_starts;
  assert(!c6_softap_stop() && radio_mode == 1);
  assert(initializes == init_before && wifi_starts == start_before);
  assert(!g_c6ap->state.active && !g_c6ap->state.state_unknown);
  assert(c6net_ifup(&g_c6ap->net.dev) == -ENETDOWN);
  clean(); g_c6net_reconnect = false;
  assert(!c6_softap_start("Unit", "test-password") && !connects);
  assert(!c6_softap_stop() && !g_c6net_reconnect);
  /* Each step may fail once. Cleanup must leave AP down and restore STA. */
  for (unsigned i = 1; i <= 6; i++) {
    clean(); fail_at = i;
    assert(c6_softap_start("Unit", "test-password") < 0);
    assert(!g_c6ap->state.active && !g_c6ap_requested);
    assert(!(g_c6ap->net.dev.d_flags & IFF_UP));
    assert(radio_mode == 1 && radio_started);
    assert(!g_c6ap->state.state_unknown);
  }
  /* A fresh AP registration must also cleanly release failed MAC/netdev
   * preparation, including its independently registered RX callback.
   */
  for (unsigned i = 1; i <= 8; i++) {
    clean(); g_c6ap->net.registered = false; fail_at = i;
    assert(c6_softap_start("Unit", "test-password") < 0);
    assert(!g_c6ap->state.active && !g_c6ap_requested);
    assert(radio_mode == 1 && radio_started);
    assert(!g_c6ap->state.state_unknown);
    if (i == 6) assert(!rxcb[ESP_HOSTED_IF_AP]);
  }
  clean();
  assert(!c6_softap_start("Unit", "test-password"));
  fail_at = calls + 1;
  assert(c6_softap_stop() < 0 && g_c6ap->state.state_unknown);
  assert(!g_c6ap->state.active && !g_c6ap_requested);
  fail_at = 0; assert(!c6_softap_stop());
  clean(); fail_all = true;
  assert(c6_softap_start("Unit", "test-password") < 0);
  assert(g_c6ap->state.state_unknown && !g_c6ap->state.active);
  fail_all = false;
  assert(c6_softap_start("Unit", "test-password") == -EBUSY);
  assert(!c6_softap_stop() && !g_c6ap->state.state_unknown);
  clean();
  assert(!c6net_trial_connect("NewSSID", "new-password"));
  assert(trial_pending && !g_c6net_reconnect);
  assert(c6net_connect("Other", "other-password") == -EBUSY);
  assert(c6net_disconnect() == -EBUSY && c6net_reconnect() == -EBUSY);
  assert(c6_softap_start("Unit", "test-password") == -EBUSY);
  assert(c6_softap_stop() == -EBUSY);
  trial_fail = true;
  assert(c6net_trial_finish(true) == -EIO && trial_pending && !g_c6net_reconnect);
  trial_fail = false;
  assert(!c6net_trial_finish(false) && !trial_pending && g_c6net_reconnect);
  clean(); g_c6net_reconnect = false;
  assert(!c6net_trial_connect("NewSSID", "new-password"));
  assert(!c6net_trial_finish(false) && !connects && !g_c6net_reconnect);
  assert(!c6net_trial_connect("NewSSID", "new-password"));
  assert(!c6net_trial_finish(true) && g_c6net_reconnect);
  clean(); trial_fail = true;
  assert(c6net_trial_connect("NewSSID", "new-password") == -EIO);
  assert(trial_pending && !g_c6net_reconnect && g_c6net_trial_owned);
  trial_fail = false; assert(!c6net_trial_finish(false));
  clean(); c6net_wifi_event(NULL, true); c6net_sync_carrier(&g_c6net.dev);
  struct c6_link_snapshot before, after;
  assert(!c6net_get_link_snapshot(&before) && before.associated);
  trial_reject = true;
  assert(c6net_trial_connect("NewSSID", "new-password") == -EINVAL);
  assert(!c6net_get_link_snapshot(&after));
  assert(after.generation == before.generation && after.associated);
  clean(); trial_event = true;
  assert(!c6net_trial_connect("NewSSID", "new-password"));
  assert(!c6net_get_link_snapshot(&after) && after.associated);
  assert(!c6net_trial_finish(true));
  test_phy();
  test_adapter_stats();
  puts("C6_SOFTAP_NET_OK: actual wire dispatch -> STA/AP callbacks -> per-interface stack replies; instance/length guards; HCI/RPC selectors unchanged; startup/readback; warm-reset reconciliation; fault/cleanup/trial ownership");
  return 0;
}
