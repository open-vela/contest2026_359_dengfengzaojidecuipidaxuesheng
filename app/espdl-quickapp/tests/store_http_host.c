/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_store_http.h"
#include "glass_https.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int mode;
static int64_t now;
static atomic_bool cancel;
static const struct webclient_tls_ops tls_ops={0};
int64_t glass_https_milliseconds(void){return now;}
const struct webclient_tls_ops *glass_https_bounded_tls_ops(void){return &tls_ops;}
void webclient_set_defaults(struct webclient_context *c){memset(c,0,sizeof(*c));}
int webclient_perform(struct webclient_context *c)
{
  assert(c->timeout_sec==10&&c->nheaders==2&&c->buflen==2048);
  assert(!strstr(c->url,"//v1"));
  if(!strncmp(c->url,"https://",8)){
    assert(c->tls_ops==&tls_ops);struct glass_https_bounded_request *tls=c->tls_ctx;
    assert(tls->connect_timeout_ms==5000&&tls->request.ca_size>2000);
    assert(tls->request.abort_flag==&cancel&&tls->request.deadline_ms==now+30000);
    assert(strstr((const char *)tls->request.ca_pem,"BEGIN CERTIFICATE"));
  }else assert(!c->tls_ops);
  if(mode==1)return c->header_callback("Location: http://untrusted.example/file",false,NULL);
  if(mode==2)return c->header_callback("Content-Encoding: gzip",false,NULL);
  if(mode==3)return c->header_callback("too long",true,NULL);
  c->http_status=mode==4?404:mode==5?503:200;
  char text[65];memset(text,'x',sizeof(text));int length=sizeof(text);
  if(mode==6){text[1]=0;return c->sink_callback(&(char *){text},0,3,&length,c->sink_callback_arg);}
  if(mode==7)atomic_store(&cancel,true);
  if(mode==8)now+=30001;
  if(mode==9)return c->sink_callback(&(char *){text},0,65,&length,c->sink_callback_arg);
  int ret=c->header_callback("Content-Encoding: identity",false,NULL);if(ret)return ret;
  ret=c->sink_callback(&(char *){text},0,5,&length,c->sink_callback_arg);if(ret)return ret;
  return c->sink_callback(&(char *){text},5,12,&length,c->sink_callback_arg);
}
static int fetch(int selected)
{
  mode=selected;now=100;atomic_store(&cancel,false);char *text=NULL;
  int ret=glass_store_http_fetch("https://store.example.com/","/v1/catalog",64,&cancel,&text);
  if(!ret){assert(text&&strlen(text)==12);free(text);}else assert(!text);
  return ret;
}
int main(void)
{
  assert(!fetch(0));assert(fetch(1)==-EACCES);assert(fetch(2)==-ENOTSUP);assert(fetch(3)==-EFBIG);
  assert(fetch(4)==-ENOENT&&fetch(5)==-EREMOTEIO&&fetch(6)==-EBADMSG);
  assert(fetch(7)==-ECANCELED&&fetch(8)==-ETIMEDOUT&&fetch(9)==-EFBIG);
  now=100;mode=0;atomic_store(&cancel,false);char *text=NULL;
  assert(!glass_store_http_fetch("http://127.0.0.1:8080","/v1/catalog",64,&cancel,&text));free(text);
  assert(glass_store_http_fetch("http://public.example.com","/v1/catalog",64,&cancel,&text)==-EINVAL);
  assert(glass_store_http_fetch("https://store.example.com","/v1/catalog\r\nBad",64,&cancel,&text)==-EINVAL);
  atomic_store(&cancel,true);assert(glass_store_http_fetch("https://store.example.com","/v1/catalog",64,&cancel,&text)==-ECANCELED);
  puts("STORE_HTTP_OK: production request/sinks; HTTPS trust/deadlines/abort; LAN HTTP; redirect/encoding/header/NUL/body/status rejection; mid-read cancel and timeout; no credentials");
}
