/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int test_clock_gettime(clockid_t, struct timespec *);
static int test_open(const char *, int, ...);
static ssize_t test_read(int, void *, size_t);
static int test_close(int);
#define clock_gettime test_clock_gettime
#define open test_open
#define read test_read
#define close test_close
#include "recovery-under-test.c"
#undef clock_gettime
#undef open
#undef read
#undef close

static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t owner;
static bool owner_set;
static struct c6_desktop_link host_link;
static struct c6_softap_snapshot_s host_ap;
static struct dhcpd_runtime_status_s host_dhcp = {.pid = -1};
static _Atomic uint64_t seconds_now = 10, lease_generation;
static void check_owner(void);
int c6_softap_phy_refresh(void) { check_owner(); return 0; }
static uint8_t saved_ap_ip[4];
#ifdef CONFIG_NET_NAT44
static _Atomic uint32_t wired_ip;
void glass_ethernet_share_stop(void) { check_owner(); }
void glass_ethernet_maintain(bool share, struct glass_ethernet_s *out)
{
  check_owner(); memset(out,0,sizeof(*out));
  uint32_t current=atomic_load(&wired_ip), apip=0;memcpy(&apip,saved_ap_ip,4);
  if(share && current && (current&htonl(0xffffff00u))==(apip&htonl(0xffffff00u)))
    out->error=-EADDRINUSE;
}
int dhcpd_set_routerip(uint32_t address)
{check_owner();uint32_t expected=0;memcpy(&expected,saved_ap_ip,4);assert(address==ntohl(expected));return 0;}
#endif
static atomic_bool pending, block_start, start_entered;
static atomic_int random_error, ap_start_error, ap_stop_error, ap_status_error;
static atomic_int dhcp_start_error, dhcp_stop_error, dhcp_status_error;
static atomic_int portal_error, net_fail_step, net_steps;
static atomic_int trial_begin_error, trial_finish_error, status_error;
static atomic_uint starts, stops, dhcp_starts, dhcp_stops, commits, aborts;
static atomic_uint leases, maintains, serial_random;
static bool finish_changes_link;

static void sleep_ms(unsigned ms)
{
  struct timespec delay = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
  nanosleep(&delay, NULL);
}

static void check_owner(void)
{
  pthread_mutex_lock(&host_lock);
  if (!owner_set) { owner = pthread_self(); owner_set = true; }
  assert(pthread_equal(owner, pthread_self()));
  pthread_mutex_unlock(&host_lock);
}

static int test_clock_gettime(clockid_t which, struct timespec *out)
{
  assert(which == CLOCK_MONOTONIC);
  out->tv_sec = (time_t)atomic_load(&seconds_now); out->tv_nsec = 0;
  return 0;
}
static int test_open(const char *path, int flags, ...)
{
  assert(!strcmp(path, "/dev/urandom") && flags == O_RDONLY);
  int error = atomic_load(&random_error);
  if (error) { errno = error; return -1; }
  return 4000;
}
static ssize_t test_read(int fd, void *data, size_t size)
{
  assert(fd == 4000 && size);
  unsigned serial = atomic_fetch_add(&serial_random, 1);
  for (size_t i = 0; i < size; i++) ((uint8_t *)data)[i] = (uint8_t)(serial + i * 7);
  return (ssize_t)size;
}
static int test_close(int fd) { assert(fd == 4000); return 0; }

static int backend_scan(struct c6_scan_ap *records, size_t size, size_t *count)
{ check_owner(); assert(records && size); *count = 0; return 0; }
static int backend_connect(const char *s, const char *p)
{ (void)s; (void)p; assert(!"service must use RAM trial callback"); return -EIO; }
static int backend_disconnect(void)
{
  check_owner(); pthread_mutex_lock(&host_lock);
  memset(&host_link, 0, sizeof(host_link));
  pthread_mutex_unlock(&host_lock); return 0;
}
static int backend_status(struct c6_desktop_link *out)
{
  check_owner(); atomic_fetch_add(&maintains, 1);
  int error = atomic_load(&status_error); if (error) return error;
  pthread_mutex_lock(&host_lock); *out = host_link; pthread_mutex_unlock(&host_lock);
  return 0;
}
const struct c6_desktop_backend g_c6_desktop_backend = {
  .scan = backend_scan, .connect = backend_connect,
  .status = backend_status, .disconnect = backend_disconnect
};

int c6_softap_start(const char *ssid, const char *password)
{
  check_owner(); atomic_fetch_add(&starts, 1); start_entered = true;
  assert(strlen(ssid) < 33 && strlen(password) == 12);
  while (atomic_load(&block_start)) sleep_ms(1);
  int ret = atomic_load(&ap_start_error);
  pthread_mutex_lock(&host_lock);
  host_ap.active = ret == 0;
  host_ap.state_unknown = ret != 0;
  strcpy(host_ap.ssid, ssid); strcpy(host_ap.ifname, C6_SOFTAP_IFNAME);
  pthread_mutex_unlock(&host_lock); return ret;
}
int c6_softap_stop(void)
{
  check_owner(); atomic_fetch_add(&stops, 1);
  int ret = atomic_load(&ap_stop_error);
  pthread_mutex_lock(&host_lock);
  if (!ret) host_ap.active = host_ap.state_unknown = false;
  pthread_mutex_unlock(&host_lock); return ret;
}
int c6_softap_status(struct c6_softap_snapshot_s *out)
{
  check_owner(); int ret = atomic_load(&ap_status_error); if (ret) return ret;
  pthread_mutex_lock(&host_lock); *out = host_ap; pthread_mutex_unlock(&host_lock);
  return 0;
}
bool c6_wifi_trial_pending(void) { return atomic_load(&pending); }
int c6net_trial_connect(const char *ssid, const char *password)
{
  check_owner(); assert(ssid && password); pending = true;
  pthread_mutex_lock(&host_lock);
  host_link.generation++; host_link.initialized = true;
  host_link.associated = host_link.carrier_ready = host_link.ipv4_ready = false;
  pthread_mutex_unlock(&host_lock); return atomic_load(&trial_begin_error);
}
int c6net_trial_finish(bool commit)
{
  check_owner(); assert(pending);
  if (commit) atomic_fetch_add(&commits, 1); else atomic_fetch_add(&aborts, 1);
  int ret = atomic_load(&trial_finish_error);
  if (!ret) {
    pending = false;
    pthread_mutex_lock(&host_lock);
    if (!commit || finish_changes_link) {
      host_link.generation++;
      host_link.ipv4_ready = false;
    }
    pthread_mutex_unlock(&host_lock);
  }
  return ret;
}
int c6net_get_link_snapshot(struct c6_link_snapshot *out)
{
  check_owner(); pthread_mutex_lock(&host_lock);
  memset(out, 0, sizeof(*out)); out->generation = host_link.generation;
  out->initialized = true; out->ifup = true;
  out->associated = host_link.associated; out->carrier_ready = host_link.carrier_ready;
  pthread_mutex_unlock(&host_lock); return 0;
}
void glass_wifi_request_lease(void) { check_owner(); atomic_fetch_add(&leases, 1); }
bool glass_wifi_lease_ready(uint64_t generation)
{ return generation && generation == atomic_load(&lease_generation); }

int netlib_get_ipv4addr(const char *name, struct in_addr *out)
{ check_owner(); assert(strcmp(name, C6_SOFTAP_IFNAME)); out->s_addr = 0;
#ifdef CONFIG_NET_NAT44
  if(!strcmp(name,"eth0"))out->s_addr=atomic_load(&wired_ip);
#endif
  return 0; }
int netlib_get_ipv4netmask(const char *name, struct in_addr *out)
{ check_owner(); (void)name; out->s_addr = htonl(0xffffff00u); return 0; }
static int net_set(const char *name, const struct in_addr *address, int kind)
{
  check_owner(); assert(!strcmp(name, C6_SOFTAP_IFNAME));
  int step = atomic_fetch_add(&net_steps, 1) + 1;
  if (step == atomic_load(&net_fail_step)) { errno = EIO; return -1; }
  if (kind == 0) memcpy(saved_ap_ip, &address->s_addr, 4);
  if (kind == 1) assert(!address->s_addr); /* AP is never the Internet gateway. */
  return 0;
}
int netlib_set_ipv4addr(const char *n, const struct in_addr *a) { return net_set(n, a, 0); }
int netlib_set_dripv4addr(const char *n, const struct in_addr *a) { return net_set(n, a, 1); }
int netlib_set_ipv4netmask(const char *n, const struct in_addr *a) { return net_set(n, a, 2); }
int dhcpd_set_startip(uint32_t ip) { check_owner(); assert((ip & 255) == 2); return 0; }
int dhcpd_set_netmask(uint32_t mask) { check_owner(); assert(mask == 0xffffff00u); return 0; }
int dhcpd_start(const char *name)
{
  check_owner(); assert(!strcmp(name, C6_SOFTAP_IFNAME));
  atomic_fetch_add(&dhcp_starts, 1);
  pthread_mutex_lock(&host_lock);
  host_dhcp.running = true; host_dhcp.ready = false; host_dhcp.pid = 777;
  pthread_mutex_unlock(&host_lock); return atomic_load(&dhcp_start_error);
}
int dhcpd_stop(void)
{
  check_owner(); atomic_fetch_add(&dhcp_stops, 1);
  int ret = atomic_load(&dhcp_stop_error);
  if (!ret) { pthread_mutex_lock(&host_lock); memset(&host_dhcp, 0, sizeof(host_dhcp));
    host_dhcp.pid = -1; pthread_mutex_unlock(&host_lock); }
  return ret;
}
int dhcpd_runtime_status(struct dhcpd_runtime_status_s *out)
{
  check_owner(); int ret = atomic_load(&dhcp_status_error); if (ret) return ret;
  pthread_mutex_lock(&host_lock); *out = host_dhcp; pthread_mutex_unlock(&host_lock); return 0;
}
int portal_service_start(void) { check_owner(); return atomic_load(&portal_error); }

static struct glass_recovery_snapshot_s snapshot(void)
{
  struct glass_recovery_snapshot_s out;
  assert(!glass_network_recovery_read(&out)); return out;
}
static void wait_mailbox(void)
{
  for (unsigned i = 0; i < 3000; i++) {
    struct c6_desktop_result result; assert(!glass_wifi_service_read(&result));
    if (!result.busy) return;
    sleep_ms(1);
  }
  assert(!"network worker did not finish its bounded backend request");
}
static void wait_state(enum glass_recovery_state_e state)
{
  for (unsigned i = 0; i < 3000; i++) {
    if (snapshot().state == state) return;
    sleep_ms(1);
  }
  struct glass_recovery_snapshot_s s = snapshot();
  fprintf(stderr, "state wait: actual=%d expected=%d ap_error=%d cleanup=%d\n",
          s.state, state, s.ap_error, s.cleanup_error);
  assert(!"recovery state not reached");
}
static void wait_not_joining(void)
{
  for (unsigned i = 0; i < 3000; i++) {
    if (!snapshot().joining) return;
    sleep_ms(1);
  }
  assert(!"trial did not finish");
}
static void clear_errors(void)
{
  random_error = ap_start_error = ap_stop_error = ap_status_error = 0;
  dhcp_start_error = dhcp_stop_error = dhcp_status_error = portal_error = 0;
  trial_begin_error = trial_finish_error = status_error = 0;
  net_fail_step = net_steps = 0; block_start = false;
}
static void reset_service(void)
{
  clear_errors(); wait_mailbox();
  assert(!glass_network_recovery_stop()); wait_mailbox(); wait_state(GLASS_RECOVERY_OFF);
  assert(!pending);
  pthread_mutex_lock(&host_lock);
  memset(&host_link, 0, sizeof(host_link)); host_link.generation = 100;
  host_link.initialized = true;
  pthread_mutex_unlock(&host_lock);
  lease_generation = 0; seconds_now = 10;
  starts = stops = dhcp_starts = dhcp_stops = commits = aborts = leases = 0;
  net_steps = 0;
  maintains = 0; start_entered = false; finish_changes_link = false;
}
static void start_wait(void)
{
  assert(!glass_network_recovery_start()); wait_mailbox();
}
static void dhcp_ready(void)
{ pthread_mutex_lock(&host_lock); host_dhcp.ready = true; pthread_mutex_unlock(&host_lock); }
static uint64_t trial_floor(void)
{ pthread_mutex_lock(&g_recovery_lock); uint64_t n = g_recovery->trial_generation;
  pthread_mutex_unlock(&g_recovery_lock); return n; }
static void set_link(uint64_t generation, bool ready, uint64_t lease)
{
  pthread_mutex_lock(&host_lock); host_link.generation = generation;
  host_link.initialized = host_link.associated = host_link.carrier_ready = true;
  host_link.ipv4_ready = ready;
  uint8_t ip[] = {192, 168, 22, 7}; memcpy(host_link.ipv4, ip, 4);
  pthread_mutex_unlock(&host_lock); lease_generation = lease;
}

int main(void)
{
  assert(!glass_network_recovery_init()); reset_service();
  /* Mailbox acceptance is immediate while real worker owns blocked I/O. */
  block_start = true; assert(!glass_network_recovery_start());
  for (unsigned i = 0; !start_entered && i < 1000; i++) sleep_ms(1);
  assert(start_entered);
  struct glass_recovery_snapshot_s s = snapshot();
  assert(s.busy && s.state == GLASS_RECOVERY_STARTING && !s.dhcp_ready);
  assert(glass_network_recovery_start() == -EBUSY);
  assert(glass_wifi_service_submit(C6_DESKTOP_STATUS, NULL, NULL) == -EBUSY);
  char ssid[33], password[13], first_password[13];
  assert(!glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)));
  assert(strlen(password) == 12); strcpy(first_password, password);
  block_start = false; wait_mailbox();
  s = snapshot(); assert(s.state == GLASS_RECOVERY_STARTING && !s.dhcp_ready && s.ap_running);
  dhcp_ready(); wait_state(GLASS_RECOVERY_READY);
  assert(snapshot().dhcp_ready);
  reset_service();
  assert(glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)) == -ENOENT);
  start_wait();
  assert(!glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)));
  assert(strcmp(first_password, password));
  seconds_now = 19; wait_state(GLASS_RECOVERY_ERROR);
  s = snapshot(); assert(s.ap_error == -ETIMEDOUT && !s.ap_running && !s.dhcp_ready);

  /* Configuration failures clean resources; failed DHCP start still owns PID. */
  for (int step = 1; step <= 3; step++) {
    reset_service(); net_fail_step = step; start_wait(); s = snapshot();
    assert(s.state == GLASS_RECOVERY_ERROR && s.ap_error == -EIO && !s.ap_running);
    assert(!s.cleanup_error && !host_dhcp.running);
  }
  reset_service(); dhcp_start_error = -ENOMEM; start_wait(); s = snapshot();
  assert(s.state == GLASS_RECOVERY_ERROR && s.ap_error == -ENOMEM);
  assert(dhcp_starts == 1 && dhcp_stops == 1 && !s.ap_running);
  reset_service(); portal_error = -EADDRINUSE; start_wait();
  assert(snapshot().ap_error == -EADDRINUSE && dhcp_starts == 0);
  reset_service(); ap_start_error = -ETIMEDOUT; ap_stop_error = -EIO;
  start_wait(); s = snapshot();
  assert(s.ap_error == -ETIMEDOUT && s.cleanup_error == -EIO && s.ap_running);
  assert(!glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)));
  reset_service(); start_wait(); dhcp_ready(); wait_state(GLASS_RECOVERY_READY);
  pthread_mutex_lock(&host_lock); host_dhcp.ready = false; host_dhcp.last_error = -EIO;
  pthread_mutex_unlock(&host_lock); wait_state(GLASS_RECOVERY_ERROR);
  assert(snapshot().ap_error == -EIO && !snapshot().ap_running);

  /* A failed status read is not proof of OFF and must retain local secret. */
  reset_service(); start_wait();
  assert(!glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)));
  ap_status_error = -EIO;
  assert(!glass_network_recovery_stop()); wait_mailbox(); s = snapshot();
  assert(s.state == GLASS_RECOVERY_ERROR && s.ap_running && s.cleanup_error == -EIO);
  assert(!glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)));
  clear_errors(); assert(!glass_network_recovery_stop()); wait_mailbox();
  assert(!snapshot().ap_running);
  reset_service(); start_wait(); dhcp_stop_error = -ETIMEDOUT;
  unsigned before_stops = atomic_load(&stops);
  assert(!glass_network_recovery_stop()); wait_mailbox(); s = snapshot();
  assert(s.ap_running && s.cleanup_error == -ETIMEDOUT && stops == before_stops);
  assert(!glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)));
  reset_service(); start_wait(); ap_stop_error = -EIO;
  assert(!glass_network_recovery_stop()); wait_mailbox();
  assert(snapshot().ap_running && snapshot().cleanup_error == -EIO);
  /* P4 state can be empty while the backend reports a retained remote AP.
   * Its unknown state must remain visible even without a local password.
   */
  reset_service();
  pthread_mutex_lock(&host_lock); host_ap.active = false; host_ap.state_unknown = true;
  pthread_mutex_unlock(&host_lock); ap_stop_error = -ETIMEDOUT;
  assert(!glass_network_recovery_stop()); wait_mailbox(); s = snapshot();
  assert(s.state == GLASS_RECOVERY_ERROR && s.ap_running && s.cleanup_error == -ETIMEDOUT);
  assert(glass_network_recovery_credentials(ssid, sizeof(ssid), password, sizeof(password)) == -ENOENT);

  /* A current address must have both the trial generation and its DHCP lease. */
  reset_service();
  assert(glass_wifi_service_submit(C6_DESKTOP_CONNECT, "Unit", "short") == -EINVAL);
  assert(glass_wifi_service_submit(C6_DESKTOP_CONNECT, "bad\nSSID", "test-password") == -EINVAL);
  assert(!glass_wifi_service_submit(C6_DESKTOP_CONNECT, "Unit", "test-password")); wait_mailbox();
  uint64_t floor = trial_floor(); assert(floor > 100 && pending && leases == 1);
  set_link(floor - 1, true, floor - 1); sleep_ms(1100); assert(commits == 0);
  set_link(floor, true, floor - 1); sleep_ms(1100); assert(commits == 0);
  set_link(floor, true, floor); wait_not_joining(); assert(commits == 1 && !pending);
  sleep_ms(1100); assert(commits == 1);
  s = snapshot(); assert(s.ipv4_ready && !s.error);

  /* Commit error retains pending but is never replayed by maintenance. */
  reset_service(); assert(!glass_wifi_service_submit(C6_DESKTOP_CONNECT, "Unit", "test-password"));
  wait_mailbox(); floor = trial_floor(); trial_finish_error = -EIO;
  set_link(floor, true, floor); sleep_ms(2200);
  assert(commits == 1 && pending && snapshot().joining && snapshot().error == -EIO);
  clear_errors(); assert(!glass_network_recovery_stop()); wait_mailbox();
  assert(aborts == 1 && !pending);
  reset_service(); trial_begin_error = -EIO; trial_finish_error = -ETIMEDOUT;
  assert(!glass_wifi_service_submit(C6_DESKTOP_CONNECT, "Unit", "test-password")); wait_mailbox();
  s = snapshot(); assert(s.error == -EIO && s.cleanup_error == -ETIMEDOUT && s.joining);
  sleep_ms(1100); assert(aborts == 1); /* Explicit recovery required. */
  reset_service(); assert(!glass_wifi_service_submit(C6_DESKTOP_CONNECT, "Unit", "test-password"));
  wait_mailbox(); seconds_now = 111; wait_not_joining();
  assert(aborts == 1 && !commits && snapshot().error == -ETIMEDOUT);
  reset_service(); finish_changes_link = true;
  assert(!glass_wifi_service_submit(C6_DESKTOP_CONNECT, "Unit", "test-password")); wait_mailbox();
  floor = trial_floor(); set_link(floor, true, floor); wait_not_joining();
  assert(commits == 1 && !snapshot().ipv4_ready);

  /* Continuous public STATUS during a pending trial must not postpone the
   * worker's absolute maintenance tick. This operation is allowed while
   * joining, unlike requests during the AP STARTING transition.
   */
  reset_service();
  assert(!glass_wifi_service_submit(C6_DESKTOP_CONNECT, "Unit", "test-password"));
  wait_mailbox(); floor = trial_floor(); set_link(floor, true, floor);
  unsigned accepted_status = 0;
  for (unsigned i = 0; i < 2400; i++) {
    int ret = glass_wifi_service_submit(C6_DESKTOP_STATUS, NULL, NULL);
    assert(ret == 0 || ret == -EBUSY);
    if (!ret) accepted_status++;
    if (!snapshot().joining) break;
    sleep_ms(1);
  }
  assert(!snapshot().joining && commits == 1 && accepted_status > 50);
  /* Also stress the raw shared mailbox while AP start is awaiting DHCP. */
  reset_service(); start_wait(); dhcp_ready();
  for (unsigned i = 0; i < 2400; i++) {
    int ret = c6_desktop_worker_submit(&g_recovery->worker, C6_DESKTOP_STATUS, NULL, NULL);
    assert(ret == 0 || ret == -EBUSY);
    if (snapshot().state == GLASS_RECOVERY_READY) break;
    sleep_ms(1);
  }
  assert(snapshot().state == GLASS_RECOVERY_READY);
#ifdef CONFIG_NET_NAT44
  reset_service();start_wait();dhcp_ready();wait_state(GLASS_RECOVERY_READY);
  char old_ssid[33],old_password[13],new_ssid[33],new_password[13];
  assert(!glass_network_recovery_credentials(old_ssid,sizeof(old_ssid),old_password,sizeof(old_password)));
  wired_ip=htonl(0xc0a80432u);
  for(unsigned i=0;i<4000&&starts<2;i++)sleep_ms(1);
  assert(starts==2);wait_mailbox();dhcp_ready();wait_state(GLASS_RECOVERY_READY);
  assert(!glass_network_recovery_credentials(new_ssid,sizeof(new_ssid),new_password,sizeof(new_password)));
  assert(!strcmp(old_ssid,new_ssid)&&!strcmp(old_password,new_password));
  assert(snapshot().ap_ipv4[2]==42);
  wired_ip=0;
  puts("NETWORK_RECOVERY_SHARING_OK: upstream overlap restarts same SSID/key on disjoint subnet");
#endif
  reset_service(); random_error = EIO;
  assert(glass_network_recovery_start() == -EIO && starts == 0);
  clear_errors(); wait_mailbox();
  assert(!c6_desktop_worker_stop(&g_recovery->worker));
  free(g_recovery); g_recovery = NULL;
  assert(owner_set);
  puts("NETWORK_RECOVERY_OK: real service + persistent worker; readiness, ownership, every cleanup failure, private credentials, generation/DHCP commit, no replay, polling fairness");
  return 0;
}
