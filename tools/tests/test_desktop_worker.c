/* SPDX-License-Identifier: Apache-2.0 */
#include "../c6/desktop_worker.h"
#include <assert.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

static void check_idle_cpu(struct c6_desktop_worker *worker)
{
  clockid_t clock;
  struct timespec before, after, delay = {2, 0};
  assert(!pthread_getcpuclockid(worker->thread, &clock));
  assert(!clock_gettime(clock, &before));
  while (nanosleep(&delay, &delay) < 0) assert(errno == EINTR);
  assert(!clock_gettime(clock, &after));
  long long ns = (long long)(after.tv_sec - before.tv_sec) * 1000000000LL +
                 after.tv_nsec - before.tv_nsec;
  printf("Wi-Fi idle worker: %lld ns CPU over >=2 s wall time; %.6f%% of one host core\n",
         ns, (double)ns / 20000000.0);
  /* Allow scheduler/sanitizer noise, but reject a spinning idle worker. */
  assert(ns >= 0 && ns < 50000000LL);
}

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static bool entered, release_scan;
static int scan_error;
static int status_error;
static bool disconnected;
static struct c6_desktop_link observed;

static int status(struct c6_desktop_link *link)
{
  *link = observed;
  return status_error;
}

static int disconnect_ap(void)
{
  disconnected = true;
  memset(&observed, 0, sizeof(observed));
  return 0;
}

static int scan(struct c6_scan_ap *records, size_t capacity, size_t *count)
{
  assert(capacity == C6_SCAN_LIMIT);
  pthread_mutex_lock(&gate);
  entered = true;
  pthread_cond_broadcast(&wake);
  while (!release_scan) pthread_cond_wait(&wake, &gate);
  int error = scan_error;
  pthread_mutex_unlock(&gate);
  *count = 1;
  assert(c6_scan_ap_set(records, (const uint8_t *)"test", 4, -50, 6) == 0);
  return error;
}

static int connect_ap(const char *ssid, const char *password)
{
  assert(!strcmp(ssid, "test"));
  assert(!strcmp(password, "fixture-only"));
  return -ETIMEDOUT;
}

static struct c6_desktop_result wait_result(struct c6_desktop_worker *w)
{
  struct c6_desktop_result r;
  for (int i = 0; i < 1000; i++)
    {
      assert(!c6_desktop_worker_read(w, &r));
      if (!r.busy) return r;
      struct timespec delay = {0, 1000000};
      nanosleep(&delay, NULL);
    }
  assert(false);
  return r;
}

int main(void)
{
  struct c6_desktop_worker w;
  const struct c6_desktop_backend backend = {
    .scan = scan, .connect = connect_ap
  };
  assert(!c6_desktop_worker_start(&w, &backend));
  assert(c6_desktop_worker_submit(&w, C6_DESKTOP_CONNECT, "", "") == -EINVAL);
  assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_SCAN, NULL, NULL));
  pthread_mutex_lock(&gate);
  while (!entered) pthread_cond_wait(&wake, &gate);
  assert(c6_desktop_worker_submit(&w, C6_DESKTOP_SCAN, NULL, NULL) == -EBUSY);
  release_scan = true;
  pthread_cond_broadcast(&wake);
  pthread_mutex_unlock(&gate);
  struct c6_desktop_result r = wait_result(&w);
  assert(!r.error && r.count == 1 && r.sequence == 1);
  assert(!memcmp(r.records[0].ssid, "test", 4));
  assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_CONNECT, "test", "fixture-only"));
  r = wait_result(&w);
  assert(r.error == -ETIMEDOUT && r.count == 0 && r.sequence == 2);
  pthread_mutex_lock(&w.lock);
  for (size_t i = 0; i < sizeof(w.password); i++) assert(w.password[i] == 0);
  pthread_mutex_unlock(&w.lock);
  pthread_mutex_lock(&gate);
  scan_error = -EIO;
  pthread_mutex_unlock(&gate);
  assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_SCAN, NULL, NULL));
  r = wait_result(&w);
  assert(r.error == -EIO && r.count == 0 && r.records[0].ssid_length == 0);
  assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_STATUS, NULL, NULL));
  assert(wait_result(&w).error == -ENOTSUP);
  assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_DISCONNECT, NULL, NULL));
  assert(wait_result(&w).error == -ENOTSUP);
  assert(!c6_desktop_worker_stop(&w));
  for (size_t i = 0; i < sizeof(w.password); i++) assert(w.password[i] == 0);
  const struct c6_desktop_backend lifecycle = {
    .scan = scan, .connect = connect_ap,
    .status = status, .disconnect = disconnect_ap
  };
  assert(!c6_desktop_worker_start(&w, &lifecycle));
  for (int cycle = 0; cycle < 100; cycle++)
    {
      observed = (struct c6_desktop_link){true, true, true, true, {192, 0, 2, 1}};
      assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_STATUS, NULL, NULL));
      r = wait_result(&w);
      assert(!r.error && r.link.ipv4_ready && r.link.ipv4[3] == 1);
      status_error = -EIO;
      assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_STATUS, NULL, NULL));
      r = wait_result(&w);
      assert(r.error == -EIO && !r.link.ipv4_ready && r.link.ipv4[3] == 0);
      status_error = 0;
      observed.associated = false;
      assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_STATUS, NULL, NULL));
      assert(wait_result(&w).error == -EPROTO);
      assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_DISCONNECT, NULL, NULL));
      assert(!wait_result(&w).error && disconnected);
      assert(!c6_desktop_worker_submit(&w, C6_DESKTOP_STATUS, NULL, NULL));
      r = wait_result(&w);
      assert(!r.error && !r.link.associated && !r.link.ipv4_ready);
    }
  check_idle_cpu(&w);
  assert(!c6_desktop_worker_stop(&w));
  return 0;
}
