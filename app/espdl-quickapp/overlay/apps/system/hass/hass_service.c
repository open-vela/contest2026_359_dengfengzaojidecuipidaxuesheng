/* SPDX-License-Identifier: Apache-2.0 */
#include "hass_service.h"
#include "hass_transport.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef HASS_CACHE_TTL_MS
#  define HASS_CACHE_TTL_MS 10000
#endif

enum request_kind_e
{
  REQUEST_NONE = 0,
  REQUEST_OTHER,
  REQUEST_STATES,
  REQUEST_CONTROL
};

struct client_s
{
  uint32_t id;
  unsigned grants;
  struct hass_result_s result;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct client_s g_clients[HASS_MAX_CLIENTS];
static uint32_t g_next_client = 1, g_next_request = 1;
static uint32_t g_owner, g_request;
static enum request_kind_e g_kind;
static char g_url[128], g_token[512];
static char *g_states_cache;
static uint64_t g_cache_at;

static void wipe(void *data, size_t size)
{
  volatile unsigned char *p = data;
  while (size--) *p++ = 0;
}

void hass_result_free(struct hass_result_s *result)
{
  if (!result) return;
  if (result->body) { wipe(result->body, strlen(result->body)); free(result->body); }
  memset(result, 0, sizeof(*result));
}

static struct client_s *lookup(uint32_t id)
{
  for (unsigned i = 0; i < HASS_MAX_CLIENTS; i++)
    if (id && g_clients[i].id == id) return &g_clients[i];
  return NULL;
}

static uint64_t now_ms(void)
{
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) return 0;
  return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static void cache_clear(void)
{
  if (g_states_cache)
    {
      wipe(g_states_cache, strlen(g_states_cache));
      free(g_states_cache);
    }
  g_states_cache = NULL;
  g_cache_at = 0;
}

static bool cache_fresh(uint64_t *age)
{
  if (!g_states_cache || !g_cache_at) return false;
  uint64_t current = now_ms();
  if (!current || current < g_cache_at) return false;
  uint64_t elapsed = current - g_cache_at;
  if (age) *age = elapsed;
  return elapsed <= HASS_CACHE_TTL_MS;
}

/* Only this service consumes its private transport's result. */
static void pump(void)
{
  if (!g_owner) return;
  struct hass_transport_result_s wire;
  hass_transport_poll(&wire);
  if (wire.busy) return;
  struct client_s *client = lookup(g_owner);
  if (client && wire.done)
    {
      if (g_kind == REQUEST_STATES && wire.error == 0 &&
          wire.status >= 200 && wire.status < 300 && wire.body)
        {
          char *copy = strdup(wire.body);
          if (copy)
            {
              cache_clear();
              g_states_cache = copy;
              g_cache_at = now_ms();
            }
        }
      client->result = (struct hass_result_s){g_request, false, true, false,
                                            wire.status, wire.error, wire.body};
      wire.body = NULL;
      if (wire.status == 401 || wire.status == 403)
        {
          wipe(g_token, sizeof(g_token));
          cache_clear();
        }
    }
  if (wire.body) { wipe(wire.body, strlen(wire.body)); free(wire.body); }
  g_owner = g_request = 0;
  g_kind = REQUEST_NONE;
}

static bool http_url(const char *url)
{
  if (!url || strlen(url) >= sizeof(g_url) || strncmp(url, "http://", 7)) return false;
  const char *p = url + 7;
  size_t n = strcspn(p, ":/");
  if (!n || n >= sizeof(g_url) - 7) return false;
  for (size_t i = 0; i < n; i++)
    if (!((p[i] >= 'a' && p[i] <= 'z') ||
          (p[i] >= 'A' && p[i] <= 'Z') ||
          (p[i] >= '0' && p[i] <= '9') ||
          p[i] == '.' || p[i] == '-' || p[i] == '_')) return false;
  p += n;
  if (*p == ':')
    {
      unsigned port = 0;
      const char *start = ++p;
      while (*p >= '0' && *p <= '9')
        { port = port * 10 + (*p++ - '0'); if (port > 65535) return false; }
      if (p == start || !port) return false;
    }
  if (*p && strcmp(p, "/")) return false;
  return true;
}

static bool entity_valid(const char *entity)
{
  if (!entity || strlen(entity) >= 96 || entity[0] < 'a' || entity[0] > 'z') return false;
  unsigned dots = 0;
  for (const char *p = entity; *p; p++)
    {
      if (*p == '.') { if (p == entity || !p[1] || ++dots > 1) return false; }
      else if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    }
  return dots == 1;
}

int hass_open(unsigned grants)
{
  if (!(grants & HASS_READ) || (grants & ~7u)) return -EINVAL;
  pthread_mutex_lock(&g_lock);
  int ret = -EMFILE;
  if (g_next_client > INT_MAX) ret = -EOVERFLOW;
  else for (unsigned i = 0; i < HASS_MAX_CLIENTS; i++)
    if (!g_clients[i].id)
      { ret = (int)g_next_client++; g_clients[i].id = ret; g_clients[i].grants = grants; break; }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int hass_close(uint32_t id)
{
  pthread_mutex_lock(&g_lock);
  struct client_s *client = lookup(id);
  int ret = client ? 0 : -EBADF;
  if (client)
    {
      if (g_owner == id) hass_transport_stop();
      hass_result_free(&client->result);
      memset(client, 0, sizeof(*client));
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int hass_configure(uint32_t id, const char *url, const char *token, bool allow)
{
  if (!allow) return -EACCES;
  if (!http_url(url) || !token || !*token || strlen(token) >= sizeof(g_token)) return -EINVAL;
  for (const unsigned char *p = (const unsigned char *)token; *p; p++)
    if (*p < 33 || *p > 126) return -EINVAL;
  pthread_mutex_lock(&g_lock);
  pump();
  struct client_s *client = lookup(id);
  int ret = !client ? -EBADF : !(client->grants & HASS_CONFIGURE) ? -EACCES : 0;
  if (!ret)
    {
      uint32_t owner = g_owner;
      uint32_t request = g_request;
      if (owner) hass_transport_stop();
      for (unsigned i = 0; i < HASS_MAX_CLIENTS; i++)
        hass_result_free(&g_clients[i].result);
      struct client_s *interrupted = lookup(owner);
      if (interrupted)
        interrupted->result = (struct hass_result_s)
          {request, false, true, false, 0, -ECANCELED, NULL};
      g_owner = g_request = 0;
      g_kind = REQUEST_NONE;
      cache_clear();
      wipe(g_token, sizeof(g_token));
      strcpy(g_url, url);
      strcpy(g_token, token);
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int hass_clear_configuration(uint32_t id)
{
  pthread_mutex_lock(&g_lock);
  struct client_s *client = lookup(id);
  int ret = !client ? -EBADF : !(client->grants & HASS_CONFIGURE) ? -EACCES : 0;
  if (!ret)
    {
      if (g_owner) hass_transport_stop();
      wipe(g_token, sizeof(g_token)); wipe(g_url, sizeof(g_url));
      cache_clear();
      for (unsigned i = 0; i < HASS_MAX_CLIENTS; i++) hass_result_free(&g_clients[i].result);
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

static int submit(uint32_t id, const char *resource, const char *entity, bool control,
                  bool on, int brightness)
{
  pthread_mutex_lock(&g_lock);
  pump();
  struct client_s *client = lookup(id);
  int ret = !client ? -EBADF : control && !(client->grants & HASS_CONTROL) ? -EACCES : 0;
  if (!ret && !g_token[0]) ret = -ENOTCONN;
  if (!ret && client->result.done) ret = -EBUSY;
  if (!ret && g_next_request > INT_MAX) ret = -EOVERFLOW;
  if (!ret && entity && !entity_valid(entity)) ret = -EINVAL;
  bool states = !control && !entity && resource && !strcmp(resource, "states");
  if (!ret && states && cache_fresh(NULL))
    {
      char *copy = strdup(g_states_cache);
      if (!copy) ret = -ENOMEM;
      else
        {
          uint32_t request = g_next_request++;
          client->result = (struct hass_result_s){request, false, true, true,
                                                 200, 0, copy};
          pthread_mutex_unlock(&g_lock);
          return (int)request;
        }
    }
  if (!ret && g_owner) ret = -EBUSY;
  if (!ret && control)
    {
      char domain[96], body[192];
      size_t n = (size_t)(strchr(entity, '.') - entity);
      memcpy(domain, entity, n); domain[n] = 0;
      if (strcmp(domain, "light") && strcmp(domain, "switch") &&
          strcmp(domain, "input_boolean") && strcmp(domain, "fan")) ret = -EACCES;
      else if (brightness < -1 || brightness > 100 || brightness == 0 ||
               (brightness != -1 && (!on || strcmp(domain, "light")))) ret = -EINVAL;
      else
        {
          if (brightness == -1) snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity);
          else snprintf(body, sizeof(body), "{\"entity_id\":\"%s\",\"brightness_pct\":%d}", entity, brightness);
          ret = hass_transport_call_service(g_url, g_token, domain,
                                            on ? "turn_on" : "turn_off", body);
          if (ret == 0) cache_clear();
        }
    }
  else if (!ret && entity) ret = hass_transport_get_state(g_url, g_token, entity);
  else if (!ret) ret = hass_transport_get(g_url, g_token, resource);
  if (!ret)
    {
      g_owner = id;
      g_request = g_next_request++;
      g_kind = control ? REQUEST_CONTROL : states ? REQUEST_STATES : REQUEST_OTHER;
      ret = (int)g_request;
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int hass_get(uint32_t id, const char *resource) { return submit(id, resource, NULL, false, false, -1); }
int hass_get_state(uint32_t id, const char *entity)
{ return entity ? submit(id, NULL, entity, false, false, -1) : -EINVAL; }
int hass_control(uint32_t id, const char *entity, bool on, int brightness)
{ return entity ? submit(id, NULL, entity, true, on, brightness) : -EINVAL; }

int hass_poll(uint32_t id, struct hass_result_s *result)
{
  if (!result) return -EINVAL;
  memset(result, 0, sizeof(*result));
  pthread_mutex_lock(&g_lock);
  pump();
  struct client_s *client = lookup(id);
  int ret = client ? 0 : -EBADF;
  if (client)
    {
      *result = client->result;
      memset(&client->result, 0, sizeof(client->result));
      if (g_owner == id) { result->busy = true; result->request_id = g_request; }
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}

void hass_status(bool *configured, bool *busy, unsigned *clients)
{
  pthread_mutex_lock(&g_lock);
  pump();
  unsigned count = 0;
  for (unsigned i = 0; i < HASS_MAX_CLIENTS; i++) if (g_clients[i].id) count++;
  if (configured) *configured = g_token[0] != 0;
  if (busy) *busy = g_owner != 0;
  if (clients) *clients = count;
  pthread_mutex_unlock(&g_lock);
}

int hass_get_url(uint32_t id, char *url, size_t size)
{
  if (!url || !size) return -EINVAL;
  url[0] = 0;
  pthread_mutex_lock(&g_lock);
  int ret = !lookup(id) ? -EBADF : !g_token[0] ? -ENOTCONN : 0;
  if (!ret && strlen(g_url) >= size) ret = -ENOSPC;
  if (!ret) strcpy(url, g_url);
  pthread_mutex_unlock(&g_lock);
  return ret;
}

int hass_cache_info(uint32_t id, bool *available, uint32_t *age_ms)
{
  pthread_mutex_lock(&g_lock);
  int ret = lookup(id) ? 0 : -EBADF;
  if (!ret)
    {
      uint64_t age = 0;
      bool fresh = cache_fresh(&age);
      if (available) *available = fresh;
      if (age_ms) *age_ms = fresh && age <= UINT32_MAX ? (uint32_t)age : 0;
    }
  pthread_mutex_unlock(&g_lock);
  return ret;
}
