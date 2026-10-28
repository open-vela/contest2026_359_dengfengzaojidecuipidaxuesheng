#!/usr/bin/env python3
"""Opt-in browser TCP deadline; preserve the original glass_https ABI/API."""
def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('unsupported TLS source anchor: '+old[:70])
    return text.replace(old, new, 1)

def header(text):
    if 'glass_https_bounded_tls_ops' in text:
        raise ValueError('TCP budget already applied')
    return text + '''
/* Opt-in extension: original request layout and TLS ops remain unchanged. */
struct glass_https_bounded_request {
  struct glass_https_request request;
  uint32_t connect_timeout_ms;
};
const struct webclient_tls_ops *glass_https_bounded_tls_ops(void);
'''

def source(text):
    text=once(text,
        'static int tcp_connect(struct glass_https_request *request,const char *host,const char *port)\n{',
        '''static int tcp_connect(struct glass_https_request *request,const char *host,const char *port,
                       uint32_t connect_timeout_ms)
{
  /* DNS/TCP use a local deadline. Do not consume/replace the caller's
   * handshake deadline or modify the old request ABI used by other apps. */
  struct glass_https_request bounded=*request;
  if(connect_timeout_ms) {
    int64_t end=glass_https_milliseconds()+connect_timeout_ms;
    if(!bounded.deadline_ms||end<bounded.deadline_ms) bounded.deadline_ms=end;
    request=&bounded;
  }''')
    text=once(text,
        'static int tls_connect(void *ctx,const char *host,const char *port,\n                       unsigned timeout,struct webclient_tls_connection **out)',
        'static int tls_connect_common(void *ctx,const char *host,const char *port,\n                       unsigned timeout,struct webclient_tls_connection **out, uint32_t connect_timeout_ms)')
    text=once(text, 'result=tcp_connect(request,host,port);',
              'result=tcp_connect(request,host,port,connect_timeout_ms);')
    text=once(text, 'static ssize_t tls_send(', '''static int tls_connect(void *ctx,const char *host,const char *port,
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

static ssize_t tls_send(''')
    text += '''
const struct webclient_tls_ops *glass_https_bounded_tls_ops(void)
{
  static const struct webclient_tls_ops ops={.connect=bounded_connect,.send=tls_send,.recv=tls_recv,.close=tls_close};
  return &ops;
}
'''
    return text
