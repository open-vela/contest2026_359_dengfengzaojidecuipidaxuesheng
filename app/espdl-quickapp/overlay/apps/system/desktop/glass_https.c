/* SPDX-License-Identifier: Apache-2.0
 * Shared transport extracted from the board-verified glass_weather_https.c.
 * Keep the same IPv4 connector, nonblocking mbedTLS BIO, WANT_READ/WRITE loop
 * and TLS 1.3 ticket handling for both weather and ESPClaw.
 */
#include "glass_https.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

struct webclient_tls_connection {
  mbedtls_net_context net;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;
  mbedtls_x509_crt ca;
  struct glass_https_request request;
};

static pthread_once_t crypto_once=PTHREAD_ONCE_INIT;
static int crypto_result;
static void initialize_crypto(void)
{ crypto_result=psa_crypto_init()==PSA_SUCCESS?0:-EIO; }
int glass_https_initialize(void)
{ pthread_once(&crypto_once,initialize_crypto); return crypto_result; }

int glass_https_validate_ca(const unsigned char *pem,size_t size)
{
  if(!pem||size<2||pem[size-1]!=0) return -EINVAL;
  int ret=glass_https_initialize(); if(ret) return ret;
  mbedtls_x509_crt ca; mbedtls_x509_crt_init(&ca);
  ret=mbedtls_x509_crt_parse(&ca,pem,size);
  mbedtls_x509_crt_free(&ca); return ret?-EINVAL:0;
}

int64_t glass_https_milliseconds(void)
{
  struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
  return (int64_t)now.tv_sec*1000+now.tv_nsec/1000000;
}

static int request_state(const struct glass_https_request *request)
{
  if(request->abort_flag&&atomic_load(request->abort_flag)) return -ECANCELED;
  return glass_https_milliseconds()>=request->deadline_ms?-ETIMEDOUT:0;
}

static void stage(struct glass_https_request *request,const char *name)
{ if(request->diagnostic) request->diagnostic->stage=name; }

static int wait_socket(struct glass_https_request *request,int fd,int events)
{
  for(;;) {
    int state=request_state(request); if(state) return state;
    int64_t remaining=request->deadline_ms-glass_https_milliseconds();
    if(remaining<=0) return -ETIMEDOUT;
    /* Weather retains its original wait; cancellable chat checks every 100 ms. */
    int slice=request->abort_flag?100:10000;
    struct pollfd pfd={.fd=fd,.events=events};
    int ret=poll(&pfd,1,remaining>slice?slice:(int)remaining);
    if(ret<0&&errno==EINTR) continue;
    if(ret<0) return -errno;
    if(!ret) {
      if(request->abort_flag) continue;
      return -ETIMEDOUT;
    }
    if(pfd.revents&POLLNVAL) return -EBADF;
    return 0;
  }
}

static int tcp_connect(struct glass_https_request *request,const char *host,const char *port,
                       uint32_t connect_timeout_ms)
{
  /* DNS/TCP use a local deadline. Do not consume/replace the caller's
   * handshake deadline or modify the old request ABI used by other apps. */
  struct glass_https_request bounded=*request;
  if(connect_timeout_ms) {
    int64_t end=glass_https_milliseconds()+connect_timeout_ms;
    if(!bounded.deadline_ms||end<bounded.deadline_ms) bounded.deadline_ms=end;
    request=&bounded;
  }
  struct addrinfo hints={.ai_family=AF_INET,.ai_socktype=SOCK_STREAM,.ai_protocol=IPPROTO_TCP};
  struct addrinfo *addresses=NULL;
  stage(request,"dns");
  if(getaddrinfo(host,port,&hints,&addresses)) return -EHOSTUNREACH;
  int result=-ECONNREFUSED;
  stage(request,"tcp");
  for(struct addrinfo *addr=addresses;addr;addr=addr->ai_next) {
    int state=request_state(request); if(state) { result=state; break; }
    int fd=socket(addr->ai_family,addr->ai_socktype,addr->ai_protocol);
    if(fd<0) { result=-errno; continue; }
    int flags=fcntl(fd,F_GETFL,0);
    if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0) { result=-errno; close(fd); continue; }
    int ret=connect(fd,addr->ai_addr,addr->ai_addrlen);
    int error=ret==0?0:errno;
    if(error==EINPROGRESS||error==EWOULDBLOCK||error==EINTR) {
      result=wait_socket(request,fd,POLLOUT);
      if(result<0) { close(fd); continue; }
      socklen_t size=sizeof(error);
      if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&error,&size)) error=errno;
    }
    if(!error) { result=fd; break; }
    result=-error; close(fd);
  }
  freeaddrinfo(addresses); return result;
}

static int tls_close(void *ctx,struct webclient_tls_connection *conn)
{
  (void)ctx; if(!conn) return 0;
  mbedtls_net_free(&conn->net); mbedtls_ssl_free(&conn->ssl);
  mbedtls_ssl_config_free(&conn->config); mbedtls_x509_crt_free(&conn->ca);
  free(conn); return 0;
}

static int tls_wait(struct webclient_tls_connection *conn,int ret)
{
  if(ret!=MBEDTLS_ERR_SSL_WANT_READ&&ret!=MBEDTLS_ERR_SSL_WANT_WRITE) {
    struct glass_https_diagnostic *d=conn->request.diagnostic;
    uint32_t verify=mbedtls_ssl_get_verify_result(&conn->ssl);
    if(d) { d->tls_error=ret; d->verify_flags=verify; }
    /* UINT32_MAX means verification was not completed, not a bad certificate. */
    return (verify&&verify!=UINT32_MAX)||ret==MBEDTLS_ERR_X509_CERT_VERIFY_FAILED?-EACCES:-EPROTO;
  }
  return wait_socket(&conn->request,conn->net.fd,
                      ret==MBEDTLS_ERR_SSL_WANT_READ?POLLIN:POLLOUT);
}

static int tls_connect_common(void *ctx,const char *host,const char *port,
                       unsigned timeout,struct webclient_tls_connection **out, uint32_t connect_timeout_ms)
{
  if(out) *out=NULL;
  struct glass_https_request *request=ctx;
  if(!out||!request||!host||!*host||!port||!request->ca_pem||!request->ca_size) return -EINVAL;
  int result=glass_https_initialize(); if(result) return result;
  struct webclient_tls_connection *conn=calloc(1,sizeof(*conn));
  if(!conn) return -ENOMEM;
  conn->request=*request;
  if(!conn->request.deadline_ms) conn->request.deadline_ms=glass_https_milliseconds()+(int64_t)timeout*1000;
  request=&conn->request;
  mbedtls_net_init(&conn->net); mbedtls_ssl_init(&conn->ssl);
  mbedtls_ssl_config_init(&conn->config); mbedtls_x509_crt_init(&conn->ca);
  stage(request,"trust"); result=-EPROTO;
  int ret=mbedtls_x509_crt_parse(&conn->ca,request->ca_pem,request->ca_size);
  if(ret) goto fail;
  ret=mbedtls_ssl_config_defaults(&conn->config,MBEDTLS_SSL_IS_CLIENT,
                                 MBEDTLS_SSL_TRANSPORT_STREAM,MBEDTLS_SSL_PRESET_DEFAULT);
  if(ret) goto fail;
  mbedtls_ssl_conf_authmode(&conn->config,MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_ca_chain(&conn->config,&conn->ca,NULL);
  ret=mbedtls_ssl_setup(&conn->ssl,&conn->config); if(ret) goto fail;
  ret=mbedtls_ssl_set_hostname(&conn->ssl,host); if(ret) goto fail;
  result=tcp_connect(request,host,port,connect_timeout_ms); if(result<0) goto fail;
  conn->net.fd=result;
  /* This is the same BIO used by the verified weather connection. */
  mbedtls_ssl_set_bio(&conn->ssl,&conn->net,mbedtls_net_send,mbedtls_net_recv,NULL);
  stage(request,"handshake");
  while((ret=mbedtls_ssl_handshake(&conn->ssl))!=0) {
    result=tls_wait(conn,ret); if(result<0) goto fail;
  }
  if(mbedtls_ssl_get_verify_result(&conn->ssl)) { result=-EACCES; goto fail; }
  stage(request,"connected"); *out=conn; return 0;
fail:
  if(request->diagnostic) {
    request->diagnostic->transport_error=result;
    request->diagnostic->tls_error=ret;
    request->diagnostic->verify_flags=mbedtls_ssl_get_verify_result(&conn->ssl);
  }
  tls_close(ctx,conn); return result;
}

static int tls_connect(void *ctx,const char *host,const char *port,
                       unsigned timeout,struct webclient_tls_connection **out)
{ return tls_connect_common(ctx,host,port,timeout,out,0); }
static int bounded_connect(void *ctx,const char *host,const char *port,
                       unsigned timeout,struct webclient_tls_connection **out)
{
  struct glass_https_bounded_request *r=ctx;
  if(!r) return -EINVAL;
  int result=tls_connect_common(&r->request,host,port,timeout,out,r->connect_timeout_ms);
  /* A completed handshake starts the HTTP I/O phase. Crypto CPU time may
   * consume the last connect-budget slice; do not reject the first write
   * on a healthy verified connection because of that old deadline. */
  if(!result&&r->request.idle_timeout_ms)
    (*out)->request.deadline_ms=glass_https_milliseconds()+r->request.idle_timeout_ms;
  return result;
}

static ssize_t tls_send(void *ctx,struct webclient_tls_connection *conn,const void *data,size_t size)
{
  (void)ctx; stage(&conn->request,"send");
  for(;;) {
    int state=request_state(&conn->request); if(state) return state;
    int ret=mbedtls_ssl_write(&conn->ssl,data,size);
    if(ret>=0) {
      if(ret>0&&conn->request.idle_timeout_ms) conn->request.deadline_ms=glass_https_milliseconds()+conn->request.idle_timeout_ms;
      return ret;
    }
    int wait=tls_wait(conn,ret); if(wait<0) return wait;
  }
}

static ssize_t tls_recv(void *ctx,struct webclient_tls_connection *conn,void *data,size_t size)
{
  (void)ctx; stage(&conn->request,"receive");
  for(;;) {
    int state=request_state(&conn->request); if(state) return state;
    int ret=mbedtls_ssl_read(&conn->ssl,data,size);
    /* TLS 1.3 notifications are not HTTP bytes or a failed read. */
    if(ret==MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
      if(conn->request.diagnostic) conn->request.diagnostic->session_tickets++;
      continue;
    }
    if(ret==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if(ret>=0) {
      if(ret>0&&conn->request.idle_timeout_ms) conn->request.deadline_ms=glass_https_milliseconds()+conn->request.idle_timeout_ms;
      return ret;
    }
    int wait=tls_wait(conn,ret); if(wait<0) return wait;
  }
}

const struct webclient_tls_ops *glass_https_tls_ops(void)
{
  static const struct webclient_tls_ops ops={.connect=tls_connect,.send=tls_send,.recv=tls_recv,.close=tls_close};
  return &ops;
}

int glass_https_poll(struct webclient_tls_connection *conn,short events,int timeout_ms)
{
  if(!conn||timeout_ms<0) return -EINVAL;
  int state=request_state(&conn->request); if(state) return state;
  if((events&POLLIN)&&mbedtls_ssl_get_bytes_avail(&conn->ssl)>0) return 1;
  struct pollfd pfd={.fd=conn->net.fd,.events=events};
  int64_t end=glass_https_milliseconds()+timeout_ms;
  int ret;
  for(;;) {
    state=request_state(&conn->request);if(state)return state;
    int64_t now=glass_https_milliseconds();
    int64_t remaining=(end<conn->request.deadline_ms?end:conn->request.deadline_ms)-now;
    if(remaining<0)remaining=0;
    ret=poll(&pfd,1,(int)remaining);
    if(ret>=0||errno!=EINTR)break;
    if(glass_https_milliseconds()>=end)return request_state(&conn->request);
  }
  if(ret<0) return -errno;
  if(!ret) return request_state(&conn->request);
  if(pfd.revents&POLLNVAL) return -EBADF;
  if(pfd.revents&events) return 1;
  return pfd.revents&(POLLERR|POLLHUP)?-ECONNRESET:0;
}

void glass_https_set_deadline(struct webclient_tls_connection *conn,int64_t deadline_ms)
{ if(conn) conn->request.deadline_ms=deadline_ms; }
void glass_https_set_abort_flag(struct webclient_tls_connection *conn,atomic_bool *flag)
{ if(conn) conn->request.abort_flag=flag; }

const struct webclient_tls_ops *glass_https_bounded_tls_ops(void)
{
  static const struct webclient_tls_ops ops={.connect=bounded_connect,.send=tls_send,.recv=tls_recv,.close=tls_close};
  return &ops;
}
