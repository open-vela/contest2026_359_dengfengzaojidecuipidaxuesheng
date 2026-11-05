/* SPDX-License-Identifier: Apache-2.0 */
#include "qpk_jbox.h"
#include "glass_portal.h"
#include "jbox_script.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct jbox_state {struct jbox_client *client;unsigned generation;char host[16],device_id[13];};
static atomic_uint generations=0;
static void wipe(void *p,size_t n){volatile uint8_t *v=p;while(n--)*v++=0;}
static bool only(const cJSON *a,const char *keys)
{
  if(!cJSON_IsObject(a))return false;
  for(const cJSON *v=a->child;v;v=v->next){
    if(!v->string||strlen(v->string)>30||cJSON_GetObjectItemCaseSensitive(a,v->string)!=v)return false;
    char token[34];snprintf(token,sizeof(token),",%s,",v->string);if(!strstr(keys,token))return false;
  }
  return true;
}
static int number(const cJSON *a,const char *key,int fallback,int low,int high)
{
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(a,key);if(!v)return fallback;
  if(!cJSON_IsNumber(v)||!(v->valuedouble>=low&&v->valuedouble<=high)||v->valuedouble!=(int)v->valuedouble)return -1;
  return v->valueint;
}
static const char *text(const cJSON *a,const char *key,const char *fallback)
{const cJSON *v=cJSON_GetObjectItemCaseSensitive(a,key);return !v?fallback:cJSON_IsString(v)?v->valuestring:NULL;}
static uint32_t u32(const uint8_t *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p16(uint8_t *p,unsigned n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void p32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static bool ident(const char *s){return s&&strlen(s)==12&&strspn(s,"0123456789abcdefABCDEF")==12;}
static bool host_valid(const char *s)
{struct in_addr a;return s&&strlen(s)<16&&inet_pton(AF_INET,s,&a)==1&&a.s_addr&&
  a.s_addr!=INADDR_BROADCAST&&!IN_MULTICAST(ntohl(a.s_addr));}
static int bytes(const cJSON *a,const char *key,uint8_t *out,unsigned limit)
{
  const cJSON *v=cJSON_GetObjectItemCaseSensitive(a,key);if(!cJSON_IsArray(v))return -EINVAL;
  int n=cJSON_GetArraySize(v);if(n<0||(unsigned)n>limit)return -EINVAL;
  unsigned i=0;for(const cJSON *item=v->child;item;item=item->next){
    if(!cJSON_IsNumber(item)||!(item->valuedouble>=0&&item->valuedouble<=255)||item->valuedouble!=item->valueint)return -EINVAL;
    out[i++]=(uint8_t)item->valueint;
  }return n;
}
static int byte_result(cJSON *o,const char *key,const uint8_t *b,size_t n)
{
  cJSON *a=cJSON_AddArrayToObject(o,key);if(!a)return -ENOMEM;
  for(size_t i=0;i<n;i++){cJSON *v=cJSON_CreateNumber(b[i]);if(!v)return -ENOMEM;cJSON_AddItemToArray(a,v);}return 0;
}
static int nets_result(cJSON *out,const uint8_t *b)
{
  cJSON *nets=cJSON_AddArrayToObject(out,"nets");if(!nets)return -ENOMEM;
  for(unsigned i=0;i<8;i++){
    cJSON *net=cJSON_CreateArray();if(!net)return -ENOMEM;cJSON_AddItemToArray(nets,net);
    for(unsigned node=0;node<52;node++)if(b[i*8+node/8]&(1u<<(node%8))){
      cJSON *v=cJSON_CreateNumber(node);if(!v)return -ENOMEM;cJSON_AddItemToArray(net,v);
    }
  }return 0;
}
void qpk_jbox_destroy(void *opaque)
{struct jbox_state *s=opaque;if(s){jbox_close(s->client);wipe(s,sizeof(*s));free(s);}}
static int bind_board(const char *host,const char *device_id,struct qpk_hw_progress *p)
{
  if(!host_valid(host)||!ident(device_id))return -EINVAL;
  uint8_t key[32]={0};int ret=jbox_pair(host,key,p);
  if(p&&atomic_load(&p->cancel))ret=-ECANCELED;
  if(!ret){
    struct portal_jbox_config config={0};
    snprintf(config.host,sizeof(config.host),"%s",host);
    for(unsigned i=0;i<12;i++)config.device_id[i]=(char)(device_id[i]>='A'&&device_id[i]<='F'?device_id[i]+32:device_id[i]);
    for(unsigned i=0;i<32;i++)snprintf(config.key+i*2,3,"%02x",key[i]);
    ret=portal_jbox_save(&config);wipe(&config,sizeof(config));
  }
  wipe(key,32);return ret;
}
int qpk_jbox_bind(const char *host,const char *device_id)
{return bind_board(host,device_id,NULL);}
static int connect_board(struct jbox_state *s,const cJSON *a,cJSON *out,struct qpk_hw_progress *p)
{
  if(!only(a,",host,,deviceId,,timeoutMs,"))return -EINVAL;
  if(jbox_connected(s->client))return -EISCONN;
  struct portal_jbox_config config={0};int ret=portal_jbox_load(&config);if(ret)return ret;
  const char *host=text(a,"host",config.host),*id=text(a,"deviceId",config.device_id);
  char normalized[13]={0};
  if(ident(id))for(unsigned i=0;i<12;i++)normalized[i]=(char)(id[i]>='A'&&id[i]<='F'?id[i]+32:id[i]);
  int timeout=number(a,"timeoutMs",5000,100,15000);
  if(!host_valid(host)||!ident(id)||strcmp(normalized,config.device_id)||timeout<0){wipe(&config,sizeof(config));return -EINVAL;}
  uint8_t key[32]={0};
  if(strlen(config.key)!=64||strspn(config.key,"0123456789abcdefABCDEF")!=64){wipe(&config,sizeof(config));return -EINVAL;}
  for(unsigned i=0;i<32;i++){char part[3]={config.key[i*2],config.key[i*2+1],0};key[i]=(uint8_t)strtoul(part,NULL,16);}
  unsigned generation=atomic_load(&generations);
  do{if(generation>=INT_MAX){ret=-EOVERFLOW;goto done;}}
  while(!atomic_compare_exchange_weak(&generations,&generation,generation+1));
  jbox_close(s->client);s->client=NULL;
  ret=jbox_connect(&s->client,host,key,(unsigned)timeout,p);
  if(!ret){
    s->generation=generation+1;snprintf(s->host,sizeof(s->host),"%s",host);strcpy(s->device_id,normalized);
    cJSON_AddNumberToObject(out,"generation",s->generation);cJSON_AddBoolToObject(out,"authenticated",true);
    cJSON_AddStringToObject(out,"host",s->host);cJSON_AddStringToObject(out,"deviceId",s->device_id);
  }
  if(p&&atomic_load(&p->cancel)){jbox_close(s->client);s->client=NULL;ret=-ECANCELED;}
done:wipe(key,32);wipe(&config,sizeof(config));return ret;
}
/* Construct only documented commands. There is no arbitrary command/key API. */
static int encode(const char *op,const cJSON *a,uint8_t *b,size_t *n,uint8_t *cmd)
{
  *n=0;
  const char *plain[]={"caps","status","safe","routeClear"};const uint8_t ids[]={0x10,0x11,0x70,0x23};
  for(unsigned i=0;i<4;i++)if(!strcmp(op,plain[i])){if(!only(a,",generation,"))return -EINVAL;*cmd=ids[i];return 0;}
  if(!strcmp(op,"routeSet")){
    if(!only(a,",generation,,nets,"))return -EINVAL;
    const cJSON *nets=cJSON_GetObjectItemCaseSensitive(a,"nets");if(!cJSON_IsArray(nets)||cJSON_GetArraySize(nets)>8)return -EINVAL;
    memset(b,0,64);uint64_t seen=0;unsigned i=0;
    for(const cJSON *net=nets->child;net;net=net->next,i++){
      if(!cJSON_IsArray(net))return -EINVAL;
      for(const cJSON *node=net->child;node;node=node->next){
        if(!cJSON_IsNumber(node)||!(node->valuedouble>=0&&node->valuedouble<52)||node->valuedouble!=node->valueint)return -EINVAL;
        unsigned j=(unsigned)node->valueint;if(seen&(UINT64_C(1)<<j))return -EINVAL;
        seen|=UINT64_C(1)<<j;b[i*8+j/8]|=(uint8_t)(1u<<(j%8));
      }
    }*cmd=0x20;*n=64;return 0;
  }
  if(!strcmp(op,"connectNodes")){
    int x=number(a,"a",-1,0,51),y=number(a,"b",-1,0,51);
    if(!only(a,",generation,,a,,b,")||x<0||y<0||x==y)return -EINVAL;
    *cmd=0x21;b[0]=x;b[1]=y;*n=2;return 0;
  }
  if(!strcmp(op,"removeNode")||!strcmp(op,"adcRead")){
    bool route=!strcmp(op,"removeNode");int io=number(a,route?"node":"io",-1,0,route?51:17);
    if(!only(a,route?",generation,,node,":",generation,,io,")||io<0)return -EINVAL;
    *cmd=route?0x22:0x34;b[0]=io;*n=1;return 0;
  }
  if(!strcmp(op,"gpioConfig")){
    int io=number(a,"io",-1,0,17),mode=number(a,"mode",0,0,3),pull=number(a,"pull",0,0,2),initial=number(a,"initial",0,0,1);
    if(!only(a,",generation,,io,,mode,,pull,,initial,")||io<0||mode<0||pull<0||initial<0)return -EINVAL;
    *cmd=0x30;b[0]=io;b[1]=mode;b[2]=pull;b[3]=initial;*n=4;return 0;
  }
  if(!strcmp(op,"gpioWrite")||!strcmp(op,"gpioRead")){
    bool write=!strcmp(op,"gpioWrite");int mask=number(a,"mask",write?-1:0x3ffff,0,0x3ffff),levels=number(a,"levels",write?-1:0,0,0x3ffff);
    if(!only(a,write?",generation,,mask,,levels,":",generation,,mask,")||mask<0||levels<0||(write&&(levels&~mask)))return -EINVAL;
    *cmd=write?0x31:0x32;p32(b,mask);p32(b+4,levels);*n=write?8:4;return 0;
  }
  if(!strcmp(op,"pwm")){
    int ch=number(a,"channel",-1,0,3),io=number(a,"io",-1,0,255),hz=number(a,"hz",1000,10,40000),duty=number(a,"duty",4096,0,8191);
    if(!only(a,",generation,,channel,,io,,hz,,duty,")||ch<0||io<0||(io>17&&io!=255)||hz<0||duty<0)return -EINVAL;
    *cmd=0x33;b[0]=ch;b[1]=io;p32(b+2,hz);p16(b+6,duty);*n=8;return 0;
  }
  int bus=number(a,"bus",-1,0,1);
  if(!strcmp(op,"i2cClose")||!strcmp(op,"spiClose")||!strcmp(op,"uartClose")){
    if(!only(a,",generation,,bus,")||bus<0)return -EINVAL;
    *cmd=!strcmp(op,"i2cClose")?0x41:!strcmp(op,"spiClose")?0x52:0x63;b[0]=bus;*n=1;return 0;
  }
  if(!strcmp(op,"i2cSetup")){
    int sda=number(a,"sda",-1,0,17),scl=number(a,"scl",-1,0,17),hz=number(a,"hz",50000,10000,400000);
    if(!only(a,",generation,,bus,,sda,,scl,,hz,")||bus<0||sda<0||scl<0||sda==scl||hz<0)return -EINVAL;
    *cmd=0x40;b[0]=bus;b[1]=sda;b[2]=scl;p32(b+3,hz);*n=7;return 0;
  }
  if(!strcmp(op,"i2cTransfer")){
    int addr=number(a,"address",-1,8,119),read=number(a,"read",0,0,4096),written=bytes(a,"data",b+6,4096);
    if(!only(a,",generation,,bus,,address,,data,,read,")||bus<0||addr<0||read<0||written<0||read+written>4096)return -EINVAL;
    *cmd=0x42;b[0]=bus;b[1]=addr;p16(b+2,written);p16(b+4,read);*n=6+(size_t)written;return 0;
  }
  if(!strcmp(op,"spiSetup")){
    const char *keys[]={"mosi","miso","sck","cs"};int pins[4];
    for(unsigned i=0;i<4;i++){pins[i]=number(a,keys[i],-1,0,i==1?255:17);if(pins[i]<0||(pins[i]>17&&pins[i]!=255))return -EINVAL;}
    int mode=number(a,"mode",0,0,3),hz=number(a,"hz",100000,1000,20000000);
    if(!only(a,",generation,,bus,,mosi,,miso,,sck,,cs,,mode,,hz,")||bus<0||mode<0||hz<0)return -EINVAL;
    for(unsigned i=0;i<4;i++)for(unsigned j=i+1;j<4;j++)if(pins[i]==pins[j])return -EINVAL;
    for(unsigned i=0;i<4;i++)if(pins[i]<9&&hz>2000000)return -EINVAL;
    *cmd=0x50;b[0]=bus;for(unsigned i=0;i<4;i++)b[1+i]=pins[i];b[5]=mode;p32(b+6,hz);*n=10;return 0;
  }
  if(!strcmp(op,"spiTransfer")||!strcmp(op,"uartWrite")){
    bool spi=!strcmp(op,"spiTransfer");int count=bytes(a,"data",b+(spi?3:1),4096);
    if(!only(a,",generation,,bus,,data,")||bus<0||count<0||(spi&&!count))return -EINVAL;
    *cmd=spi?0x51:0x61;b[0]=bus;if(spi)p16(b+1,count);*n=(spi?3:1)+(size_t)count;return 0;
  }
  if(!strcmp(op,"uartSetup")){
    int tx=number(a,"tx",-1,0,17),rx=number(a,"rx",-1,0,17),baud=number(a,"baud",115200,300,921600),parity=number(a,"parity",0,0,2),stop=number(a,"stopbits",1,1,2);
    if(!only(a,",generation,,bus,,tx,,rx,,baud,,parity,,stopbits,")||bus<0||tx<0||rx<0||tx==rx||baud<0||parity<0||stop<0||((tx<9||rx<9)&&baud>460800))return -EINVAL;
    *cmd=0x60;b[0]=bus;b[1]=tx;b[2]=rx;p32(b+3,baud);b[7]=parity;b[8]=stop;*n=9;return 0;
  }
  if(!strcmp(op,"uartRead")){
    int count=number(a,"length",512,0,4096),timeout=number(a,"timeoutMs",100,0,500);
    if(!only(a,",generation,,bus,,length,,timeoutMs,")||bus<0||count<0||timeout<0)return -EINVAL;
    *cmd=0x62;b[0]=bus;p16(b+1,count);p16(b+3,timeout);*n=5;return 0;
  }
  if(!strcmp(op,"functionLoad")){
    int slot=number(a,"slot",0,0,3),args=number(a,"arguments",0,0,4),count=bytes(a,"code",b+4,4096);
    if(!only(a,",generation,,slot,,arguments,,code,")||slot<0||args<0||count<4||count%4)return -EINVAL;
    *cmd=0x80;b[0]=slot;b[1]=args;p16(b+2,count/4);*n=4+(size_t)count;return 0;
  }
  if(!strcmp(op,"functionCall")||!strcmp(op,"functionClear")){
    bool call=!strcmp(op,"functionCall");int slot=number(a,"slot",0,0,3);
    if(!only(a,call?",generation,,slot,,ios,,delayMs,":",generation,,slot,")||slot<0||
       (call&&number(a,"delayMs",0,0,5000)<0))return -EINVAL;
    *cmd=call?0x81:0x82;b[0]=slot;*n=1;
    if(call){int count=bytes(a,"ios",b+2,4);if(count<0)return -EINVAL;unsigned seen=0;
      for(int i=0;i<count;i++){if(b[2+i]>17||(seen&(1u<<b[2+i])))return -EINVAL;seen|=1u<<b[2+i];}
      b[1]=count;*n=2+(size_t)count;}
    return 0;
  }
  return -ENOSYS;
}
static int decode(uint8_t cmd,const uint8_t *request,const uint8_t *b,size_t n,cJSON *out)
{
  if(cmd==0x10){
    if(n!=26||b[0]!=1||b[1]!=52||b[2]!=8||b[3]!=18||
       (b[4]|(unsigned)b[5]<<8)!=4096||(b[6]|(unsigned)b[7]<<8)!=JBOX_PORT)return -EPROTO;
    cJSON_AddNumberToObject(out,"version",b[0]);cJSON_AddNumberToObject(out,"nodes",b[1]);
    cJSON_AddNumberToObject(out,"nets",b[2]);cJSON_AddNumberToObject(out,"ioCount",b[3]);
    cJSON_AddNumberToObject(out,"maxTransfer",b[4]|(unsigned)b[5]<<8);
    int ret=byte_result(out,"ioGpios",b+8,18);if(ret)return ret;
    cJSON *map=cJSON_AddObjectToObject(out,"boardMap");if(!map)return -ENOMEM;
    cJSON_AddStringToObject(map,"format","CN rows are [oddPin,evenPin,matrixNode]; each odd/even pair is internally joined");
    for(unsigned connector=0;connector<2;connector++){
      cJSON *rows=cJSON_AddArrayToObject(map,connector?"CN2":"CN1");if(!rows)return -ENOMEM;
      for(unsigned pair=0;pair<20;pair++){
        int values[3]={(int)(pair*2+1),(int)(pair*2+2),(int)(connector*20+pair)};
        cJSON *row=cJSON_CreateIntArray(values,3);if(!row)return -ENOMEM;cJSON_AddItemToArray(rows,row);
      }
    }
    cJSON_AddStringToObject(map,"external","node40..48=EXT1..9=IO0..8; actual S3 GPIO from ioGpios[io]");
    cJSON_AddStringToObject(map,"power","node49=5V,node50=3.3V,node51=GND; never mix power rails or route 5V to a signal IO");
    cJSON_AddStringToObject(out,"jboxScript","functionCompile accepts bounded source or program=dht11; uploads only, never runs. functionCall uses IO ids. DHT11 returns [temperature*10,humidity*10]; >=2s between reads. RMT opcodes 34..37 require updated S3 firmware; remote capability is not reported by v1 caps.");
    return 0;
  }
  if(cmd==0x11){
    if(n!=108)return -EPROTO;
    cJSON_AddNumberToObject(out,"uptimeMs",u32(b));cJSON_AddNumberToObject(out,"heapFree",u32(b+4));
    int ret=nets_result(out,b+8);if(!ret)ret=byte_result(out,"ioMode",b+72,18);if(!ret)ret=byte_result(out,"ioLease",b+90,18);return ret;
  }
  if(cmd==0x20||cmd==0x21||cmd==0x22){
    if(n!=(cmd==0x21?65:64)||(cmd==0x21&&b[0]>=8))return -EPROTO;
    if(cmd==0x21)cJSON_AddNumberToObject(out,"net",b[0]);
    return nets_result(out,b+(cmd==0x21?1:0));
  }
  if(cmd==0x32||cmd==0x34){if(n!=4)return -EPROTO;cJSON_AddNumberToObject(out,cmd==0x32?"levels":"raw",u32(b));return 0;}
  if(cmd==0x42||cmd==0x51||cmd==0x62){
    unsigned expected=cmd==0x42?request[4]|(unsigned)request[5]<<8:request[1]|(unsigned)request[2]<<8;
    if(n>4096||(cmd==0x62?n>expected:n!=expected))return -EPROTO;
    cJSON_AddNumberToObject(out,"bytes",n);return byte_result(out,"data",b,n);
  }
  if(cmd==0x61){if(n!=2)return -EPROTO;cJSON_AddNumberToObject(out,"bytes",b[0]|(unsigned)b[1]<<8);return 0;}
  if(cmd==0x81){
    if(n<2)return -EPROTO;
    unsigned count=b[0]|(unsigned)b[1]<<8;
    if(count>128||n!=2+count*4)return -EPROTO;
    cJSON *a=cJSON_AddArrayToObject(out,"values");if(!a)return -ENOMEM;
    for(unsigned i=0;i<count;i++){cJSON *v=cJSON_CreateNumber(u32(b+2+i*4));if(!v)return -ENOMEM;cJSON_AddItemToArray(a,v);}return 0;
  }
  return n?-EPROTO:0;
}
static int execute(void **opaque,const char *op,const cJSON *a,cJSON *out,struct qpk_hw_progress *p)
{
  if(!opaque||!op||!out||!cJSON_IsObject(a))return -EINVAL;
  if(p&&atomic_load(&p->cancel))return -ECANCELED;
  if(!strcmp(op,"discover")){
    int timeout=number(a,"timeoutMs",1500,100,5000);if(!only(a,",timeoutMs,")||timeout<0)return -EINVAL;
    return jbox_discover(out,(unsigned)timeout,p);
  }
  if(!strcmp(op,"pair")){
    if(!only(a,",host,,deviceId,"))return -EINVAL;
    const char *host=text(a,"host",NULL),*id=text(a,"deviceId",NULL);
    if(!host_valid(host)||!ident(id))return -EINVAL;
    if(*opaque){struct jbox_state *old=*opaque;jbox_close(old->client);old->client=NULL;}
    int ret=bind_board(host,id,p);
    if(!ret)cJSON_AddBoolToObject(out,"saved",true);
    return ret;
  }
  struct jbox_state *s=*opaque;
  if(!strcmp(op,"connect")){
    if(!s){s=calloc(1,sizeof(*s));if(!s)return -ENOMEM;*opaque=s;}
    return connect_board(s,a,out,p);
  }
  if(!s||!jbox_connected(s->client))return -ENOTCONN;
  int generation=number(a,"generation",-1,1,INT_MAX);
  if(generation<0)return -EINVAL;
  if((unsigned)generation!=s->generation)return -ESTALE;
  if(!strcmp(op,"disconnect")){
    if(!only(a,",generation,"))return -EINVAL;
    jbox_close(s->client);s->client=NULL;cJSON_AddBoolToObject(out,"disconnected",true);return 0;
  }
  uint8_t *request=malloc(JBOX_MAX_PAYLOAD),*reply=malloc(JBOX_MAX_PAYLOAD);
  if(!request||!reply){free(request);free(reply);return -ENOMEM;}
  uint8_t cmd=0;size_t n=0,received=0;int32_t remote=0;
  struct jbox_script_program *compiled=NULL;
  int ret=0;
  if(!strcmp(op,"functionCompile")){
    const char *source=text(a,"source",NULL),*program=text(a,"program",NULL);
    int slot=number(a,"slot",0,0,3);
    if(!only(a,",generation,,slot,,source,,program,")||slot<0||
       (!!cJSON_GetObjectItemCaseSensitive(a,"source")+!!cJSON_GetObjectItemCaseSensitive(a,"program"))!=1)
      ret=-EINVAL;
    if(!ret&&program){source=jbox_script_example(program);if(!source)ret=-EINVAL;}
    if(!ret&&!source)ret=-EINVAL;
    if(!ret){compiled=calloc(1,sizeof(*compiled));if(!compiled)ret=-ENOMEM;}
    if(!ret)ret=jbox_script_compile(source,compiled);
    if(ret&&compiled)cJSON_AddNumberToObject(out,"compileErrorOffset",compiled->error_offset);
    if(!ret){
      cmd=0x80;request[0]=slot;request[1]=compiled->arguments;p16(request+2,compiled->count);
      memcpy(request+4,compiled->code,compiled->count*4);n=4+compiled->count*4;
      if(n>JBOX_MAX_PAYLOAD)ret=-E2BIG;
    }
  }else ret=encode(op,a,request,&n,&cmd);
  if(!ret&&p&&atomic_load(&p->cancel))ret=-ECANCELED;
  if(!ret&&!strcmp(op,"functionCall")){
    unsigned delay=(unsigned)number(a,"delayMs",0,0,5000);
    for(unsigned elapsed=0;elapsed<delay;){
      if(p&&atomic_load(&p->cancel)){ret=-ECANCELED;break;}
      unsigned step=delay-elapsed>20?20:delay-elapsed;
      struct timespec wait={0,(long)step*1000000};
      while(nanosleep(&wait,&wait)<0&&errno==EINTR){
        if(p&&atomic_load(&p->cancel)){ret=-ECANCELED;break;}
      }
      if(ret)break;
      elapsed+=step;
    }
  }
  if(!ret)ret=jbox_rpc(s->client,cmd,request,n,reply,JBOX_MAX_PAYLOAD,&received,&remote,p);
  if(!ret&&remote){cJSON_AddNumberToObject(out,"remoteStatus",remote);ret=-EREMOTEIO;}
  else if(!ret){ret=decode(cmd,request,reply,received,out);if(ret==-EPROTO){jbox_close(s->client);s->client=NULL;}}
  if(p&&atomic_load(&p->cancel)){jbox_close(s->client);s->client=NULL;ret=-ECANCELED;}
  if(!ret&&compiled){
    cJSON_AddBoolToObject(out,"compiled",true);cJSON_AddBoolToObject(out,"uploaded",true);
    cJSON_AddBoolToObject(out,"executed",false);
    cJSON_AddNumberToObject(out,"slot",request[0]);cJSON_AddStringToObject(out,"name",compiled->name);
    cJSON_AddNumberToObject(out,"instructions",compiled->count);
    cJSON *parameters=cJSON_AddArrayToObject(out,"parameters");
    if(!parameters)ret=-ENOMEM;
    else for(unsigned i=0;i<compiled->arguments;i++){
      cJSON *parameter=cJSON_CreateString(compiled->parameters[i]);if(!parameter){ret=-ENOMEM;break;}
      cJSON_AddItemToArray(parameters,parameter);
    }
    if(text(a,"program",NULL))cJSON_AddStringToObject(out,"resultUnits","[temperatureC_x10,humidityPercent_x10]; only a successful functionCall yields measurements");
  }
  free(compiled);
  cJSON_AddNumberToObject(out,"generation",s->generation);
  cJSON_AddBoolToObject(out,"connected",jbox_connected(s->client));
  free(request);free(reply);return ret;
}
int qpk_jbox_execute(void **opaque,const char *op,const cJSON *a,cJSON *out,struct qpk_hw_progress *p)
{
  struct timespec start,end;clock_gettime(CLOCK_MONOTONIC,&start);
  int ret=execute(opaque,op,a,out,p);clock_gettime(CLOCK_MONOTONIC,&end);
  if(ret==-ECANCELED&&opaque&&*opaque){struct jbox_state *s=*opaque;jbox_close(s->client);s->client=NULL;}
  if(p){int64_t ms=(int64_t)(end.tv_sec-start.tv_sec)*1000+(end.tv_nsec-start.tv_nsec)/1000000;
    atomic_store(&p->elapsed_ms,(unsigned)ms);}
  return ret;
}
