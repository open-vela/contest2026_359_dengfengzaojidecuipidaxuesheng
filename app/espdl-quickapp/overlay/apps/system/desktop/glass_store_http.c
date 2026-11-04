/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_store_http.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
bool glass_store_http_base_valid(const char *url)
{
  if(!url||strlen(url)>=GLASS_STORE_URL_CAP)return false;
  bool tls=!strncmp(url,"https://",8),plain=!strncmp(url,"http://",7);
  if(!tls&&!plain)return false;
  const char *host=url+(tls?8:7),*end=host;
  while(*end&&*end!=':'&&*end!='/'){
    unsigned char ch=(unsigned char)*end;
    if(!isalnum(ch)||ch>=128){if(ch!='.'&&ch!='-')return false;}
    end++;
  }
  if(end==host||(size_t)(end-host)>=128)return false;
  char name[128];memcpy(name,host,(size_t)(end-host));name[end-host]=0;
  if(*end==':'){
    char *tail=NULL;unsigned long port=strtoul(end+1,&tail,10);
    if(tail==end+1||!port||port>65535||(*tail&&*tail!='/'))return false;
    for(const char *p=end+1;p<tail;p++)if(!isdigit((unsigned char)*p))return false;
    end=tail;
  }
  for(const char *p=end;*p;p++)if((unsigned char)*p>=128||(!isalnum((unsigned char)*p)&&*p!='/'&&*p!='_'&&*p!='-'&&*p!='.'))return false;
  if(strstr(end,".."))return false;
  if(plain){
    struct in_addr address;
    if(strcmp(name,"localhost")){
      if(inet_pton(AF_INET,name,&address)!=1)return false;
      uint32_t ip=ntohl(address.s_addr);
      if((ip>>24)!=10&&(ip>>24)!=127&&(ip>>16)!=0xc0a8&&(ip>>20)!=0xac1)return false;
    }
  }
  return true;
}

#if defined(__NuttX__) || defined(GLASS_STORE_HTTP_TEST)
#include <netutils/webclient.h>
#include "glass_https.h"
#include "weather_root_ca.inc"
struct store_response {char *body;size_t used,maximum;atomic_bool *cancel;int64_t deadline;char buffer[2048];};
static int response_sink(char **buffer,int offset,int end,int *length,void *opaque)
{
  struct store_response *r=opaque;
  if(atomic_load(r->cancel))return -ECANCELED;
  if(glass_https_milliseconds()>=r->deadline)return -ETIMEDOUT;
  if(offset<0||end<offset||end>*length)return -EINVAL;
  size_t n=(size_t)(end-offset);if(n>r->maximum-r->used)return -EFBIG;
  if(memchr(*buffer+offset,0,n))return -EBADMSG;
  memcpy(r->body+r->used,*buffer+offset,n);r->used+=n;r->body[r->used]=0;return 0;
}
static int response_header(const char *line,bool truncated,void *opaque)
{
  (void)opaque;if(truncated)return -EFBIG;
  /* Keep every download on the explicitly selected origin, without redirects. */
  if(!strncasecmp(line,"location:",9))return -EACCES;
  if(!strncasecmp(line,"content-encoding:",17)){
    const char *v=line+17;while(*v==' '||*v=='\t')v++;
    if(strncasecmp(v,"identity",8)|| (v[8]&&v[8]!='\r'&&v[8]!='\n'&&v[8]!=' '))return -ENOTSUP;
  }
  return 0;
}
int glass_store_http_fetch(const char *base,const char *path,size_t maximum,atomic_bool *cancel,char **out)
{
  *out=NULL;
  if(!glass_store_http_base_valid(base)||!path||path[0]!='/'||strpbrk(path,"\r\n#")||!cancel||!maximum||maximum>65536)return -EINVAL;
  if(atomic_load(cancel))return -ECANCELED;
  char url[320];size_t n=strlen(base);while(n&&base[n-1]=='/')n--;
  if(snprintf(url,sizeof(url),"%.*s%s",(int)n,base,path)>=(int)sizeof(url))return -ENAMETOOLONG;
  struct store_response *r=calloc(1,sizeof(*r));if(!r)return -ENOMEM;
  r->body=malloc(maximum+1);if(!r->body){free(r);return -ENOMEM;}
  r->body[0]=0;r->maximum=maximum;r->cancel=cancel;r->deadline=glass_https_milliseconds()+30000;
  struct glass_https_bounded_request tls={.request={.ca_pem=weather_root_ca,.ca_size=sizeof(weather_root_ca),.deadline_ms=r->deadline,.abort_flag=cancel},.connect_timeout_ms=5000};
  static const char *const headers[]={"Accept: application/json, application/javascript", "Accept-Encoding: identity"};
  struct webclient_context client;webclient_set_defaults(&client);
  client.url=url;client.protocol_version=WEBCLIENT_PROTOCOL_VERSION_HTTP_1_1;client.timeout_sec=10;
  client.headers=headers;client.nheaders=2;client.buffer=r->buffer;client.buflen=sizeof(r->buffer);
  client.sink_callback=response_sink;client.sink_callback_arg=r;client.header_callback=response_header;
  if(!strncmp(base,"https://",8)){client.tls_ops=glass_https_bounded_tls_ops();client.tls_ctx=&tls;}
  int ret=webclient_perform(&client);
  if(!ret&&atomic_load(cancel))ret=-ECANCELED;
  if(!ret&&client.http_status!=200)ret=client.http_status==404?-ENOENT:client.http_status==429?-EAGAIN:-EREMOTEIO;
  if(!ret&&!r->used)ret=-EBADMSG;
  if(!ret){*out=r->body;r->body=NULL;}
  free(r->body);free(r);return ret;
}
#endif
#if !defined(__NuttX__) && !defined(GLASS_STORE_HTTP_TEST)
/* Native UI fixtures do not open networks; transport tests supply an explicit
 * loopback/webclient fixture. Firmware always links the real implementation. */
__attribute__((weak)) int glass_store_http_fetch(const char *base,const char *path,size_t maximum,atomic_bool *cancel,char **out)
{(void)base;(void)path;(void)maximum;(void)cancel;*out=NULL;return -ENETDOWN;}
#endif
