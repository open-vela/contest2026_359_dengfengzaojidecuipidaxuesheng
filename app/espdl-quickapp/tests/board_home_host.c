/* SPDX-License-Identifier: Apache-2.0
 * Real in-process home tools over the real shared HA service and the real
 * desktop agent, with a scripted HTTP transport. Proves card results, the
 * states cache, read-back verification, the service whitelist, argument
 * injection rejection, and that no page-opening UI helper is ever called. */
#include "glass_agent.h"
#include "glass_board_home.h"
#include "glass_portal.h"
#include "hass_service.h"
#include "hass_transport.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern unsigned glass_board_ui_calls;

/* Link stubs the desktop app normally provides. */
const char *glass_qpk_guide(void){return "Validated quick app guide";}
static char *agent_config;
int qpk_storage_read(const char *root,const char *package,const char *key,char *out,size_t capacity,size_t *length)
{(void)root;(void)package;if(strcmp(key,"agent")||!agent_config)return -ENOENT;
 assert(strlen(agent_config)<capacity);strcpy(out,agent_config);*length=strlen(out);return 0;}
int qpk_storage_write(const char *root,const char *package,const char *key,const char *data,size_t size)
{(void)root;(void)package;assert(!strcmp(key,"agent"));free(agent_config);agent_config=strndup(data,size);return agent_config?0:-ENOMEM;}
int glass_mcp_http_perform(struct glass_mcp_http *request)
{(void)request;assert(!"no external MCP server in this fixture");return -1;}

/* Scripted Home Assistant transport: every submission must have exactly one
 * queued response, and poll transfers it once. */
struct fake_response {int status;int error;char *body;};
static struct fake_response queue[256];
static unsigned head,tail;
static bool pending;
static unsigned get_calls,state_reads,service_calls;
static char last_resource[32],last_entity[96],last_domain[32],last_action[32],last_data[1024];

static void fake_queue(int status,int error,const char *body)
{
  assert(tail<sizeof(queue)/sizeof(queue[0]));
  queue[tail].status=status;queue[tail].error=error;queue[tail].body=body?strdup(body):NULL;
  assert(!body||queue[tail].body);tail++;
}
static unsigned fake_depth(void){return tail-head;}
static int fake_start(void){assert(!pending&&head<tail);pending=true;return 0;}
int hass_transport_get(const char *base,const char *token,const char *resource)
{(void)token;assert(!strcmp(base,"http://home.local:8123"));snprintf(last_resource,sizeof(last_resource),"%s",resource);
 get_calls++;return fake_start();}
int hass_transport_get_state(const char *base,const char *token,const char *entity)
{(void)token;assert(!strcmp(base,"http://home.local:8123"));snprintf(last_entity,sizeof(last_entity),"%s",entity);
 state_reads++;return fake_start();}
int hass_transport_call_service(const char *base,const char *token,const char *domain,const char *service,const char *data)
{(void)token;assert(!strcmp(base,"http://home.local:8123"));
 snprintf(last_domain,sizeof(last_domain),"%s",domain);snprintf(last_action,sizeof(last_action),"%s",service);
 snprintf(last_data,sizeof(last_data),"%s",data);service_calls++;return fake_start();}
void hass_transport_poll(struct hass_transport_result_s *result)
{
  memset(result,0,sizeof(*result));
  if(!pending)return;
  struct fake_response *entry=&queue[head++];
  pending=false;
  result->done=true;result->status=entry->status;result->error=entry->error;
  result->body=entry->body;entry->body=NULL;
}
bool hass_transport_busy(void){return pending;}
void hass_transport_stop(void)
{
  if(pending){free(queue[head].body);queue[head].body=NULL;head++;pending=false;}
}

static cJSON *call(struct glass_agent *agent,const char *name,const char *text,bool error)
{
  cJSON *args=cJSON_Parse(text);char *out=NULL;assert(args);assert(glass_agent_call(agent,name,args,&out)==1);cJSON_Delete(args);
  assert(out);cJSON *result=cJSON_Parse(out);assert(result);
  if((cJSON_GetObjectItem(result,"error")!=NULL)!=error)fprintf(stderr,"tool=%s args=%s expected_error=%u actual=%s\n",name,text,error,out);
  assert((cJSON_GetObjectItem(result,"error")!=NULL)==error);free(out);return result;
}
static cJSON *board(struct glass_agent *agent,const char *tool,const char *arguments,bool error)
{
  char text[256];
  snprintf(text,sizeof(text),"{\"server\":\"board\",\"tool\":\"%s\",\"arguments\":%s}",tool,arguments);
  return call(agent,"mcp_call_tool",text,error);
}
int main(void)
{
  glass_board_home_register();
  atomic_bool cancel=false;
  struct glass_agent *agent=glass_agent_create(&cancel,AGENT_MODE_ASSISTANT);assert(agent);
  char *advertised=glass_agent_tools(agent,"[]");
  assert(advertised&&strstr(advertised,"home_control")&&strstr(advertised,"home_list"));free(advertised);
  cJSON_Delete(call(agent,"home_status","{}",false)); /* No discovery round. */
  cJSON *result=call(agent,"mcp_list_tools","{\"server\":\"board\"}",false);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(result,"tools"))>=18);cJSON_Delete(result);
  result=board(agent,"home_status","{}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok"))&&!cJSON_IsTrue(cJSON_GetObjectItem(result,"configured")));
  cJSON_Delete(result);
  result=board(agent,"home_control","{\"entity\":\"light.desk\",\"on\":true}",true);
  assert(!strcmp(cJSON_GetObjectItem(result,"error")->valuestring,"home_control_disabled"));cJSON_Delete(result);
  cJSON *permission=cJSON_Parse("{\"home_control_enabled\":1}");
  assert(!portal_config_save("agent",permission));cJSON_Delete(permission);
  result=board(agent,"home_state","{\"entity\":\"light.desk\"}",true);
  assert(!strcmp(cJSON_GetObjectItem(result,"error")->valuestring,"not_configured"));cJSON_Delete(result);
  assert(!fake_depth()&&!get_calls&&!state_reads&&!service_calls);
  unsigned admin=(unsigned)hass_open(HASS_READ|HASS_CONFIGURE);assert(admin);
  assert(!hass_configure(admin,"http://home.local:8123","board-token",true));hass_close(admin);
  result=board(agent,"home_status","{}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"configured")));cJSON_Delete(result);
  /* home_list paginates from the shared states cache without a second fetch. */
  char states[4096];size_t used=0;
  used+=snprintf(states+used,sizeof(states)-used,"[");
  for(unsigned i=0;i<20;i++)
    used+=snprintf(states+used,sizeof(states)-used,"%s{\"entity_id\":\"%s\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"客厅主灯%u\"}}",
      i?",":"",i?"switch.socket":"light.desk",i);
  snprintf(states+used,sizeof(states)-used,"]");
  fake_queue(200,0,states);
  result=board(agent,"home_list","{}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok"))&&cJSON_GetObjectItem(result,"total")->valueint==20);
  cJSON *items=cJSON_GetObjectItem(result,"items");assert(cJSON_GetArraySize(items)==16);
  assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"entity")->valuestring,"light.desk"));
  assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(items,0),"name")->valuestring,"客厅主灯0"));
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"more")));cJSON_Delete(result);
  assert(get_calls==1);
  result=board(agent,"home_list","{\"offset\":16}",false);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(result,"items"))==4&&!cJSON_GetObjectItem(result,"more"));
  cJSON_Delete(result);
  assert(get_calls==1&&!fake_depth());
  result=board(agent,"home_list","{\"query\":\"主灯19\"}",false);
  assert(cJSON_GetObjectItem(result,"total")->valueint==1&&cJSON_GetArraySize(cJSON_GetObjectItem(result,"items"))==1);
  assert(!strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(result,"items"),0),"name")->valuestring,"客厅主灯19"));cJSON_Delete(result);
  result=board(agent,"home_list","{\"query\":\"没有这个设备\"}",false);
  assert(cJSON_GetObjectItem(result,"total")->valueint==0);cJSON_Delete(result);
  assert(get_calls==1&&!fake_depth());
  /* home_state returns the validated card and its level. */
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"客厅主灯\",\"brightness\":204}}");
  result=board(agent,"home_state","{\"entity\":\"light.desk\"}",false);
  assert(!strcmp(cJSON_GetObjectItem(result,"name")->valuestring,"客厅主灯"));
  cJSON *card=cJSON_GetObjectItem(result,"card");
  assert(cJSON_IsObject(card)&&!strcmp(cJSON_GetObjectItem(card,"kind")->valuestring,"light")&&
    cJSON_GetObjectItem(card,"level")->valueint==80&&cJSON_IsTrue(cJSON_GetObjectItem(card,"on")));
  cJSON_Delete(result);
  /* control: 2xx plus a matching read is the only success evidence */
  fake_queue(200,0,"[]");
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"客厅主灯\",\"brightness\":204}}");
  result=board(agent,"home_control","{\"entity\":\"light.desk\",\"on\":true,\"brightness\":80}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"verified")));
  card=cJSON_GetObjectItem(result,"card");
  assert(cJSON_IsObject(card)&&cJSON_GetObjectItem(card,"level")->valueint==80);
  cJSON_Delete(result);
  assert(service_calls==1&&!strcmp(last_domain,"light")&&!strcmp(last_action,"turn_on")&&strstr(last_data,"\"brightness_pct\":80"));
  /* a mismatching read is retried inside the native 8 s / 4-read window */
  fake_queue(200,0,"[]");
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"brightness\":51}}");
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"客厅主灯\",\"brightness\":204}}");
  unsigned reads=state_reads;
  result=board(agent,"home_control","{\"entity\":\"light.desk\",\"on\":true,\"brightness\":80}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"verified"))&&state_reads==reads+2);cJSON_Delete(result);
  /* unreadable reads leave the result explicitly unconfirmed, with no card */
  fake_queue(200,0,"[]");
  for(unsigned i=0;i<4;i++)fake_queue(500,0,NULL);
  result=board(agent,"home_control","{\"entity\":\"switch.socket\",\"on\":true}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok"))&&!cJSON_IsTrue(cJSON_GetObjectItem(result,"verified"))&&
    cJSON_IsObject(cJSON_GetObjectItem(result,"card"))&&strstr(cJSON_GetObjectItem(result,"note")->valuestring,"未确认"));
  cJSON_Delete(result);
  /* the shared whitelist and argument validation hold on the AI path */
  result=board(agent,"home_control","{\"entity\":\"sensor.temp\",\"on\":true}",true);
  assert(!strcmp(cJSON_GetObjectItem(result,"error")->valuestring,"entity_not_allowed"));cJSON_Delete(result);
  assert(service_calls==3);
  cJSON_Delete(board(agent,"home_control","{\"entity\":\"light.desk\"}",true));
  cJSON_Delete(board(agent,"home_list","{\"page\":1}",true));
  cJSON_Delete(board(agent,"home_status","{\"x\":1}",true));
  cJSON_Delete(board(agent,"home_bogus","{}",true));
  assert(!fake_depth());
  /* home_service: scenes report execution, stateful targets read back once */
  fake_queue(200,0,"[]");
  fake_queue(200,0,"{\"entity_id\":\"scene.movie\",\"state\":\"2026-10-17T12:00:00+00:00\",\"attributes\":{\"friendly_name\":\"观影模式\"}}");
  result=board(agent,"home_service","{\"entity\":\"scene.movie\",\"service\":\"turn_on\"}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"accepted"))&&!cJSON_IsTrue(cJSON_GetObjectItem(result,"verified"))&&!strcmp(last_action,"turn_on")&&strstr(last_data,"scene.movie"));
  card=cJSON_GetObjectItem(result,"card");
  assert(cJSON_IsObject(card)&&!strcmp(cJSON_GetObjectItem(card,"name")->valuestring,"观影模式")&&
    !strcmp(cJSON_GetObjectItem(card,"state")->valuestring,"已受理"));
  cJSON_Delete(result);
  fake_queue(200,0,"{\"entity_id\":\"cover.blind\",\"state\":\"open\",\"attributes\":{\"current_position\":20}}");
  fake_queue(200,0,"[]");
  fake_queue(200,0,"{\"entity_id\":\"cover.blind\",\"state\":\"open\",\"attributes\":{\"friendly_name\":\"阳台窗帘\",\"current_position\":60}}");
  result=board(agent,"home_service","{\"entity\":\"cover.blind\",\"service\":\"set_cover_position\",\"arguments\":{\"position\":60}}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"verified"))&&strstr(last_data,"\"position\":60"));
  card=cJSON_GetObjectItem(result,"card");
  assert(cJSON_IsObject(card)&&cJSON_GetObjectItem(card,"level")->valueint==60);
  cJSON_Delete(result);
  /* a smuggled second target or unknown service is refused before any transport */
  cJSON_Delete(board(agent,"home_service","{\"entity\":\"cover.blind\",\"service\":\"set_cover_position\",\"arguments\":{\"entity_id\":\"light.desk\"}}",true));
  cJSON_Delete(board(agent,"home_service","{\"entity\":\"light.desk\",\"service\":\"shell_exec\"}",true));
  assert(!fake_depth());
  unsigned writes=service_calls;
  cJSON_Delete(board(agent,"home_service","{\"entity\":\"lock.front\",\"service\":\"unlock\"}",true));
  cJSON_Delete(board(agent,"home_service","{\"entity\":\"cover.blind\\\",\\\"x\\\":1\",\"service\":\"open_cover\"}",true));
  assert(service_calls==writes);
  fake_queue(200,0,"{\"entity_id\":\"cover.blind\",\"state\":\"open\",\"attributes\":{\"current_position\":20}}");
  fake_queue(200,0,"[]");
  for(unsigned i=0;i<4;i++)fake_queue(200,0,"{\"entity_id\":\"cover.blind\",\"state\":\"open\",\"attributes\":{\"current_position\":20}}");
  result=board(agent,"home_service","{\"entity\":\"cover.blind\",\"service\":\"set_cover_position\",\"arguments\":{\"position\":60}}",false);
  assert(!cJSON_IsTrue(cJSON_GetObjectItem(result,"verified")));cJSON_Delete(result);
  permission=cJSON_Parse("{\"home_control_enabled\":0}");
  assert(!portal_config_save("agent",permission));cJSON_Delete(permission);
  cJSON_Delete(board(agent,"home_control","{\"entity\":\"light.desk\",\"on\":true}",true));
  assert(service_calls==writes+1);
  permission=cJSON_Parse("{\"home_control_enabled\":1}");
  assert(!portal_config_save("agent",permission));cJSON_Delete(permission);
  /* The exact screenshot case: RGB readback can confirm pink; an old
   * color is retried without repeating the side-effecting POST. */
  fake_queue(200,0,"[]");
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"brightness\":77,\"rgb_color\":[255,0,0]}}");
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"brightness\":77,\"rgb_color\":[255,191,204],\"color_mode\":\"hs\"}}");
  writes=service_calls;reads=state_reads;
  result=call(agent,"home_service","{\"entity\":\"light.desk\",\"service\":\"turn_on\",\"arguments\":{\"brightness_pct\":30,\"rgb_color\":[255,192,203]}}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"verified"))&&service_calls==writes+1&&state_reads==reads+2);
  assert(cJSON_IsArray(cJSON_GetObjectItem(cJSON_GetObjectItem(result,"attributes"),"rgb_color")));cJSON_Delete(result);
  fake_queue(200,0,"[]");
  for(unsigned i=0;i<4;i++)fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"rgb_color\":[0,0,255]}}");
  result=call(agent,"home_service","{\"entity\":\"light.desk\",\"service\":\"turn_on\",\"arguments\":{\"rgb_color\":[255,192,203]}}",false);
  assert(!cJSON_IsTrue(cJSON_GetObjectItem(result,"verified"))&&service_calls==writes+2);cJSON_Delete(result);
  assert(!fake_depth());
  /* Fused named-light operation: resolve only lights, reject ambiguity,
   * preserve fresh reads, single POST and verified color/brightness. */
  admin=(unsigned)hass_open(HASS_READ|HASS_CONFIGURE);assert(admin);
  assert(!hass_configure(admin,"http://home.local:8123","board-token",true));hass_close(admin);
  const char *named="[{\"entity_id\":\"button.toggle\",\"state\":\"unknown\",\"attributes\":{\"friendly_name\":\"床头灯按钮\"}},"
    "{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"床头灯\"}}]";
  fake_queue(200,0,named);
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"friendly_name\":\"床头灯\",\"brightness\":77,\"rgb_color\":[255,192,203]}}");
  writes=service_calls;reads=state_reads;
  result=call(agent,"home_light","{\"query\":\"床头灯\"}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok"))&&state_reads==reads+1&&service_calls==writes);
  assert(cJSON_IsArray(cJSON_GetObjectItem(cJSON_GetObjectItem(result,"attributes"),"rgb_color")));cJSON_Delete(result);
  fake_queue(200,0,"[]");
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"on\",\"attributes\":{\"brightness\":77,\"rgb_color\":[255,192,203]}}");
  result=call(agent,"home_light","{\"query\":\"床头灯\",\"brightness\":30,\"rgb_color\":[255,192,203]}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"verified"))&&service_calls==writes+1);
  assert(strstr(last_data,"brightness_pct")&&strstr(last_data,"rgb_color"));cJSON_Delete(result);
  /* Duplicate exact names must cause no state read or write. */
  fake_queue(200,0,"[{\"entity_id\":\"light.desk\",\"attributes\":{\"friendly_name\":\"床头灯\"}},"
    "{\"entity_id\":\"light.other\",\"attributes\":{\"friendly_name\":\"床头灯\"}}]");
  reads=state_reads;writes=service_calls;
  result=call(agent,"home_light","{\"query\":\"床头灯\",\"on\":true}",true);
  assert(!strcmp(cJSON_GetObjectItem(result,"error")->valuestring,"ambiguous_light"));cJSON_Delete(result);
  assert(state_reads==reads&&service_calls==writes);
  result=call(agent,"home_light","{\"query\":\"床头\",\"on\":true}",true);
  assert(!strcmp(cJSON_GetObjectItem(result,"error")->valuestring,"light_not_found"));cJSON_Delete(result);
  assert(state_reads==reads&&service_calls==writes);
  cJSON_Delete(call(agent,"home_light","{\"query\":\"light.desk\",\"on\":false,\"brightness\":30}",true));
  cJSON_Delete(call(agent,"home_light","{\"query\":\"switch.socket\",\"on\":true}",true));
  cJSON_Delete(call(agent,"home_light","{\"query\":\"light.desk\",\"rgb_color\":[255,0,0.5]}",true));
  cJSON_Delete(call(agent,"home_light","{\"query\":\"light.desk\",\"rgb_color\":[255,0,256]}",true));
  cJSON_Delete(call(agent,"home_light","{\"query\":\"\",\"on\":true}",true));
  permission=cJSON_Parse("{\"home_control_enabled\":0}");
  assert(!portal_config_save("agent",permission));cJSON_Delete(permission);
  cJSON_Delete(call(agent,"home_light","{\"query\":\"light.desk\",\"on\":true}",true));
  fake_queue(200,0,"{\"entity_id\":\"light.desk\",\"state\":\"off\",\"attributes\":{}}");
  result=call(agent,"home_light","{\"query\":\"light.desk\"}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok")));cJSON_Delete(result);
  assert(service_calls==writes&&!fake_depth());
  #include "board_home_functions.inc"
  assert(glass_board_ui_calls==0);
  glass_agent_destroy(agent);
  glass_agent_set_board_home(NULL);
  free(agent_config);
  puts("BOARD_HOME_OK: board home tools; fused unique-light read/control; no ambiguous writes; permissions; verified color; states cache; whitelist and injection rejection");
  return 0;
}
