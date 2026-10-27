/* SPDX-License-Identifier: Apache-2.0 */
#include "desktop_worker.h"
#include <limits.h>
#include <string.h>
#include <time.h>

#ifndef CONFIG_SYSTEM_C6_DESKTOP_STACKSIZE
#define CONFIG_SYSTEM_C6_DESKTOP_STACKSIZE 8192
#endif

static void wipe(void *data, size_t length)
{
  volatile unsigned char *p = data;
  while (length--) *p++ = 0;
}

static void *run(void *arg)
{
  struct c6_desktop_worker *w = arg;
  struct timespec next_maintain = {0};
  if (w->backend.maintain)
    { clock_gettime(CLOCK_MONOTONIC, &next_maintain); next_maintain.tv_sec++; }
  pthread_mutex_lock(&w->lock);
  for (;;)
    {
      if (w->backend.maintain && !w->stopping)
        {
          struct timespec now;
          clock_gettime(CLOCK_MONOTONIC, &now);
          if (now.tv_sec > next_maintain.tv_sec ||
              (now.tv_sec == next_maintain.tv_sec && now.tv_nsec >= next_maintain.tv_nsec))
            {
              pthread_mutex_unlock(&w->lock);
              w->backend.maintain();
              pthread_mutex_lock(&w->lock);
              clock_gettime(CLOCK_MONOTONIC, &next_maintain);
              next_maintain.tv_sec++;
            }
        }
      while (!w->queued && !w->stopping)
        {
          if (!w->backend.maintain)
            pthread_cond_wait(&w->wake, &w->lock);
          else
            {
              int waited = pthread_cond_timedwait(&w->wake, &w->lock, &next_maintain);
              if (waited == ETIMEDOUT) break;
              if (waited && waited != EINTR)
                { w->result.error = -waited; w->stopping = true; }
            }
        }
      if (w->stopping) break;
      if (!w->queued) continue;
      struct c6_desktop_result result = {0};
      char ssid[33];
      char password[65];
      result.sequence = w->result.sequence;
      result.operation = w->result.operation;
      memcpy(ssid, w->ssid, sizeof(ssid));
      memcpy(password, w->password, sizeof(password));
      wipe(w->ssid, sizeof(w->ssid));
      wipe(w->password, sizeof(w->password));
      w->queued = false;
      pthread_mutex_unlock(&w->lock);

      if (result.operation == C6_DESKTOP_SCAN)
        {
          result.error = w->backend.scan(result.records, C6_SCAN_LIMIT,
                                         &result.count);
          if (!result.error && result.count > C6_SCAN_LIMIT)
            result.error = -EOVERFLOW;
          if (!result.error)
            for (size_t i = 0; i < result.count; i++)
              if (result.records[i].ssid_length > 32) result.error = -EPROTO;
          if (result.error)
            {
              result.count = 0;
              memset(result.records, 0, sizeof(result.records));
            }
        }
      else if (result.operation == C6_DESKTOP_CONNECT)
        result.error = w->backend.connect(ssid, password);
      else if (result.operation == C6_DESKTOP_DISCONNECT)
        result.error = w->backend.disconnect ? w->backend.disconnect() : -ENOTSUP;
      else if (result.operation == C6_DESKTOP_SOFTAP_START)
        result.error = w->backend.softap_start ?
          w->backend.softap_start(ssid, password) : -ENOTSUP;
      else if (result.operation == C6_DESKTOP_SOFTAP_STOP)
        result.error = w->backend.softap_stop ? w->backend.softap_stop() : -ENOTSUP;
      else
        {
          result.error = w->backend.status ?
            w->backend.status(&result.link) : -ENOTSUP;
          if (!result.error &&
              ((result.link.associated && !result.link.initialized) ||
               (result.link.carrier_ready && !result.link.associated) ||
               (result.link.ipv4_ready && !result.link.carrier_ready)))
            result.error = -EPROTO;
          if (result.error) memset(&result.link, 0, sizeof(result.link));
          else if (!result.link.ipv4_ready)
            memset(result.link.ipv4, 0, sizeof(result.link.ipv4));
        }
      wipe(password, sizeof(password));
      wipe(ssid, sizeof(ssid));
      pthread_mutex_lock(&w->lock);
      w->result = result;
    }
  wipe(w->password, sizeof(w->password));
  wipe(w->ssid, sizeof(w->ssid));
  w->queued = false;
  w->result.busy = false;
  pthread_mutex_unlock(&w->lock);
  return NULL;
}

int c6_desktop_worker_start(struct c6_desktop_worker *w,
                            const struct c6_desktop_backend *backend)
{
  if (!w || !backend || !backend->scan || !backend->connect) return -EINVAL;
  memset(w, 0, sizeof(*w));
  w->backend = *backend;
  int ret = pthread_mutex_init(&w->lock, NULL);
  if (ret) return -ret;
  pthread_condattr_t condition;
  ret = pthread_condattr_init(&condition);
  if (ret) { pthread_mutex_destroy(&w->lock); return -ret; }
  if (backend->maintain)
    ret = pthread_condattr_setclock(&condition, CLOCK_MONOTONIC);
  if (!ret) ret = pthread_cond_init(&w->wake, &condition);
  pthread_condattr_destroy(&condition);
  if (ret) { pthread_mutex_destroy(&w->lock); return -ret; }
  pthread_attr_t attr;
  ret = pthread_attr_init(&attr);
  if (!ret)
    {
      size_t stack = CONFIG_SYSTEM_C6_DESKTOP_STACKSIZE;
#ifdef PTHREAD_STACK_MIN
      if (stack < PTHREAD_STACK_MIN) stack = PTHREAD_STACK_MIN;
#endif
      ret = pthread_attr_setstacksize(&attr, stack);
      if (!ret) ret = pthread_create(&w->thread, &attr, run, w);
      pthread_attr_destroy(&attr);
    }
  if (ret)
    {
      pthread_cond_destroy(&w->wake);
      pthread_mutex_destroy(&w->lock);
    }
  return -ret;
}

int c6_desktop_worker_submit(struct c6_desktop_worker *w,
                             enum c6_desktop_operation operation,
                             const char *ssid, const char *password)
{
  if (!w || operation < C6_DESKTOP_SCAN || operation > C6_DESKTOP_SOFTAP_STOP)
    return -EINVAL;
  size_t slen = 0, plen = 0;
  if (operation == C6_DESKTOP_CONNECT || operation == C6_DESKTOP_SOFTAP_START)
    {
      if (!ssid || !password) return -EINVAL;
      slen = strnlen(ssid, 33);
      plen = strnlen(password, 65);
      if (!slen || slen > 32 || plen > 64) return -EINVAL;
    }
  int ret = pthread_mutex_lock(&w->lock);
  if (ret) return -ret;
  if (w->stopping) ret = -ESHUTDOWN;
  else if (w->result.busy) ret = -EBUSY;
  else if (w->result.sequence == ULONG_MAX) ret = -EOVERFLOW;
  else
    {
      unsigned long sequence = w->result.sequence + 1;
      memset(&w->result, 0, sizeof(w->result));
      w->result.sequence = sequence;
      w->result.operation = operation;
      w->result.busy = true;
      if (slen) memcpy(w->ssid, ssid, slen + 1);
      if (plen) memcpy(w->password, password, plen + 1);
      w->queued = true;
      ret = pthread_cond_signal(&w->wake);
      if (ret)
        {
          w->queued = w->result.busy = false;
          wipe(w->password, sizeof(w->password));
          wipe(w->ssid, sizeof(w->ssid));
          ret = -ret;
        }
    }
  pthread_mutex_unlock(&w->lock);
  return ret;
}

int c6_desktop_worker_read(struct c6_desktop_worker *w,
                           struct c6_desktop_result *result)
{
  if (!w || !result) return -EINVAL;
  int ret = pthread_mutex_lock(&w->lock);
  if (ret) return -ret;
  *result = w->result;
  pthread_mutex_unlock(&w->lock);
  return 0;
}

int c6_desktop_worker_stop(struct c6_desktop_worker *w)
{
  if (!w) return -EINVAL;
  int ret = pthread_mutex_lock(&w->lock);
  if (ret) return -ret;
  w->stopping = true;
  pthread_cond_signal(&w->wake);
  pthread_mutex_unlock(&w->lock);
  ret = pthread_join(w->thread, NULL);
  if (ret) return -ret;
  pthread_cond_destroy(&w->wake);
  pthread_mutex_destroy(&w->lock);
  return 0;
}
