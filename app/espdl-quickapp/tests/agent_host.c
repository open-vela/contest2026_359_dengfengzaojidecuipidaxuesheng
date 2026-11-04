/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_agent.h"
#include "glass_portal.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static char *saved;
static unsigned http_calls,tool_calls,initializes,deletes;
static bool call_timeout,expired,wrong_version,call_cancel;
static unsigned remote_cancels;
const char *glass_qpk_guide(void){return "Validated quick app skill";}
int qpk_storage_read(const char *root,const char *package,const char *key,char *out,size_t capacity,size_t *length)
{(void)root;(void)package;if(strcmp(key,"agent")||!saved)return -ENOENT;assert(strlen(saved)<capacity);strcpy(out,saved);*length=strlen(saved);return 0;}
int qpk_storage_write(const char *root,const char *package,const char *key,const char *data,size_t size)
{(void)root;(void)package;assert(!strcmp(key,"agent"));free(saved);saved=strndup(data,size);return saved?0:-ENOMEM;}
int glass_mcp_http_perform(struct glass_mcp_http *http)
{
  http_calls++;assert(!strcmp(http->url,"https://mcp.example/mcp"));assert(!strcmp(http->token,"private-token"));
  assert(http->timeout_ms<=15000);http->status=200;http->result=NULL;http->response_session[0]=0;
  if(!strcmp(http->method,"DELETE")){deletes++;assert(!strcmp(http->session,"session-42"));return 0;}
  cJSON *req=cJSON_Parse(http->body);assert(req);const char *method=cJSON_GetObjectItem(req,"method")->valuestring;
  cJSON *id=cJSON_GetObjectItem(req,"id"),*params=cJSON_GetObjectItem(req,"params");
  cJSON *root=cJSON_CreateObject(),*result=cJSON_AddObjectToObject(root,"result");
  if(!strcmp(method,"initialize")){
    initializes++;assert(!http->session[0]&&!http->version[0]);
    assert(!strcmp(cJSON_GetObjectItem(params,"protocolVersion")->valuestring,"2025-11-25"));
    cJSON_AddStringToObject(result,"protocolVersion",wrong_version?"2099-01-01":"2025-11-25");
    cJSON_AddObjectToObject(cJSON_AddObjectToObject(result,"capabilities"),"tools");strcpy(http->response_session,"session-42");
  }else{
    assert(!strcmp(http->session,"session-42")&&!strcmp(http->version,"2025-11-25"));
    if(!strcmp(method,"notifications/initialized")){assert(!id);http->status=202;}
    else if(!strcmp(method,"notifications/cancelled")){assert(!id&&cJSON_IsNumber(cJSON_GetObjectItem(params,"requestId")));remote_cancels++;http->status=202;}
    else if(!strcmp(method,"tools/list")){
      if(expired){http->status=404;expired=false;}
      cJSON *array=cJSON_AddArrayToObject(result,"tools"),*tool=cJSON_CreateObject();
      cJSON_AddStringToObject(tool,"name","weather_get");cJSON_AddStringToObject(tool,"description","Get weather");
      cJSON_AddStringToObject(cJSON_AddObjectToObject(tool,"inputSchema"),"type","object");cJSON_AddItemToArray(array,tool);
      if(!cJSON_GetObjectItem(params,"cursor"))cJSON_AddStringToObject(result,"nextCursor","page-2");
    }else if(!strcmp(method,"tools/call")){
      tool_calls++;assert(!strcmp(cJSON_GetObjectItem(params,"name")->valuestring,"weather_get"));
      assert(!strcmp(cJSON_GetObjectItem(cJSON_GetObjectItem(params,"arguments"),"city")->valuestring,"Shanghai"));
      if(call_timeout){cJSON_Delete(root);cJSON_Delete(req);return -ETIMEDOUT;}
      if(call_cancel){atomic_store(http->cancel,true);cJSON_Delete(root);cJSON_Delete(req);return -ECANCELED;}
      cJSON_AddBoolToObject(result,"isError",false);cJSON *content=cJSON_AddArrayToObject(result,"content"),*text=cJSON_CreateObject();
      cJSON_AddStringToObject(text,"type","text");cJSON_AddStringToObject(text,"text","Sunny");cJSON_AddItemToArray(content,text);
    }else assert(!"unexpected MCP method");
  }
  if(id){cJSON_AddStringToObject(root,"jsonrpc","2.0");cJSON_AddNumberToObject(root,"id",id->valueint);}
  http->result=root;cJSON_Delete(req);return 0;
}
static void save(const char *text,int expected)
{cJSON *o=cJSON_Parse(text);assert(o);assert(portal_config_save("agent",o)==expected);cJSON_Delete(o);}
static cJSON *call(struct glass_agent *agent,const char *name,const char *text,bool error)
{
  cJSON *args=cJSON_Parse(text);char *out=NULL;assert(args);assert(glass_agent_call(agent,name,args,&out)==1);cJSON_Delete(args);
  assert(out);cJSON *result=cJSON_Parse(out);assert(result);assert((cJSON_GetObjectItem(result,"error")!=NULL)==error);free(out);return result;
}
static void mkdir_checked(const char *path){assert(!mkdir(path,0700)||errno==EEXIST);}
int main(int argc,char **argv)
{
  assert(argc==2);mkdir_checked(argv[1]);char scoped[512];snprintf(scoped,sizeof(scoped),"%s/assistant",argv[1]);mkdir_checked(scoped);char dir[512],path[600];snprintf(dir,sizeof(dir),"%s/testing",scoped);mkdir_checked(dir);
  snprintf(path,sizeof(path),"%s/SKILL.md",dir);FILE *f=fopen(path,"wb");assert(f);fputs("\xef\xbb\xbf---\r\nname: testing\r\ndescription: Use for a deterministic test.\r\n---\r\nRespond with an exact result.\r\n",f);fclose(f);
  char link[600];snprintf(link,sizeof(link),"%s/escape",scoped);unlink(link);assert(!symlink("/etc",link));
  save("{\"skills_enabled\":2}",-EINVAL);save("{\"mcp1_url\":\"http://local/mcp\"}",-EINVAL);
  save("{\"mcp1_enabled\":1}",-EINVAL);save("{\"mcp1_token\":\"x\\r\\nAuthorization: x\"}",-EINVAL);
  save("{\"mcp1_tools\":\"weather_get,,delete\"}",-EINVAL);save("{\"unknown\":0}",-EINVAL);
  save("{\"mcp1_url\":\"https://mcp.example/mcp\",\"mcp1_token\":\"private-token\",\"mcp1_enabled\":1,\"mcp1_tools\":\"weather_get\"}",0);
  save("{\"skills_enabled\":1}",0);
  cJSON *public=portal_config_get("agent");assert(public&&!cJSON_GetObjectItem(public,"mcp1_token"));assert(cJSON_IsTrue(cJSON_GetObjectItem(public,"mcp1_token_set")));cJSON_Delete(public);
  atomic_bool cancel=false;struct glass_agent *agent=glass_agent_create(&cancel,AGENT_MODE_ASSISTANT);assert(agent);assert(!http_calls);
  char *context=glass_agent_context(agent);assert(strstr(context,"testing")&&strstr(context,"mcp1")&&!strstr(context,"private-token"));free(context);
  char *tools=glass_agent_tools(agent,"[]");assert(strstr(tools,"skill_read")&&strstr(tools,"mcp_call_tool"));free(tools);
  cJSON *result=call(agent,"skill_list","{}",false);assert(cJSON_GetArraySize(result)==2);cJSON_Delete(result);
  result=call(agent,"skill_read","{\"id\":\"testing\"}",false);assert(strstr(cJSON_GetObjectItem(result,"instructions")->valuestring,"exact result"));cJSON_Delete(result);
  cJSON_Delete(call(agent,"skill_read","{\"id\":\"../etc\"}",true));cJSON_Delete(call(agent,"skill_read","{\"id\":\"escape\"}",true));
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"mcp1\",\"tool\":\"weather_get\",\"arguments\":{}}",true));assert(!http_calls);
  cJSON_Delete(call(agent,"mcp_list_tools","{\"server\":\"mcp2\"}",true));assert(!http_calls);
  result=call(agent,"mcp_list_tools","{\"server\":\"mcp1\"}",false);assert(cJSON_GetObjectItem(result,"nextCursor"));cJSON_Delete(result);
  assert(initializes==1&&http_calls==3);
  cJSON_Delete(call(agent,"mcp_list_tools","{\"server\":\"mcp1\",\"cursor\":\"page-2\"}",false));assert(initializes==1);
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"mcp1\",\"tool\":\"delete_all\",\"arguments\":{}}",true));assert(!tool_calls);
  result=call(agent,"mcp_call_tool","{\"server\":\"mcp1\",\"tool\":\"weather_get\",\"arguments\":{\"city\":\"Shanghai\"}}",false);assert(strstr(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(result,"content"),0),"text")->valuestring,"Sunny"));cJSON_Delete(result);
  call_timeout=true;cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"mcp1\",\"tool\":\"weather_get\",\"arguments\":{\"city\":\"Shanghai\"}}",true));assert(tool_calls==2);call_timeout=false;
  expired=true;cJSON_Delete(call(agent,"mcp_list_tools","{\"server\":\"mcp1\"}",true));cJSON_Delete(call(agent,"mcp_list_tools","{\"server\":\"mcp1\"}",false));assert(initializes==2);
  call_cancel=true;cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"mcp1\",\"tool\":\"weather_get\",\"arguments\":{\"city\":\"Shanghai\"}}",true));assert(remote_cancels==1);call_cancel=false;atomic_store(&cancel,false);
  unsigned before=http_calls;atomic_store(&cancel,true);cJSON_Delete(call(agent,"mcp_list_tools","{\"server\":\"mcp1\"}",true));assert(http_calls==before);
  glass_agent_destroy(agent);assert(deletes==1);atomic_store(&cancel,false);
  wrong_version=true;agent=glass_agent_create(&cancel,AGENT_MODE_ASSISTANT);cJSON_Delete(call(agent,"mcp_list_tools","{\"server\":\"mcp1\"}",true));glass_agent_destroy(agent);wrong_version=false;
  save("{\"mcp1_enabled\":0,\"skills_enabled\":0}",0);agent=glass_agent_create(&cancel,AGENT_MODE_ASSISTANT);tools=glass_agent_tools(agent,"[]");assert(strstr(tools,"mcp_call_tool"));free(tools);cJSON_Delete(call(agent,"skill_list","{}",true));
  before=http_calls;
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"device_status\",\"arguments\":{}}",true));
  result=call(agent,"mcp_list_tools","{\"server\":\"board\"}",false);assert(cJSON_GetArraySize(cJSON_GetObjectItem(result,"tools"))>=11);cJSON_Delete(result);
  /* Without the shared HA service the smart home tools do not exist. */
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"home_status\",\"arguments\":{}}",true));
  cJSON_Delete(call(agent,"mcp_list_tools","{\"server\":\"board\",\"cursor\":\"bad\"}",true));
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"device_status\",\"arguments\":{}}",false));
  result=call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_capabilities\",\"arguments\":{}}",false);
  char *capabilities=cJSON_PrintUnformatted(result);assert(capabilities&&strstr(capabilities,"jbox.status")&&!strstr(capabilities,"jbox.pair"));free(capabilities);cJSON_Delete(result);
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_reference\",\"arguments\":{\"section\":2}}",false));
  result=call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_reference\",\"arguments\":{\"section\":8}}",false);
  assert(strstr(cJSON_GetObjectItem(result,"scope")->valuestring,"persist across AI requests"));
  assert(strstr(cJSON_GetObjectItem(result,"reference")->valuestring,"system.jbox"));cJSON_Delete(result);
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_reference\",\"arguments\":{\"section\":1.5}}",true));
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_submit\",\"arguments\":{\"operation\":\"shell.exec\",\"arguments\":{}}}",true));
  result=call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_submit\",\"arguments\":{\"operation\":\"gpio.read\",\"arguments\":{\"pin\":6}}}",false);
  assert(cJSON_GetObjectItem(result,"id")->valueint==1);cJSON_Delete(result);
  result=call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_poll\",\"arguments\":{\"id\":1}}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"done")));assert(cJSON_GetObjectItem(cJSON_GetObjectItem(result,"result"),"value")->valueint==1);cJSON_Delete(result);
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_poll\",\"arguments\":{\"id\":1}}",true));
  cJSON_Delete(call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_submit\",\"arguments\":{\"operation\":\"jbox.pair\",\"arguments\":{\"host\":\"127.0.0.1\",\"deviceId\":\"aabbccddeeff\"}}}",true));
  result=call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_submit\",\"arguments\":{\"operation\":\"jbox.status\",\"arguments\":{\"generation\":1}}}",false);
  assert(cJSON_GetObjectItem(result,"id")->valueint==2);cJSON_Delete(result);
  result=call(agent,"mcp_call_tool","{\"server\":\"board\",\"tool\":\"hardware_poll\",\"arguments\":{\"id\":2}}",false);
  assert(cJSON_IsTrue(cJSON_GetObjectItem(result,"ok"))&&cJSON_GetObjectItem(cJSON_GetObjectItem(result,"result"),"heapFree")->valueint==456);cJSON_Delete(result);
  assert(http_calls==before);glass_agent_destroy(agent);usleep(200000);
  unlink(link);unlink(path);rmdir(dir);rmdir(scoped);
  char profile_dir[512],profile_path[600];snprintf(profile_dir,sizeof(profile_dir),AGENT_PROFILE_ROOT "/create");mkdir_checked(profile_dir);
  snprintf(profile_path,sizeof(profile_path),"%s/agent.md",profile_dir);
  f=fopen(profile_path,"w");assert(f);fputs("Custom creation profile\r\nFirst version",f);fclose(f);
  agent=glass_agent_create(&cancel,AGENT_MODE_CREATE);assert(agent);
  assert(!strcmp(glass_agent_profile(agent),"Custom creation profile\nFirst version"));
  f=fopen(profile_path,"w");assert(f);fputs("Second version",f);fclose(f);
  assert(strstr(glass_agent_profile(agent),"First version"));
  struct glass_agent *next=glass_agent_create(&cancel,AGENT_MODE_CREATE);assert(!strcmp(glass_agent_profile(next),"Second version"));glass_agent_destroy(next);
  glass_agent_destroy(agent);unlink(profile_path);assert(!symlink("/etc/passwd",profile_path));
  agent=glass_agent_create(&cancel,AGENT_MODE_CREATE);assert(strstr(glass_agent_profile(agent),"创作模式"));glass_agent_destroy(agent);
  unlink(profile_path);rmdir(profile_dir);free(saved);
  puts("AGENT_HOST_OK: SD skills; config validation/secret redaction; MCP initialize/notification/session/version/list/pagination/call/allowlist/cancel/timeout/no replay/cleanup");return 0;
}
