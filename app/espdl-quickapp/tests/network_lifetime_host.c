/* Production lease maintenance with virtual time, no real network traffic. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <net/if.h>
#include "link_state.h"

#define CONFIG_SYSTEM_C6_DESKTOP 1
static uint64_t fake_now;
static int fake_clock(clockid_t id, struct timespec *now)
{
  assert(id == CLOCK_MONOTONIC);
  now->tv_sec = fake_now;
  now->tv_nsec = 0;
  return 0;
}
static size_t test_strlcpy(char *dst, const char *src, size_t size)
{
  size_t len = strlen(src);
  if (size) snprintf(dst, size, "%s", src);
  return len;
}
#define clock_gettime fake_clock
#define strlcpy test_strlcpy
#include "glass_netboot.inc"
#undef clock_gettime

static struct c6_link_state g_c6link = C6_LINK_STATE_INITIALIZER;
static pthread_mutex_t g_c6net_connect_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_c6net_reconnect = true;
static bool c6_wifi_trial_pending(void) { return false; }
static unsigned connects, disconnects;
static int esp_hosted_rpc_wifi_connect(const char *ssid, const char *password)
{
  assert(!*ssid && !*password);
  connects++;
  return 0;
}
static int esp_hosted_rpc_wifi_disconnect(void)
{
  disconnects++;
  return 0;
}
#include "reconnect.inc"

struct fake_interface
{
  struct in_addr ip, gateway, mask;
  uint8_t flags;
};
static struct fake_interface interfaces[2];
static struct in_addr resolver;
static unsigned requests, closes, opened;
static uint32_t duration = 100;
static bool fail_dhcp, switch_during_dhcp, nak;
static unsigned delay;
static struct fake_interface *interface(const char *name)
{
  return &interfaces[strcmp(name, "eth0") == 0];
}
int netlib_getifstatus(const char *name, uint8_t *flags)
{ *flags = interface(name)->flags; return 0; }
int netlib_get_ipv4addr(const char *name, struct in_addr *ip)
{ *ip = interface(name)->ip; return 0; }
int netlib_get_dripv4addr(const char *name, struct in_addr *ip)
{ *ip = interface(name)->gateway; return 0; }
int netlib_getmacaddr(const char *name, uint8_t *mac)
{ (void)name; memset(mac, 1, IFHWADDRLEN); return 0; }
int netlib_set_ipv4addr(const char *name, const struct in_addr *ip)
{ interface(name)->ip = *ip; return 0; }
int netlib_set_ipv4netmask(const char *name, const struct in_addr *ip)
{ interface(name)->mask = *ip; return 0; }
int netlib_set_dripv4addr(const char *name, const struct in_addr *ip)
{ interface(name)->gateway = *ip; return 0; }
int netlib_clear_dnsaddr(void) { resolver.s_addr = 0; return 0; }
int netlib_set_ipv4dnsaddr(const struct in_addr *ip)
{ resolver = *ip; return 0; }
int netlib_ifup(const char *name)
{ interface(name)->flags = IFF_RUNNING; return 0; }
void *dhcpc_open(const char *name, const void *mac, int length)
{
  (void)mac; assert(length == IFHWADDRLEN);
  assert(!opened); opened++;
  return interface(name);
}
int dhcpc_request(void *handle, struct dhcpc_state *lease)
{
  struct fake_interface *device = handle;
  requests++;
  fake_now += delay;
  if (nak) device->ip.s_addr = 0;
  if (fail_dhcp) { errno = ETIMEDOUT; return -1; }
  lease->ipaddr.s_addr = htonl(0xc000020a);
  lease->default_router.s_addr = htonl(0xc0000201);
  lease->netmask.s_addr = htonl(0xffffff00);
  lease->dnsaddr.s_addr = htonl(handle == &interfaces[0] ? 1 : 2);
  lease->lease_time = duration;
  device->ip = lease->ipaddr; /* Real dhcpc_request installs the ACK address. */
  if (switch_during_dhcp)
    {
      assert(c6_link_update(&g_c6link, C6_LINK_DISCONNECTED) == 0);
      assert(c6_link_update(&g_c6link, C6_LINK_ASSOCIATED) == 0);
    }
  return 0;
}
void dhcpc_close(void *handle)
{ assert(handle && opened == 1); opened--; closes++; }
int c6net_get_link_snapshot(struct c6_link_snapshot *out)
{ return c6_link_read(&g_c6link, out); }
int c6net_prepare(void) { return 0; }
bool c6net_is_initialized(void) { return g_c6link.value.initialized; }

static void associate(void)
{
  assert(c6_link_update(&g_c6link, C6_LINK_ASSOCIATED) == 0);
  assert(c6_link_publish_carrier(&g_c6link,
           g_c6link.value.generation, true) == 0);
}
static int wifi_step(struct glass_net_lease_s *lease, bool requested)
{
  struct c6_link_snapshot link;
  assert(c6net_get_link_snapshot(&link) == 0);
  return glass_net_maintain("wlan0", false, lease,
            link.initialized && link.ifup && link.associated &&
            link.carrier_ready, link.generation, requested);
}
int main(void)
{
  struct glass_net_lease_s wifi = {0}, wired = {0};
  assert(c6_link_update(&g_c6link, C6_LINK_STARTED) == 0);
  assert(c6_link_update(&g_c6link, C6_LINK_IFUP) == 0);
  interfaces[0].flags = IFF_RUNNING;
  associate();
  fake_now = 1000;
  assert(wifi_step(&wifi, false) == 0 && requests == 1);
  assert(wifi.renew_at == 1050 && wifi.expires_at == 1100);
  assert(resolver.s_addr == htonl(1));
  fake_now = 1049;
  assert(wifi_step(&wifi, false) == -EINPROGRESS && requests == 1);
  fake_now = 1050;
  assert(wifi_step(&wifi, false) == 0 && requests == 2);
  assert(wifi.expires_at == 1150 && wifi.renew_at == 1100);
  fake_now = 1100; fail_dhcp = true;
  assert(wifi_step(&wifi, false) == -ETIMEDOUT && wifi.configured);
  assert(interfaces[0].ip.s_addr && resolver.s_addr);
  fake_now = 1109;
  assert(wifi_step(&wifi, false) == -EINPROGRESS && requests == 3);
  fake_now = 1149;
  assert(wifi_step(&wifi, false) == -ETIMEDOUT && wifi.configured);
  fake_now = 1150;
  assert(wifi_step(&wifi, false) == -EINPROGRESS && !wifi.configured);
  assert(!interfaces[0].ip.s_addr && !interfaces[0].gateway.s_addr);
  assert(!resolver.s_addr);
  fake_now = 1159; fail_dhcp = false;
  assert(wifi_step(&wifi, false) == 0 && wifi.configured);
  assert(c6_link_update(&g_c6link, C6_LINK_DISCONNECTED) == 0);
  assert(wifi_step(&wifi, false) == -ENETDOWN && !wifi.configured);
  associate();
  assert(wifi_step(&wifi, false) == 0 && wifi.configured);
  /* A complete disconnect/reconnect between worker ticks invalidates the IP. */
  assert(c6_link_update(&g_c6link, C6_LINK_DISCONNECTED) == 0);
  associate();
  unsigned before = requests;
  assert(wifi_step(&wifi, false) == 0 && requests == before + 1);
  /* Discard an ACK from the previous association, including its side effect. */
  switch_during_dhcp = true;
  assert(wifi_step(&wifi, true) == -EAGAIN && !wifi.configured);
  assert(!interfaces[0].ip.s_addr && !resolver.s_addr);
  switch_during_dhcp = false; associate();
  assert(wifi_step(&wifi, false) == 0);
  /* NAK removes the old lease immediately, even before its original expiry. */
  fail_dhcp = nak = true;
  assert(wifi_step(&wifi, true) == -ETIMEDOUT && !wifi.configured);
  assert(!interfaces[0].gateway.s_addr && !resolver.s_addr);
  fail_dhcp = nak = false;
  duration = 0;
  assert(wifi_step(&wifi, true) == -ESTALE && !interfaces[0].ip.s_addr);
  duration = 2; delay = 3;
  assert(wifi_step(&wifi, true) == -ESTALE && !interfaces[0].ip.s_addr);
  delay = 0; duration = 100;
  fake_now = (UINT64_C(1) << 32) + 100; /* No 32-bit uptime wrap. */
  assert(wifi_step(&wifi, true) == 0);
  assert(wifi.renew_at == fake_now + 50);
  fake_now += 50;
  assert(wifi_step(&wifi, false) == 0);
  duration = UINT32_MAX;
  assert(wifi_step(&wifi, true) == 0);
  assert(wifi.renew_at == UINT64_MAX && wifi.expires_at == UINT64_MAX);
  fake_now += 100000;
  assert(wifi_step(&wifi, false) == -EINPROGRESS);
  /* Wired uses the same lifetime policy and resolver falls back to Wi-Fi. */
  duration = 20; interfaces[1].flags = IFF_RUNNING;
  assert(glass_net_maintain("eth0", true, &wired, true, 1, false) == 0);
  assert(resolver.s_addr == htonl(2));
  fake_now += 10;
  assert(glass_net_maintain("eth0", true, &wired, true, 1, false) == 0);
  interfaces[1].flags = 0;
  assert(glass_net_maintain("eth0", true, &wired, false, 2, false) == -ENETDOWN);
  assert(!interfaces[1].ip.s_addr && resolver.s_addr == htonl(1));
  /* Background retry never overrides an explicit disconnect. */
  assert(c6net_disconnect() == 0 && disconnects == 1);
  assert(c6net_reconnect() == 0 && connects == 0);
  g_c6net_reconnect = true; /* The manual connect path re-enables retry. */
  assert(c6net_reconnect() == 0 && connects == 1);
  associate();
  assert(c6net_reconnect() == 0 && connects == 1);
  glass_net_lease(true);
  assert(g_net_lease_requested && g_net_lease_announce);
  assert(requests == closes && !opened);
  puts("PASS network lifetime: T1, retries, expiry, reconnect, stale ACK, NAK, invalid/infinite leases, long uptime, wired DNS fallback, manual disconnect");
  return 0;
}
