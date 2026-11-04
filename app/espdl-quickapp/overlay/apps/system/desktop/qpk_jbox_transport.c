/* SPDX-License-Identifier: Apache-2.0 */
/* JMX1 client for streetartist/ProgrammableMatrix at 2ca78dcb9c9b825b943da3b36e7baf19f558d91d. */
#include "qpk_jbox.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern void qpk_dl_sha256(const void *,size_t,uint8_t[32]);
#ifndef JBOX_DISCOVERY_ADDRESS
#define JBOX_DISCOVERY_ADDRESS "255.255.255.255"
#endif
struct jbox_client {int fd;uint32_t sequence;unsigned timeout_ms;bool secured;uint8_t session[32];};
static uint64_t now_ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void wipe(void *p,size_t n)
{volatile uint8_t *b=p;while(n--)*b++=0;}
static uint32_t get32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put32(uint8_t *p,uint32_t n)
{for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static bool equal(const uint8_t *a,const uint8_t *b,size_t n)
{unsigned diff=0;for(size_t i=0;i<n;i++)diff|=a[i]^b[i];return diff==0;}
static int hmac(const uint8_t key[32],const void *data,size_t n,uint8_t out[32])
{
  if(n>JBOX_MAX_PAYLOAD+16)return -E2BIG;
  uint8_t *inner=malloc(64+n),outer[96];if(!inner)return -ENOMEM;
  memset(inner,0x36,64);memset(outer,0x5c,64);
  for(unsigned i=0;i<32;i++){inner[i]^=key[i];outer[i]^=key[i];}
  if(n)memcpy(inner+64,data,n);
  qpk_dl_sha256(inner,64+n,outer+64);qpk_dl_sha256(outer,sizeof(outer),out);
  wipe(inner,64+n);free(inner);wipe(outer,sizeof(outer));return 0;
}
static int wait_fd(int fd,short event,uint64_t deadline,struct qpk_hw_progress *p)
{
  for(;;){
    if(p&&atomic_load(&p->cancel))return -ECANCELED;
    uint64_t now=now_ms();if(now>=deadline)return -ETIMEDOUT;
    unsigned left=(unsigned)(deadline-now);struct pollfd poller={.fd=fd,.events=event};
    int rc=poll(&poller,1,left>50?50:(int)left);
    if(rc<0){if(errno==EINTR)continue;return -errno;}
    if(rc&&poller.revents&event)return 0;
    if(rc&&poller.revents&(POLLERR|POLLHUP|POLLNVAL))return -ECONNRESET;
  }
}
static int transfer(int fd,uint8_t *buf,size_t n,bool sending,uint64_t deadline,
                    struct qpk_hw_progress *p)
{
  size_t done=0;
  while(done<n){
    int rc=wait_fd(fd,sending?POLLOUT:POLLIN,deadline,p);if(rc)return rc;
#ifdef MSG_NOSIGNAL
    int flags=sending?MSG_NOSIGNAL:0;
#else
    int flags=0;
#endif
    ssize_t got=sending?send(fd,buf+done,n-done,flags):recv(fd,buf+done,n-done,0);
    if(got<0){if(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)continue;return -errno;}
    if(!got)return -ECONNRESET;
    done+=(size_t)got;if(p)atomic_fetch_add(&p->bytes,(unsigned)got);
  }
  return 0;
}
static void disconnect(struct jbox_client *c)
{if(c->fd>=0){shutdown(c->fd,SHUT_RDWR);close(c->fd);}c->fd=-1;c->secured=false;wipe(c->session,32);}
void jbox_close(struct jbox_client *c)
{if(c){disconnect(c);wipe(c,sizeof(*c));free(c);}}
bool jbox_connected(const struct jbox_client *c){return c&&c->fd>=0&&c->secured;}
static int open_socket(struct jbox_client *c,const char *host,struct qpk_hw_progress *p)
{
  struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(JBOX_PORT)};
  if(!host||inet_pton(AF_INET,host,&addr.sin_addr)!=1||!addr.sin_addr.s_addr||
     IN_MULTICAST(ntohl(addr.sin_addr.s_addr))||addr.sin_addr.s_addr==INADDR_BROADCAST)return -EINVAL;
  c->fd=socket(AF_INET,SOCK_STREAM,0);if(c->fd<0)return -errno;
  int ret=fcntl(c->fd,F_GETFL,0);if(ret<0||fcntl(c->fd,F_SETFL,ret|O_NONBLOCK)<0){ret=-errno;goto fail;}
  if(connect(c->fd,(struct sockaddr *)&addr,sizeof(addr))<0){
    if(errno!=EINPROGRESS){ret=-errno;goto fail;}
    ret=wait_fd(c->fd,POLLOUT,now_ms()+c->timeout_ms,p);if(ret)goto fail;
    int error=0;socklen_t size=sizeof(error);
    if(getsockopt(c->fd,SOL_SOCKET,SO_ERROR,&error,&size)<0){ret=-errno;goto fail;}
    if(error){ret=-error;goto fail;}
  }
  int yes=1;
  setsockopt(c->fd,IPPROTO_TCP,TCP_NODELAY,&yes,sizeof(yes));
  setsockopt(c->fd,SOL_SOCKET,SO_KEEPALIVE,&yes,sizeof(yes));
  return 0;
fail:disconnect(c);return ret;
}
static int exchange(struct jbox_client *c,uint8_t command,const void *request,size_t n,
                    void *reply,size_t capacity,size_t *received,int32_t *status,
                    bool sign,bool signed_reply,struct qpk_hw_progress *p)
{
  if(received)*received=0;
  if(status)*status=0;
  if(!c||c->fd<0)return -ENOTCONN;
  if(n>JBOX_MAX_PAYLOAD||(!request&&n)||(!reply&&capacity)||!received||!status)return -EINVAL;
  if(c->sequence==UINT32_MAX){disconnect(c);return -EOVERFLOW;}
  uint8_t *frame=malloc(JBOX_MAX_PAYLOAD+32);if(!frame)return -ENOMEM;
  uint8_t sum[32]={0};uint32_t seq=++c->sequence;uint64_t deadline=now_ms()+c->timeout_ms;
  memcpy(frame,"JMX1",4);frame[4]=1;frame[5]=command;frame[6]=sign?2:0;frame[7]=0;
  put32(frame+8,seq);put32(frame+12,(uint32_t)n);if(n)memcpy(frame+16,request,n);
  int ret=0;
  if(sign){ret=hmac(c->session,frame,16+n,sum);if(ret)goto fail;memcpy(frame+16+n,sum,16);}
  ret=transfer(c->fd,frame,16+n+(sign?16:0),true,deadline,p);if(ret)goto fail;
  ret=transfer(c->fd,frame,16,false,deadline,p);if(ret)goto fail;
  uint32_t size=get32(frame+12);
  if(memcmp(frame,"JMX1",4)||frame[4]!=1||frame[5]!=command||
     frame[6]!=(signed_reply?3:1)||frame[7]||get32(frame+8)!=seq||size<4||size>JBOX_MAX_PAYLOAD){ret=-EPROTO;goto fail;}
  ret=transfer(c->fd,frame+16,size+(signed_reply?16:0),false,deadline,p);if(ret)goto fail;
  if(signed_reply){ret=hmac(c->session,frame,16+size,sum);if(ret)goto fail;
    if(!equal(sum,frame+16+size,16)){ret=-EACCES;goto fail;}}
  *status=(int32_t)get32(frame+16);
  if(size-4>capacity){ret=-EMSGSIZE;goto fail;}
  if(size>4)memcpy(reply,frame+20,size-4);
  *received=size-4;
  goto done;
fail:disconnect(c);
done:wipe(sum,sizeof(sum));wipe(frame,JBOX_MAX_PAYLOAD+32);free(frame);return ret;
}
int jbox_rpc(struct jbox_client *c,uint8_t command,const void *request,size_t n,
             void *reply,size_t capacity,size_t *received,int32_t *status,
             struct qpk_hw_progress *p)
{
  if(!jbox_connected(c))return -ENOTCONN;
  /* Handshake, key disclosure and factory reset are not raw application RPCs. */
  if(command<0x10||command==0x7f)return -EACCES;
  return exchange(c,command,request,n,reply,capacity,received,status,true,true,p);
}
int jbox_pair(const char *host,uint8_t key[32],struct qpk_hw_progress *p)
{
  if(!key)return -EINVAL;
  wipe(key,32);
  struct jbox_client c={.fd=-1,.timeout_ms=5000};uint8_t data[32]={0};size_t n=0;int32_t status=0;
  int ret=open_socket(&c,host,p);
  if(!ret)ret=exchange(&c,3,NULL,0,data,sizeof(data),&n,&status,false,false,p);
  if(!ret&&status)ret=-EACCES;
  if(!ret&&n!=32)ret=-EPROTO;
  if(!ret)memcpy(key,data,32);
  wipe(data,32);disconnect(&c);return ret;
}
int jbox_connect(struct jbox_client **out,const char *host,const uint8_t key[32],
                 unsigned timeout_ms,struct qpk_hw_progress *p)
{
  if(!out||*out||!key||timeout_ms<100||timeout_ms>15000)return -EINVAL;
  struct jbox_client *c=calloc(1,sizeof(*c));if(!c)return -ENOMEM;
  c->fd=-1;c->timeout_ms=timeout_ms;
  uint8_t data[32]={0},input[30]={0},proof[32]={0};size_t n=0;int32_t status=0;
  int ret=open_socket(c,host,p);
  if(!ret)ret=exchange(c,1,NULL,0,data,sizeof(data),&n,&status,false,false,p);
  if(!ret&&(status||n!=16))ret=status?-EACCES:-EPROTO;
  if(!ret){
    memcpy(input,data,16);memcpy(input+16,"JMX-AUTH-v1",11);
    ret=hmac(key,input,27,proof);
    memcpy(input+16,"JMX-SESSION-v1",14);
    if(!ret)ret=hmac(key,input,30,c->session);
    if(!ret)ret=exchange(c,2,proof,32,data,sizeof(data),&n,&status,false,true,p);
    if(!ret&&(status||n))ret=status?-EACCES:-EPROTO;
  }
  wipe(data,sizeof(data));wipe(input,sizeof(input));wipe(proof,sizeof(proof));
  if(ret){jbox_close(c);return ret;}c->secured=true;*out=c;return 0;
}
int jbox_discover(cJSON *out,unsigned timeout_ms,struct qpk_hw_progress *p)
{
  if(timeout_ms<100||timeout_ms>5000)return -EINVAL;
  cJSON *items=cJSON_AddArrayToObject(out,"devices");if(!items)return -ENOMEM;
  int fd=socket(AF_INET,SOCK_DGRAM,0);if(fd<0)return -errno;
  int yes=1,ret=0;
  if(setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof(yes))<0){ret=-errno;goto done;}
  int flags=fcntl(fd,F_GETFL,0);if(flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0){ret=-errno;goto done;}
  struct sockaddr_in target={.sin_family=AF_INET,.sin_port=htons(4748)};
  if(inet_pton(AF_INET,JBOX_DISCOVERY_ADDRESS,&target.sin_addr)!=1){ret=-EINVAL;goto done;}
  static const char query[]="JBOX-DISCOVER";
  if(sendto(fd,query,sizeof(query)-1,0,(struct sockaddr *)&target,sizeof(target))!=(ssize_t)sizeof(query)-1){ret=-errno;goto done;}
  uint64_t deadline=now_ms()+timeout_ms;
  for(;;){
    ret=wait_fd(fd,POLLIN,deadline,p);if(ret==-ETIMEDOUT){ret=0;break;}if(ret)break;
    char data[128]={0};struct sockaddr_in peer;socklen_t size=sizeof(peer);
    ssize_t count=recvfrom(fd,data,sizeof(data),0,(struct sockaddr *)&peer,&size);
    if(count<0){if(errno==EAGAIN||errno==EINTR)continue;ret=-errno;break;}
    if(count!=26||memcmp(data,"JBOX-HERE|",10)||data[22]!='|'||data[24]!='|'||
       (data[23]!='0'&&data[23]!='1')||(data[25]!='0'&&data[25]!='1')||
       strspn(data+10,"0123456789abcdefABCDEF")!=12)continue;
    data[22]=0;char address[INET_ADDRSTRLEN];
    for(unsigned i=10;i<22;i++)if(data[i]>='A'&&data[i]<='F')data[i]+=32;
    if(!inet_ntop(AF_INET,&peer.sin_addr,address,sizeof(address)))continue;
    bool duplicate=false;
    for(cJSON *v=items->child;v;v=v->next){
      const cJSON *id=cJSON_GetObjectItemCaseSensitive(v,"deviceId");
      const cJSON *host=cJSON_GetObjectItemCaseSensitive(v,"host");
      if(!strcmp(id->valuestring,data+10)&&!strcmp(host->valuestring,address))duplicate=true;
    }
    if(duplicate)continue;
    if(cJSON_GetArraySize(items)>=16){cJSON_AddBoolToObject(out,"truncated",true);continue;}
    cJSON *v=cJSON_CreateObject();if(!v){ret=-ENOMEM;break;}
    if(!cJSON_AddStringToObject(v,"deviceId",data+10)||!cJSON_AddStringToObject(v,"host",address)||
       !cJSON_AddBoolToObject(v,"paired",data[23]=='1')||!cJSON_AddBoolToObject(v,"pairingOpen",data[25]=='1')){
      cJSON_Delete(v);ret=-ENOMEM;break;}
    cJSON_AddItemToArray(items,v);
  }
done:close(fd);return ret;
}
