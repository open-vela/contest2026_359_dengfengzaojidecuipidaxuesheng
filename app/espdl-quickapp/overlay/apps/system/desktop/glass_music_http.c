/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include "glass_music.h"
#include <netutils/webclient.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#include "music_root_ca.inc"

struct music_request
{
  int64_t deadline;
  music_cancel_cb cancelled;
  void *context;
};

struct webclient_tls_connection
{
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_x509_crt ca;
};

#define MUSIC_SOCKET_RECV_BUFFER 3072
#define MUSIC_CLOSE_DRAIN_LIMIT 16384

static int64_t milliseconds(void)
{
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int wait_socket(struct music_request *request, int fd, int events)
{
  for (;;)
    {
      if (request->cancelled && request->cancelled(request->context)) return -ECANCELED;
      int64_t remaining = request->deadline - milliseconds();
      if (remaining <= 0) return -ETIMEDOUT;
      struct pollfd pfd = {.fd = fd, .events = events};
      int ret = poll(&pfd, 1, remaining > 250 ? 250 : (int)remaining);
      if (ret < 0 && errno == EINTR) continue;
      if (!ret) continue;
      if (ret < 0) return -errno;
      if (pfd.revents & POLLNVAL) return -EBADF;
      return 0;
    }
}

static int tcp_connect(struct music_request *request, const char *host, const char *port)
{
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM,
                           .ai_protocol = IPPROTO_TCP};
  struct addrinfo *addresses = NULL;
  if (getaddrinfo(host, port, &hints, &addresses)) return -EHOSTUNREACH;
  int result = -ECONNREFUSED;
  for (struct addrinfo *addr = addresses; addr; addr = addr->ai_next)
    {
      if (milliseconds() >= request->deadline) { result = -ETIMEDOUT; break; }
      int fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
      if (fd < 0) { result = -errno; continue; }
      int receive_buffer = MUSIC_SOCKET_RECV_BUFFER;
      if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer,
                     sizeof(receive_buffer)) < 0)
        { result = -errno; close(fd); continue; }
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        { result = -errno; close(fd); continue; }
      int ret = connect(fd, addr->ai_addr, addr->ai_addrlen);
      int error = ret == 0 ? 0 : errno;
      if (error == EINPROGRESS || error == EWOULDBLOCK || error == EINTR)
        {
          result = wait_socket(request, fd, POLLOUT);
          if (result < 0) { close(fd); continue; }
          socklen_t size = sizeof(error);
          if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size)) error = errno;
        }
      if (!error) { result = fd; break; }
      result = -error;
      close(fd);
    }
  freeaddrinfo(addresses);
  return result;
}

static int tls_close(void *ctx, struct webclient_tls_connection *conn)
{
  (void)ctx;
  if (!conn) return 0;
  size_t drained = 0;
  if (conn->net.fd >= 0)
    {
      /* A cancelled stream can leave the complete IOB pool attached to TCP
       * read-ahead. Drain the nonblocking socket before close so those IOBs
       * are returned even when the asynchronous close callback is starved. */
      unsigned char discard[256];
      while (drained < MUSIC_CLOSE_DRAIN_LIMIT)
        {
          ssize_t ret = recv(conn->net.fd, discard, sizeof(discard), 0);
          if (ret > 0) { drained += ret; continue; }
          if (ret < 0 && errno == EINTR) continue;
          break;
        }
      shutdown(conn->net.fd, SHUT_RDWR);
    }
  mbedtls_net_free(&conn->net);
  if (drained) printf("[music] close drained=%u\n", (unsigned)drained);
  mbedtls_ssl_free(&conn->ssl);
  mbedtls_ssl_config_free(&conn->config);
  mbedtls_x509_crt_free(&conn->ca);
  free(conn);
  return 0;
}

static int tls_wait(struct music_request *request,
                    struct webclient_tls_connection *conn, int ret)
{
  if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
    return -EPROTO;
  return wait_socket(request, conn->net.fd,
                      ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT);
}

static int tls_connect(void *ctx, const char *host, const char *port,
                       unsigned timeout, struct webclient_tls_connection **out)
{
  (void)timeout;
  *out = NULL;
  if (strcmp(port, "443")) return -EACCES;
  struct music_request *request = ctx;
  struct webclient_tls_connection *conn = calloc(1, sizeof(*conn));
  if (!conn) return -ENOMEM;
  mbedtls_net_init(&conn->net);
  mbedtls_ssl_init(&conn->ssl);
  mbedtls_ssl_config_init(&conn->config);
  mbedtls_x509_crt_init(&conn->ca);
  int result = -EPROTO;
  int ret = mbedtls_x509_crt_parse(&conn->ca, music_root_ca, sizeof(music_root_ca));
  if (ret) goto fail;
  ret = mbedtls_ssl_config_defaults(&conn->config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret) goto fail;
  mbedtls_ssl_conf_authmode(&conn->config, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conn->config, &conn->ca, NULL);
  ret = mbedtls_ssl_setup(&conn->ssl, &conn->config);
  if (ret) goto fail;
  ret = mbedtls_ssl_set_hostname(&conn->ssl, host);
  if (ret) goto fail;
  result = tcp_connect(request, host, port);
  if (result < 0) goto fail;
  conn->net.fd = result;
  mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send, mbedtls_net_recv, NULL);
  while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0)
    {
      result = tls_wait(request, conn, ret);
      if (result < 0) goto fail;
    }
  if (mbedtls_ssl_get_verify_result(&conn->ssl)) { result = -EACCES; goto fail; }
  printf("[music] TLS verified %s\n", mbedtls_ssl_get_version(&conn->ssl));
  *out = conn;
  return 0;
fail:
  printf("[music] TLS/connect failed ret=%d tls=%d verify=%lu\n", result, ret,
         (unsigned long)mbedtls_ssl_get_verify_result(&conn->ssl));
  tls_close(ctx, conn);
  return result;
}

static ssize_t tls_send(void *ctx, struct webclient_tls_connection *conn,
                        const void *data, size_t size)
{
  struct music_request *request = ctx;
  for (;;)
    {
      if (request->cancelled && request->cancelled(request->context)) return -ECANCELED;
      if (milliseconds() >= request->deadline) return -ETIMEDOUT;
      int ret = mbedtls_ssl_write(&conn->ssl, data, size);
      if (ret >= 0) { request->deadline = milliseconds() + 15000; return ret; }
      int wait = tls_wait(request, conn, ret);
      if (wait < 0) return wait;
    }
}

static ssize_t tls_recv(void *ctx, struct webclient_tls_connection *conn,
                        void *data, size_t size)
{
  struct music_request *request = ctx;
  /* Consumer backpressure (including pause) is not a network timeout. */
  request->deadline = milliseconds() + 15000;
  for (;;)
    {
      if (request->cancelled && request->cancelled(request->context)) return -ECANCELED;
      if (milliseconds() >= request->deadline) return -ETIMEDOUT;
      int ret = mbedtls_ssl_read(&conn->ssl, data, size);
      /* TLS 1.3 post-handshake tickets are notifications, not HTTP data or
       * connection errors. This client does not retain sessions. */
      if (ret == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
      if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
      if (ret >= 0) { request->deadline = milliseconds() + 15000; return ret; }
      int wait = tls_wait(request, conn, ret);
      if (wait < 0) return wait;
    }
}

struct music_http_sink {
  music_data_cb callback;
  void *context;
};
static int music_sink(char **buffer, int offset, int end, int *length, void *ctx)
{
  struct music_http_sink *sink = ctx;
  if (offset < 0 || end < offset || end > *length) return -EINVAL;
  return sink->callback(sink->context, (const unsigned char *)*buffer + offset, end - offset);
}
int music_http_get(const char *url, music_data_cb sink, void *ctx,
                   music_cancel_cb cancelled, unsigned *status)
{
  if (!url || strncmp(url, "https://", 8) || !sink) return -EINVAL;
  static const struct webclient_tls_ops tls =
    {.connect = tls_connect, .send = tls_send, .recv = tls_recv, .close = tls_close};
  if (psa_crypto_init() != PSA_SUCCESS) return -EIO;
  struct music_request request = {.deadline = milliseconds() + 30000,
                                  .cancelled = cancelled, .context = ctx};
  struct music_http_sink target = {.callback = sink, .context = ctx};
  struct webclient_context client;
  webclient_set_defaults(&client);
  char *buffer = malloc(8192);
  if (!buffer) return -ENOMEM;
  client.url = url;
  client.buffer = buffer;
  client.buflen = 8192;
  client.sink_callback = music_sink;
  client.sink_callback_arg = &target;
  client.tls_ops = &tls;
  client.tls_ctx = &request;
  client.timeout_sec = 15;
  client.protocol_version = WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;
  int ret = webclient_perform(&client);
  if (status) *status = client.http_status;
  if (!ret && client.http_status != 200) ret = -EIO;
  /* Blocking perform owns and releases its connection on every exit. */
  free(buffer);
  return ret;
}
