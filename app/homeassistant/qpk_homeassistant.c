/****************************************************************************
 * apps/system/desktop/qpk_homeassistant.c
 *
 * SPDX-License-Identifier: Apache-2.0
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "qpk_homeassistant.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HA_HOST_MAX 128
#define HA_PATH_MAX 256
#define HA_TOKEN_MAX 512
#define HA_BODY_MAX 2048
#define HA_REQUEST_MAX 4096
#define HA_RESPONSE_MAX 65536

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ha_request_s
{
  char host[HA_HOST_MAX];
  char path[HA_PATH_MAX];
  char token[HA_TOKEN_MAX];
  char body[HA_BODY_MAX];
  int port;
  bool post;
};

struct ha_state_s
{
  pthread_mutex_t lock;
  pthread_t thread;
  bool initialized;
  bool running;
  bool done;
  int status;
  int error;
  int socket;
  char *response;
  struct ha_request_s request;
};

static struct ha_state_s g_ha;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static bool ha_slug_valid(const char *text, size_t len, bool is_domain)
{
  size_t i;
  unsigned char c;

  if (len == 0 || text[0] == '_' || text[len - 1] == '_')
    {
      return false;
    }

  for (i = 0; i < len; i++)
    {
      c = (unsigned char)text[i];
      if (is_domain && c == '_' && i + 1 < len && text[i + 1] == '_')
        {
          return false;
        }

      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
        {
          return false;
        }
    }

  return true;
}

static bool ha_component_valid(const char *text, bool dot)
{
  const char *sep;
  size_t len;

  if (text == NULL || text[0] == '\0')
    {
      return false;
    }

  len = strlen(text);
  if (len >= 96)
    {
      return false;
    }

  if (!dot)
    {
      return ha_slug_valid(text, len, true);
    }

  sep = strchr(text, '.');
  if (sep == NULL || strchr(sep + 1, '.') != NULL)
    {
      return false;
    }

  return ha_slug_valid(text, (size_t)(sep - text), true) &&
         ha_slug_valid(sep + 1, strlen(sep + 1), false);
}

static int ha_parse_url(const char *url, char *host, size_t host_size,
                        int *port)
{
  const char *start;
  const char *end;
  const char *colon;
  size_t length;

  if (url == NULL || strncmp(url, "http://", 7) != 0)
    {
      return -EPROTONOSUPPORT;
    }

  start = url + 7;
  end = strchr(start, '/');
  if (end == NULL)
    {
      end = start + strlen(start);
    }

  colon = memchr(start, ':', end - start);
  length = (colon == NULL ? end : colon) - start;
  if (length == 0 || length >= host_size)
    {
      return -EINVAL;
    }

  memcpy(host, start, length);
  host[length] = '\0';
  for (length = 0; host[length] != '\0'; length++)
    {
      if (!isalnum((unsigned char)host[length]) && host[length] != '.' &&
          host[length] != '-' && host[length] != '_')
        {
          return -EINVAL;
        }
    }

  *port = colon == NULL ? 8123 : atoi(colon + 1);
  return *port > 0 && *port <= 65535 ? OK : -EINVAL;
}

static int ha_send_all(int fd, const char *data, size_t length)
{
  ssize_t sent;

  while (length > 0)
    {
      sent = send(fd, data, length, 0);
      if (sent <= 0)
        {
          return -errno;
        }

      data += sent;
      length -= sent;
    }

  return OK;
}

static void ha_close_socket(int fd)
{
  pthread_mutex_lock(&g_ha.lock);
  if (g_ha.socket == fd)
    {
      g_ha.socket = -1;
    }

  pthread_mutex_unlock(&g_ha.lock);
  close(fd);
}

static int ha_decode_chunked(char *body, size_t *length)
{
  char *input = body;
  char *output = body;
  char *end = body + *length;

  while (input < end)
    {
      char *line_end = strstr(input, "\r\n");
      unsigned long chunk;

      if (line_end == NULL)
        {
          return -EPROTO;
        }

      *line_end = '\0';
      chunk = strtoul(input, NULL, 16);
      input = line_end + 2;
      if (chunk == 0)
        {
          *length = output - body;
          body[*length] = '\0';
          return OK;
        }

      if (chunk > (unsigned long)(end - input) || output + chunk > end)
        {
          return -EOVERFLOW;
        }

      memmove(output, input, chunk);
      output += chunk;
      input += chunk;
      if (input + 2 > end || input[0] != '\r' || input[1] != '\n')
        {
          return -EPROTO;
        }

      input += 2;
    }

  return -EPROTO;
}

static int ha_request_run(struct ha_request_s *request, int *status,
                          char *response, size_t response_size)
{
  struct addrinfo hints;
  struct addrinfo *addresses = NULL;
  struct addrinfo *address;
  struct timeval timeout;
  char port_text[8];
  char message[HA_REQUEST_MAX];
  char *headers;
  char *body;
  size_t used = 0;
  size_t body_length;
  ssize_t received;
  int fd = -1;
  int ret;

  memset(&hints, 0, sizeof(hints));
  timeout.tv_sec = 5;
  timeout.tv_usec = 0;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port_text, sizeof(port_text), "%d", request->port);
  ret = getaddrinfo(request->host, port_text, &hints, &addresses);
  if (ret != 0)
    {
      return -EHOSTUNREACH;
    }

  for (address = addresses; address != NULL; address = address->ai_next)
    {
      fd = socket(address->ai_family, address->ai_socktype,
                  address->ai_protocol);
      if (fd < 0)
        {
          continue;
        }

      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
        {
          pthread_mutex_lock(&g_ha.lock);
          g_ha.socket = fd;
          pthread_mutex_unlock(&g_ha.lock);
          break;
        }

      close(fd);
      fd = -1;
    }

  freeaddrinfo(addresses);
  if (fd < 0)
    {
      return -ECONNREFUSED;
    }

  if (request->post)
    {
      ret = snprintf(message, sizeof(message),
                     "POST %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Authorization: Bearer %s\r\n"
                     "Accept: application/json\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %lu\r\nConnection: close\r\n\r\n%s",
                     request->path, request->host, request->port,
                     request->token,
                     (unsigned long)strlen(request->body), request->body);
    }
  else
    {
      ret = snprintf(message, sizeof(message),
                     "GET %s HTTP/1.1\r\nHost: %s:%d\r\n"
                     "Authorization: Bearer %s\r\n"
                     "Accept: application/json\r\nConnection: close\r\n\r\n",
                     request->path, request->host, request->port,
                     request->token);
    }

  if (ret < 0 || ret >= sizeof(message))
    {
      ha_close_socket(fd);
      return -E2BIG;
    }

  ret = ha_send_all(fd, message, ret);
  if (ret < 0)
    {
      ha_close_socket(fd);
      return ret;
    }

  while (used + 1 < response_size)
    {
      received = recv(fd, response + used, response_size - used - 1, 0);
      if (received == 0)
        {
          break;
        }

      if (received < 0)
        {
          ha_close_socket(fd);
          return -errno;
        }

      used += received;
    }

  ha_close_socket(fd);
  response[used] = '\0';
  if (used + 1 == response_size)
    {
      return -EOVERFLOW;
    }

  if (sscanf(response, "HTTP/%*u.%*u %d", status) != 1)
    {
      return -EPROTO;
    }

  headers = strstr(response, "\r\n\r\n");
  if (headers == NULL)
    {
      return -EPROTO;
    }

  body = headers + 4;
  body_length = used - (body - response);
  if (strcasestr(response, "transfer-encoding: chunked") != NULL)
    {
      ret = ha_decode_chunked(body, &body_length);
      if (ret < 0)
        {
          return ret;
        }
    }

  memmove(response, body, body_length);
  response[body_length] = '\0';
  return OK;
}

static void *ha_worker(pthread_addr_t arg)
{
  struct ha_request_s request;
  char *response;
  int status = 0;
  int ret;

  (void)arg;
  pthread_mutex_lock(&g_ha.lock);
  request = g_ha.request;
  pthread_mutex_unlock(&g_ha.lock);
  response = malloc(HA_RESPONSE_MAX);
  ret = response == NULL ? -ENOMEM :
        ha_request_run(&request, &status, response, HA_RESPONSE_MAX);

  pthread_mutex_lock(&g_ha.lock);
  g_ha.error = ret;
  g_ha.status = status;
  free(g_ha.response);
  g_ha.response = ret < 0 ? NULL : response;
  if (ret < 0)
    {
      free(response);
    }

  memset(request.token, 0, sizeof(request.token));
  memset(g_ha.request.token, 0, sizeof(g_ha.request.token));
  g_ha.running = false;
  g_ha.done = true;
  pthread_mutex_unlock(&g_ha.lock);
  return NULL;
}

static int ha_start(const char *base_url, const char *token,
                    const char *path, const char *body)
{
  pthread_attr_t attr;
  bool attr_initialized = false;
  int ret;

  if (token == NULL || token[0] == '\0' || strlen(token) >= HA_TOKEN_MAX ||
      strchr(token, '\r') != NULL || strchr(token, '\n') != NULL)
    {
      return -EINVAL;
    }

  if (!g_ha.initialized)
    {
      ret = pthread_mutex_init(&g_ha.lock, NULL);
      if (ret != 0)
        {
          return -ret;
        }

      g_ha.initialized = true;
      g_ha.socket = -1;
    }

  pthread_mutex_lock(&g_ha.lock);
  if (g_ha.running)
    {
      pthread_mutex_unlock(&g_ha.lock);
      return -EBUSY;
    }

  memset(&g_ha.request, 0, sizeof(g_ha.request));
  ret = ha_parse_url(base_url, g_ha.request.host,
                     sizeof(g_ha.request.host), &g_ha.request.port);
  if (ret < 0 || strlcpy(g_ha.request.path, path,
                         sizeof(g_ha.request.path)) >=
                 sizeof(g_ha.request.path))
    {
      pthread_mutex_unlock(&g_ha.lock);
      return ret < 0 ? ret : -E2BIG;
    }

  strlcpy(g_ha.request.token, token, sizeof(g_ha.request.token));
  if (body != NULL)
    {
      strlcpy(g_ha.request.body, body, sizeof(g_ha.request.body));
      g_ha.request.post = true;
    }

  g_ha.done = false;
  g_ha.status = 0;
  g_ha.error = 0;
  free(g_ha.response);
  g_ha.response = NULL;
  g_ha.running = true;
  pthread_mutex_unlock(&g_ha.lock);

  ret = pthread_attr_init(&attr);
  if (ret == 0)
    {
      attr_initialized = true;
      ret = pthread_attr_setstacksize(&attr, 12288);
    }

  if (ret == 0)
    {
      ret = pthread_create(&g_ha.thread, &attr, ha_worker, NULL);
    }

  if (attr_initialized)
    {
      pthread_attr_destroy(&attr);
    }

  if (ret != 0)
    {
      pthread_mutex_lock(&g_ha.lock);
      g_ha.running = false;
      memset(g_ha.request.token, 0, sizeof(g_ha.request.token));
      pthread_mutex_unlock(&g_ha.lock);
      return -ret;
    }

  pthread_detach(g_ha.thread);

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int qpk_ha_get_state(const char *base_url, const char *token,
                     const char *entity_id)
{
  char path[HA_PATH_MAX];

  if (!ha_component_valid(entity_id, true))
    {
      return -EINVAL;
    }

  snprintf(path, sizeof(path), "/api/states/%s", entity_id);
  return ha_start(base_url, token, path, NULL);
}

int qpk_ha_get(const char *base_url, const char *token, const char *resource)
{
  const char *path;

  if (strcmp(resource, "config") == 0)
    {
      path = "/api/config";
    }
  else if (strcmp(resource, "states") == 0)
    {
      path = "/api/states";
    }
  else if (strcmp(resource, "services") == 0)
    {
      path = "/api/services";
    }
  else
    {
      return -EINVAL;
    }

  return ha_start(base_url, token, path, NULL);
}

int qpk_ha_call_service(const char *base_url, const char *token,
                        const char *domain, const char *service,
                        const char *data)
{
  char path[HA_PATH_MAX];

  if (!ha_component_valid(domain, false) ||
      !ha_component_valid(service, false) ||
      data == NULL || data[0] != '{' || strlen(data) >= HA_BODY_MAX)
    {
      return -EINVAL;
    }

  snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);
  return ha_start(base_url, token, path, data);
}

void qpk_ha_poll(struct qpk_ha_result_s *result)
{
  memset(result, 0, sizeof(*result));
  if (!g_ha.initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_ha.lock);
  result->busy = g_ha.running;
  result->done = g_ha.done;
  result->status = g_ha.status;
  result->error = g_ha.error;
  result->body = g_ha.response;
  g_ha.response = NULL;
  g_ha.done = false;
  pthread_mutex_unlock(&g_ha.lock);
}

void qpk_ha_stop(void)
{
  bool running;

  if (!g_ha.initialized)
    {
      return;
    }

  pthread_mutex_lock(&g_ha.lock);
  running = g_ha.running;
  if (g_ha.socket >= 0)
    {
      shutdown(g_ha.socket, SHUT_RDWR);
    }

  pthread_mutex_unlock(&g_ha.lock);
  while (running)
    {
      usleep(10000);
      pthread_mutex_lock(&g_ha.lock);
      running = g_ha.running;
      pthread_mutex_unlock(&g_ha.lock);
    }

  pthread_mutex_destroy(&g_ha.lock);
  free(g_ha.response);
  memset(&g_ha, 0, sizeof(g_ha));
}
