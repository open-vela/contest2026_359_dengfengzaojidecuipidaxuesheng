/* SPDX-License-Identifier: Apache-2.0 */
/* Real Agent, strict schemas and async scheduler; no network or real device. */
#include "glass_agent.h"
#include "glass_board.h"
#include "qpk_hardware.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_uint executed,destroyed;
const char *glass_qpk_guide(void){return "fixture";}
int qpk_storage_read(const char *r,const char *p,const char *k,char *o,size_t c,size_t *n)
{(void)r;(void)p;(void)k;(void)o;(void)c;(void)n;return -ENOENT;}
int qpk_storage_write(const char *r,const char *p,const char *k,const char *d,size_t n)
{(void)r;(void)p;(void)k;(void)d;(void)n;assert(!"Unexpected config write");return -EPERM;}
int glass_mcp_http_perform(struct glass_mcp_http *request)
{(void)request;assert(!"Unexpected network request");return -EPERM;}
char *glass_board_ui(const char *op,const cJSON *args,atomic_bool *cancel)
{(void)op;(void)args;(void)cancel;assert(!"Unexpected UI call");return NULL;}
void *qpk_hw_platform_create(const char *package)
{assert(!strcmp(package,"com.openvela.espclaw.control"));return malloc(1);}
void *qpk_hw_platform_create_at(const char *package,const char *root)
{(void)root;return qpk_hw_platform_create(package);}
void qpk_hw_platform_destroy(void *p){free(p);atomic_fetch_add(&destroyed,1);}
char *qpk_hw_capabilities(void){return strdup("{}");}
int qpk_hw_platform_execute(void *p,const char *op,const cJSON *args,cJSON *out,struct qpk_hw_progress *progress)
{
  (void)p;assert(!strncmp(op,"jbox.",5)&&strcmp(op,"jbox.pair"));atomic_fetch_add(&executed,1);
  const cJSON *generation=cJSON_GetObjectItemCaseSensitive(args,"generation");
  if(!strcmp(op,"jbox.status")&&generation&&generation->valueint==999){
    while(!atomic_load(&progress->cancel))usleep(1000);
    return -ECANCELED;
  }
  cJSON_AddStringToObject(out,"operation",op);
  cJSON_AddItemToObject(out,"arguments",cJSON_Duplicate(args,true));return 0;
}
static cJSON *invoke(struct glass_agent *a,const char *name,const char *text,bool mcp,int error)
{
  cJSON *args=cJSON_Parse(text);assert(args);char *out=NULL;
  cJSON *wrapper=NULL;
  if(mcp){wrapper=cJSON_CreateObject();cJSON_AddStringToObject(wrapper,"server","board");
    cJSON_AddStringToObject(wrapper,"tool",name);cJSON_AddItemToObject(wrapper,"arguments",args);args=wrapper;}
  assert(glass_agent_call(a,mcp?"mcp_call_tool":name,args,&out)==1);cJSON_Delete(args);assert(out);
  cJSON *r=cJSON_Parse(out);free(out);assert(r);
  if(error>=0&&(cJSON_GetObjectItemCaseSensitive(r,"error")!=NULL)!=error){
    char *detail=cJSON_PrintUnformatted(r);fprintf(stderr,"%s mcp=%u expected_error=%d: %s\n",name,mcp,error,detail);free(detail);assert(!"Unexpected tool receipt");
  }return r;
}
static cJSON *poll(struct glass_agent *a,unsigned id,bool mcp)
{
  for(unsigned i=0;i<20;i++){
    char args[64];snprintf(args,sizeof(args),"{\"id\":%u}",id);
    cJSON *r=invoke(a,"hardware_poll",args,mcp,-1);
    if(cJSON_IsTrue(cJSON_GetObjectItem(r,"done")))return r;
    cJSON_Delete(r);
  }assert(!"Job did not finish");return NULL;
}
static cJSON *discover(struct glass_agent *a)
{
  char cursor[32]="";cJSON *all=cJSON_CreateArray();unsigned pages=0;
  do{
    char args[128];snprintf(args,sizeof(args),"{\"server\":\"board\",\"cursor\":\"%s\"}",cursor);
    if(!cursor[0])strcpy(args,"{\"server\":\"board\"}");
    cJSON *input=cJSON_Parse(args);char *out=NULL;assert(glass_agent_call(a,"mcp_list_tools",input,&out)==1);cJSON_Delete(input);
    assert(out&&strlen(out)<=AGENT_TOOL_BYTES);cJSON *r=cJSON_Parse(out);free(out);assert(r);
    cJSON *tools=cJSON_GetObjectItem(r,"tools");assert(cJSON_IsArray(tools)&&cJSON_GetArraySize(tools));
    while(tools->child)cJSON_AddItemToArray(all,cJSON_DetachItemFromArray(tools,0));
    const cJSON *next=cJSON_GetObjectItem(r,"nextCursor");snprintf(cursor,sizeof(cursor),"%s",cJSON_IsString(next)?next->valuestring:"");
    cJSON_Delete(r);assert(++pages<10);
  }while(cursor[0]);
  assert(cJSON_GetArraySize(all)==39);return all;
}
static const struct {const char *name,*operation,*args;} cases[]={
  {"jbox_discover","jbox.discover","{\"timeoutMs\":100}"},
  {"jbox_connect","jbox.connect","{\"host\":\"192.168.1.100\",\"deviceId\":\"aabbccddeeff\",\"timeoutMs\":100}"},
  {"jbox_disconnect","jbox.disconnect","{\"generation\":1}"},
  {"jbox_caps","jbox.caps","{\"generation\":1}"},
  {"jbox_status","jbox.status","{\"generation\":1}"},
  {"jbox_safe","jbox.safe","{\"generation\":1}"},
  {"jbox_route_set","jbox.routeSet","{\"generation\":1,\"nets\":[[40,0],[41,1]]}"},
  {"jbox_connect_nodes","jbox.connectNodes","{\"generation\":1,\"a\":40,\"b\":0}"},
  {"jbox_remove_node","jbox.removeNode","{\"generation\":1,\"node\":40}"},
  {"jbox_route_clear","jbox.routeClear","{\"generation\":1}"},
  {"jbox_gpio_config","jbox.gpioConfig","{\"generation\":1,\"io\":0,\"mode\":0,\"pull\":0,\"initial\":0}"},
  {"jbox_gpio_write","jbox.gpioWrite","{\"generation\":1,\"mask\":1,\"levels\":1}"},
  {"jbox_gpio_read","jbox.gpioRead","{\"generation\":1,\"mask\":262143}"},
  {"jbox_adc_read","jbox.adcRead","{\"generation\":1,\"io\":0}"},
  {"jbox_pwm","jbox.pwm","{\"generation\":1,\"channel\":0,\"io\":255,\"hz\":1000,\"duty\":0}"},
  {"jbox_i2c_setup","jbox.i2cSetup","{\"generation\":1,\"bus\":0,\"sda\":0,\"scl\":1,\"hz\":50000}"},
  {"jbox_i2c_transfer","jbox.i2cTransfer","{\"generation\":1,\"bus\":0,\"address\":64,\"data\":[],\"read\":2}"},
  {"jbox_i2c_close","jbox.i2cClose","{\"generation\":1,\"bus\":0}"},
  {"jbox_spi_setup","jbox.spiSetup","{\"generation\":1,\"bus\":1,\"mosi\":0,\"miso\":255,\"sck\":1,\"cs\":2,\"mode\":0,\"hz\":100000}"},
  {"jbox_spi_transfer","jbox.spiTransfer","{\"generation\":1,\"bus\":1,\"data\":[1,255]}"},
  {"jbox_spi_close","jbox.spiClose","{\"generation\":1,\"bus\":1}"},
  {"jbox_uart_setup","jbox.uartSetup","{\"generation\":1,\"bus\":0,\"tx\":0,\"rx\":1,\"baud\":115200,\"parity\":0,\"stopbits\":1}"},
  {"jbox_uart_write","jbox.uartWrite","{\"generation\":1,\"bus\":0,\"data\":[65,66]}"},
  {"jbox_uart_read","jbox.uartRead","{\"generation\":1,\"bus\":0,\"length\":512,\"timeoutMs\":100}"},
  {"jbox_uart_close","jbox.uartClose","{\"generation\":1,\"bus\":0}"},
  {"jbox_function_load","jbox.functionLoad","{\"generation\":1,\"slot\":0,\"arguments\":0,\"code\":[0,0,0,0]}"},
  {"jbox_function_call","jbox.functionCall","{\"generation\":1,\"slot\":0,\"ios\":[0,1]}"},
  {"jbox_function_clear","jbox.functionClear","{\"generation\":1,\"slot\":0}"}
};
int main(void)
{
  atomic_bool cancel=false;struct glass_agent *a=glass_agent_create(&cancel,AGENT_MODE_ASSISTANT);assert(a);
  cJSON_Delete(invoke(a,"jbox_status","{\"generation\":1}",true,true));assert(!atomic_load(&executed));
  cJSON *catalog=discover(a);char *advertised=glass_agent_tools(a,"[]");assert(advertised);
  cJSON *direct=cJSON_Parse(advertised);assert(direct&&!strstr(advertised,"jbox_pair"));free(advertised);
  for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
    const cJSON *definition=NULL,*parameters=NULL;unsigned found=0;
    for(const cJSON *t=catalog->child;t;t=t->next)if(!strcmp(cJSON_GetObjectItem(t,"name")->valuestring,cases[i].name)){definition=t;found++;}
    assert(found==1&&definition);
    for(const cJSON *t=direct->child;t;t=t->next){const cJSON *fn=cJSON_GetObjectItem(t,"function");
      if(!strcmp(cJSON_GetObjectItem(fn,"name")->valuestring,cases[i].name))parameters=cJSON_GetObjectItem(fn,"parameters");}
    assert(parameters&&cJSON_Compare(parameters,cJSON_GetObjectItem(definition,"inputSchema"),true));
    for(unsigned mcp=0;mcp<2;mcp++){
      cJSON *r=invoke(a,cases[i].name,cases[i].args,mcp,false);assert(cJSON_IsTrue(cJSON_GetObjectItem(r,"ok")));
      unsigned id=cJSON_GetObjectItem(r,"id")->valueint;assert(!cJSON_GetObjectItem(r,"done"));cJSON_Delete(r);
      r=poll(a,id,!mcp);assert(cJSON_IsTrue(cJSON_GetObjectItem(r,"ok")));
      cJSON *result=cJSON_GetObjectItem(r,"result"),*expected=cJSON_Parse(cases[i].args);
      assert(!strcmp(cJSON_GetObjectItem(result,"operation")->valuestring,cases[i].operation));
      assert(cJSON_Compare(expected,cJSON_GetObjectItem(result,"arguments"),true));cJSON_Delete(expected);cJSON_Delete(r);
    }
  }
  cJSON_Delete(direct);cJSON_Delete(catalog);assert(atomic_load(&executed)==56);
  const struct {const char *name,*args;} bad[]={
    {"jbox_pair","{}"},{"jbox_factory_reset","{}"},{"jbox_status","{}"},
    {"jbox_status","{\"generation\":0}"},{"jbox_status","{\"generation\":1.5}"},
    {"jbox_status","{\"generation\":1e1000}"},{"jbox_status","{\"generation\":true}"},
    {"jbox_status","{\"generation\":1,\"generation\":2}"},{"jbox_status","{\"generation\":1,\"key\":\"secret\"}"},
    {"jbox_status","null"},{"jbox_status","[]"},
    {"jbox_discover","{\"timeoutMs\":99}"},{"jbox_discover","{\"timeoutMs\":5001}"},
    {"jbox_connect","{\"host\":123}"},{"jbox_connect","{\"deviceId\":\"short\"}"},
    {"jbox_gpio_config","{\"generation\":1,\"io\":18}"},
    {"jbox_pwm","{\"generation\":1,\"channel\":0,\"io\":254}"},
    {"jbox_route_set","{\"generation\":1,\"nets\":[[52]]}"},
    {"jbox_route_set","{\"generation\":1,\"nets\":[[0,0]]}"},
    {"jbox_spi_transfer","{\"generation\":1,\"bus\":0,\"data\":[]}"},
    {"jbox_spi_transfer","{\"generation\":1,\"bus\":0,\"data\":[256]}"},
    {"jbox_spi_transfer","{\"generation\":1,\"bus\":0,\"data\":[1.5]}"},
    {"jbox_function_call","{\"generation\":1,\"ios\":[0,0]}"},
    {"jbox_function_load","{\"generation\":1,\"code\":[0,0,0]}"}
  };
  for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)for(unsigned mcp=0;mcp<2;mcp++)cJSON_Delete(invoke(a,bad[i].name,bad[i].args,mcp,true));
  assert(atomic_load(&executed)==56);
  struct glass_agent *other=glass_agent_create(&cancel,AGENT_MODE_ASSISTANT);assert(other);
  cJSON_Delete(invoke(other,"jbox_status","{\"generation\":1}",false,true));glass_agent_destroy(other);
  /* Cancel the actual queued/running scheduler job; no replay is submitted. */
  cJSON *r=invoke(a,"jbox_status","{\"generation\":999}",false,false);unsigned id=cJSON_GetObjectItem(r,"id")->valueint;cJSON_Delete(r);
  char args[64];snprintf(args,sizeof(args),"{\"id\":%u}",id);cJSON_Delete(invoke(a,"hardware_stop",args,true,false));
  r=poll(a,id,false);assert(cJSON_IsFalse(cJSON_GetObjectItem(r,"ok")));cJSON_Delete(r);
  unsigned before=atomic_load(&executed);atomic_store(&cancel,true);
  cJSON_Delete(invoke(a,"jbox_status","{\"generation\":1}",false,true));assert(atomic_load(&executed)==before);atomic_store(&cancel,false);
  for(unsigned i=0;i<8;i++)cJSON_Delete(invoke(a,"jbox_status","{\"generation\":1}",false,false));
  cJSON_Delete(invoke(a,"jbox_status","{\"generation\":1}",false,true));
  cJSON_Delete(invoke(a,"hardware_release","{}",false,false));glass_agent_destroy(a);
  for(unsigned mode=0;mode<AGENT_MODE_COUNT;mode++)if(mode!=AGENT_MODE_ASSISTANT){
    a=glass_agent_create(&cancel,mode);assert(a);advertised=glass_agent_tools(a,"[]");assert(advertised&&!strstr(advertised,"jbox_"));free(advertised);
    cJSON_Delete(invoke(a,"jbox_status","{\"generation\":1}",false,true));
    cJSON_Delete(invoke(a,"hardware_poll","{\"id\":1}",false,true));
    cJSON_Delete(invoke(a,"jbox_status","{\"generation\":1}",true,true));glass_agent_destroy(a);
  }
  for(unsigned i=0;i<100&&!atomic_load(&destroyed);i++)usleep(10000);assert(atomic_load(&destroyed));
  puts("JBOX_TOOLS_OK: 28 tools via direct and MCP routes; identical schemas; bounded pagination; strict rejection; mode/owner/job limits; cancellation and cleanup; no network/config writes");
  return 0;
}
