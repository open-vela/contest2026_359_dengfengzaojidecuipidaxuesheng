/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_portal.h"
#include "glass_agent.h"
#include "glass_ai_config.h"
#ifdef GLASS_LOCAL_CONTROL
#include "glass_local_control.h"
#endif
#include "qpk_storage.h"
#include <nuttx/config.h>
#include "../hass/hass_config_validate.h"
#ifdef CONFIG_SYSTEM_HASS
#include "hass_service.h"
#endif
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#ifndef PORTAL_CONFIG_ROOT
#define PORTAL_CONFIG_ROOT "/data/config"
#endif
static pthread_mutex_t config_lock=PTHREAD_MUTEX_INITIALIZER;
static uint8_t desktop_values[4],desktop_pending[4];
static bool desktop_dirty;
static bool config_section_valid(const char *section)
{
  return section&&(!strcmp(section,"ai")||!strcmp(section,"homeassistant")||!strcmp(section,"music")||
    !strcmp(section,"voice")||!strcmp(section,"agent")||!strcmp(section,"desktop")||!strcmp(section,"localcontrol")||!strcmp(section,"jbox"));
}
static cJSON *load(const char *section)
{
  if(!config_section_valid(section))return NULL;
  /* Essential configuration must survive removal or failure of the SD card. */
  char *data=malloc(8193); size_t size=0; cJSON *obj=NULL;
  if(!data) return NULL;
  int ret=qpk_storage_read(PORTAL_CONFIG_ROOT,"portal",section,data,8193,&size);
  if(!ret) {
    data[size]=0; obj=cJSON_ParseWithOpts(data,NULL,true);
  }
  free(data);
  if(ret==-ENOENT) obj=cJSON_CreateObject();
  if(!cJSON_IsObject(obj)) { cJSON_Delete(obj); return NULL; }
  return obj;
}
static const char *string(const cJSON *obj,const char *key,const char *fallback)
{ cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key); return cJSON_IsString(v)?v->valuestring:fallback; }
static unsigned number(const cJSON *obj,const char *key,unsigned fallback)
{ cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key); return cJSON_IsNumber(v)?(unsigned)v->valuedouble:fallback; }
static void default_string(cJSON *o,const char *key,const char *value)
{ if(!cJSON_GetObjectItemCaseSensitive(o,key)) cJSON_AddStringToObject(o,key,value); }
static cJSON *defaults(const char *section)
{
  cJSON *o=load(section); if(!o) return NULL;
  if(!strcmp(section,"ai")) {
    default_string(o,"base_url","https://api.openai.com/v1"); default_string(o,"model","");
    default_string(o,"backend","openai_compatible"); default_string(o,"api_key","");
    default_string(o,"system_prompt","请准确、简洁地回答用户，使用用户的语言。");
    if(!cJSON_GetObjectItemCaseSensitive(o,"max_tokens")) cJSON_AddNumberToObject(o,"max_tokens",16384);
    if(!cJSON_GetObjectItemCaseSensitive(o,"timeout_ms")) cJSON_AddNumberToObject(o,"timeout_ms",300000);
  } else if(!strcmp(section,"homeassistant")) {
    char *legacy=malloc(8193); size_t length;
    if(legacy) {
      if(!cJSON_GetObjectItemCaseSensitive(o,"url")&&!qpk_storage_read("/data/qpk","com.openvela.homeassistant","ha_url",legacy,8193,&length)) default_string(o,"url",legacy);
      if(!cJSON_GetObjectItemCaseSensitive(o,"token")&&!qpk_storage_read("/data/qpk","com.openvela.homeassistant","ha_token",legacy,8193,&length)) default_string(o,"token",legacy);
      free(legacy);
    }
    default_string(o,"url","http://homeassistant.local:8123"); default_string(o,"token","");
  } else if(!strcmp(section,"music")) default_string(o,"api_key","");
  else if(!strcmp(section,"voice")) {
    default_string(o,"app_id",""); default_string(o,"secret_id","");
    default_string(o,"secret_key","");
  }
  else if(!strcmp(section,"localcontrol"))default_string(o,"catalog","");
  else if(!strcmp(section,"jbox")){
    default_string(o,"host","");default_string(o,"device_id","");default_string(o,"key","");
  }
  else if(!strcmp(section,"agent")) {
    if(!cJSON_GetObjectItemCaseSensitive(o,"home_control_enabled"))cJSON_AddNumberToObject(o,"home_control_enabled",0);
    if(!cJSON_GetObjectItemCaseSensitive(o,"skills_enabled"))cJSON_AddNumberToObject(o,"skills_enabled",1);
    for(unsigned i=1;i<=AGENT_MCP_SERVERS;i++){
      char key[32];const char *names[]={"name","url","token","tools"};
      for(unsigned j=0;j<4;j++){snprintf(key,sizeof(key),"mcp%u_%s",i,names[j]);default_string(o,key,"");}
      snprintf(key,sizeof(key),"mcp%u_enabled",i);if(!cJSON_GetObjectItemCaseSensitive(o,key))cJSON_AddNumberToObject(o,key,0);
    }
  }
  else if(!strcmp(section,"desktop")) {
    cJSON_Delete(o); o=cJSON_CreateObject();
    const char *keys[]={"light","palette","widget","reduced_motion"};
    for(unsigned i=0;i<4;i++) cJSON_AddNumberToObject(o,keys[i],desktop_values[i]);
  } else { cJSON_Delete(o); return NULL; }
  return o;
}
static bool valid_url(const char *s,bool https)
{
  if(!*s) return true;
  const char *host=!strncmp(s,"https://",8)?s+8:!https&&!strncmp(s,"http://",7)?s+7:NULL;
  if(!host || !*host || *host=='/' || strpbrk(host,"@?#\\ \t\r\n")) return false;
  return true;
}
struct field { const char *section,*key; unsigned limit; bool secret; };
static const struct field fields[]={
 {"ai","api_key",2048,true},{"ai","base_url",1024,false},{"ai","model",128,false},
 {"ai","backend",32,false},{"ai","system_prompt",1024,false},
 {"homeassistant","url",HASS_URL_CAPACITY-1,false},{"homeassistant","token",HASS_TOKEN_CAPACITY-1,true},{"music","api_key",128,true},
 {"voice","app_id",32,false},{"voice","secret_id",128,true},{"voice","secret_key",128,true},
 {"jbox","host",15,false},{"jbox","device_id",12,false},{"jbox","key",64,true}
};
cJSON *portal_config_get(const char *section)
{
  pthread_mutex_lock(&config_lock); cJSON *o=defaults(section);
  if(o) for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
    if(fields[i].secret && !strcmp(section,fields[i].section)) {
      bool present=string(o,fields[i].key,"")[0]!=0;
      cJSON_DeleteItemFromObjectCaseSensitive(o,fields[i].key);
      char key[64]; snprintf(key,sizeof(key),"%s_set",fields[i].key); cJSON_AddBoolToObject(o,key,present);
    }
  if(o&&!strcmp(section,"agent"))for(unsigned i=1;i<=AGENT_MCP_SERVERS;i++){
    char key[32],flag[40];snprintf(key,sizeof(key),"mcp%u_token",i);snprintf(flag,sizeof(flag),"%s_set",key);
    bool present=string(o,key,"")[0]!=0;cJSON_DeleteItemFromObjectCaseSensitive(o,key);cJSON_AddBoolToObject(o,flag,present);
  }
  pthread_mutex_unlock(&config_lock); return o;
}
static int config_save(const char *section,const cJSON *patch)
{
  if(!cJSON_IsObject(patch)) return -EINVAL;
  pthread_mutex_lock(&config_lock); cJSON *o=defaults(section),*previous=NULL; int ret=-EINVAL;
#ifdef CONFIG_SYSTEM_HASS
  int ha_client=0;
#endif
  if(!o) goto done;
  previous=cJSON_Duplicate(o,true);if(!previous){ret=-ENOMEM;goto done;}
  const char *desktop_keys[]={"light","palette","widget","reduced_motion"};
  for(const cJSON *v=patch->child;v;v=v->next) {
    if(!v->string) goto done;
    for(const cJSON *next=v->next;next;next=next->next) if(!strcmp(next->string,v->string)) goto done;
    if(!strcmp(section,"localcontrol")){
#ifdef GLASS_LOCAL_CONTROL
      if(strcmp(v->string,"catalog")||!cJSON_IsString(v)||glc_catalog_valid(v->valuestring))goto done;
      cJSON *copy=cJSON_Duplicate(v,true);if(!copy){ret=-ENOMEM;goto done;}
      cJSON_DeleteItemFromObjectCaseSensitive(o,"catalog");cJSON_AddItemToObject(o,"catalog",copy);continue;
#else
      ret=-ENOTSUP;goto done;
#endif
    }
    if(!strcmp(section,"agent")){
      if(!glass_agent_value_valid(v->string,v))goto done;
      cJSON *copy=cJSON_Duplicate(v,true);if(!copy){ret=-ENOMEM;goto done;}
      cJSON_DeleteItemFromObjectCaseSensitive(o,v->string);cJSON_AddItemToObject(o,v->string,copy);continue;
    }
    unsigned limit=0; bool known=false;
    for(unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++)
      if(!strcmp(section,fields[i].section)&&!strcmp(v->string,fields[i].key)) { limit=fields[i].limit; known=true; }
    if(known) {
      if(!cJSON_IsString(v)||strlen(v->valuestring)>limit) goto done;
      for(const unsigned char *s=(unsigned char *)v->valuestring;*s;s++)
        if((*s<32 || *s==127) && (strcmp(v->string,"system_prompt") || (*s!='\n'&&*s!='\t'))) goto done;
      if((!strcmp(v->string,"base_url")||!strcmp(v->string,"url"))&&!valid_url(v->valuestring,!strcmp(section,"ai"))) goto done;
      if(!strcmp(v->string,"backend") && strcmp(v->valuestring,"openai_compatible") && strcmp(v->valuestring,"anthropic_compatible")) goto done;
      if(!strcmp(section,"voice")&&!strcmp(v->string,"app_id")&&
         v->valuestring[0]&&strspn(v->valuestring,"0123456789")!=strlen(v->valuestring)) goto done;
    } else {
      unsigned min=0,max=0;
      if(!strcmp(section,"ai")&&!strcmp(v->string,"max_tokens")) { min=1; max=384000; }
      if(!strcmp(section,"ai")&&!strcmp(v->string,"timeout_ms")) { min=5000; max=3600000; }
      if(!strcmp(section,"desktop")) for(unsigned i=0;i<4;i++) if(!strcmp(v->string,desktop_keys[i])) max=(i==1||i==2)?2:1;
      if(!max||!cJSON_IsNumber(v)||v->valuedouble<min||v->valuedouble>max||v->valuedouble!=(int)v->valuedouble) goto done;
    }
    cJSON *copy=cJSON_Duplicate(v,true); if(!copy) { ret=-ENOMEM; goto done; }
    cJSON_DeleteItemFromObjectCaseSensitive(o,v->string); cJSON_AddItemToObject(o,v->string,copy);
  }
  if(!strcmp(section,"desktop")) {
    for(unsigned i=0;i<4;i++) desktop_pending[i]=number(o,desktop_keys[i],0);
    desktop_pending[3]=1;
    desktop_dirty=true; ret=0; goto done;
  }
  if(!strcmp(section,"homeassistant")) {
    const char *url=string(o,"url",""),*token=string(o,"token","");
    if((*url&&!hass_config_url_valid(url))||!hass_config_token_valid(token)) goto done;
#ifdef CONFIG_SYSTEM_HASS
    /* Reserve the apply handle before writing; a failed save keeps the active
     * connection. Hold config_lock across save/apply so concurrent writers
     * cannot apply an older URL with a newer token. */
    ha_client=hass_open(HASS_READ|HASS_CONTROL|HASS_CONFIGURE);
    if(ha_client<0) { ret=ha_client; goto done; }
#endif
  }
  if(!strcmp(section,"jbox")){
    const char *host=string(o,"host",""),*id=string(o,"device_id",""),*key=string(o,"key","");
    struct in_addr address;
    if(inet_pton(AF_INET,host,&address)!=1||!address.s_addr||address.s_addr==INADDR_BROADCAST||
       IN_MULTICAST(ntohl(address.s_addr))||strlen(id)!=12||strspn(id,"0123456789abcdef")!=12||
       strlen(key)!=64||strspn(key,"0123456789abcdefABCDEF")!=64)goto done;
  }
  if(!strcmp(section,"agent"))for(unsigned i=1;i<=AGENT_MCP_SERVERS;i++){
    char key[32];snprintf(key,sizeof(key),"mcp%u_enabled",i);
    if(number(o,key,0)){snprintf(key,sizeof(key),"mcp%u_url",i);if(!string(o,key,"")[0])goto done;}
  }
  char *data=cJSON_PrintUnformatted(o);
  if(!data) { ret=-ENOMEM; goto done; }
  bool unchanged=cJSON_Compare(previous,o,true);
  bool write_needed=!unchanged;
  ret=!write_needed?0:strlen(data)>8192?-E2BIG:
    qpk_storage_write(PORTAL_CONFIG_ROOT,"portal",section,data,strlen(data));
  printf("[portal-config] section=%s medium=%s changed=%u written=%u result=%d\n",section,
    "flash",!unchanged,write_needed&&!ret,ret);
  memset(data,0,strlen(data));free(data);
#ifdef CONFIG_SYSTEM_HASS
  if(!ret&&ha_client>0) {
    const char *url=string(o,"url",""),*token=string(o,"token","");
    ret=*url&&*token?hass_configure(ha_client,url,token,true):hass_clear_configuration(ha_client);
  }
#endif
done:
#ifdef CONFIG_SYSTEM_HASS
  if(ha_client>0) hass_close(ha_client);
#endif
  cJSON_Delete(previous);cJSON_Delete(o); pthread_mutex_unlock(&config_lock); return ret;
}
int portal_config_save(const char *section,const cJSON *patch)
{
  if(!config_section_valid(section)||!cJSON_IsObject(patch))return -EINVAL;
  bool ai=!strcmp(section,"ai")||!strcmp(section,"voice")||!strcmp(section,"agent")||!strcmp(section,"localcontrol")||!strcmp(section,"jbox");
  if(ai){int ret=glass_ai_config_begin(10000);if(ret)return ret;}
  int ret=config_save(section,patch);
  if(ai)glass_ai_config_end();
  return ret;
}
int portal_jbox_load(struct portal_jbox_config *out)
{
  if(!out)return -EINVAL;
  memset(out,0,sizeof(*out));
  pthread_mutex_lock(&config_lock);cJSON *o=defaults("jbox");int ret=-EIO;
  if(o){
    const char *host=string(o,"host",""),*id=string(o,"device_id",""),*key=string(o,"key","");
    ret=!*key?-ENOENT:strlen(host)>=sizeof(out->host)||strlen(id)!=12||strlen(key)!=64?-EINVAL:0;
    if(!ret){strcpy(out->host,host);strcpy(out->device_id,id);strcpy(out->key,key);}
  }
  cJSON_Delete(o);pthread_mutex_unlock(&config_lock);return ret;
}
int portal_jbox_save(const struct portal_jbox_config *config)
{
  if(!config||!memchr(config->host,0,sizeof(config->host))||
     !memchr(config->device_id,0,sizeof(config->device_id))||!memchr(config->key,0,sizeof(config->key)))return -EINVAL;
  cJSON *patch=cJSON_CreateObject();if(!patch)return -ENOMEM;
  int ret=-ENOMEM;
  if(cJSON_AddStringToObject(patch,"host",config->host)&&cJSON_AddStringToObject(patch,"device_id",config->device_id)&&
     cJSON_AddStringToObject(patch,"key",config->key))ret=portal_config_save("jbox",patch);
  cJSON *secret=cJSON_GetObjectItemCaseSensitive(patch,"key");
  if(cJSON_IsString(secret))memset(secret->valuestring,0,strlen(secret->valuestring));
  cJSON_Delete(patch);return ret;
}
int portal_ai_load(struct portal_ai_config *out)
{
  if(!out) return -EINVAL;
  pthread_mutex_lock(&config_lock); cJSON *o=defaults("ai"); int ret=-ENOMEM;
  if(o) {
    snprintf(out->api_key,sizeof(out->api_key),"%s",string(o,"api_key",""));
    snprintf(out->base_url,sizeof(out->base_url),"%s",string(o,"base_url",""));
    snprintf(out->model,sizeof(out->model),"%s",string(o,"model",""));
    snprintf(out->backend,sizeof(out->backend),"%s",string(o,"backend","openai_compatible"));
    snprintf(out->system_prompt,sizeof(out->system_prompt),"%s",string(o,"system_prompt",""));
    out->timeout_ms=number(o,"timeout_ms",300000); out->max_tokens=number(o,"max_tokens",16384);
    ret=out->api_key[0]&&out->model[0]&&out->base_url[0]?0:-ENOENT;
  }
  cJSON_Delete(o); pthread_mutex_unlock(&config_lock); return ret;
}
int portal_voice_load(struct portal_voice_config *out)
{
  if(!out) return -EINVAL;
  memset(out,0,sizeof(*out));
  pthread_mutex_lock(&config_lock); cJSON *o=defaults("voice"); int ret=-ENOMEM;
  if(o) {
    snprintf(out->app_id,sizeof(out->app_id),"%s",string(o,"app_id",""));
    snprintf(out->secret_id,sizeof(out->secret_id),"%s",string(o,"secret_id",""));
    snprintf(out->secret_key,sizeof(out->secret_key),"%s",string(o,"secret_key",""));
    ret=out->app_id[0]&&out->secret_id[0]&&out->secret_key[0]?0:-ENOENT;
  }
  cJSON_Delete(o); pthread_mutex_unlock(&config_lock); return ret;
}
int portal_music_key(char *out,size_t capacity)
{
  pthread_mutex_lock(&config_lock); cJSON *o=defaults("music");
  int ret=o&&string(o,"api_key","")[0]?0:-ENOENT;
  if(!ret) snprintf(out,capacity,"%s",string(o,"api_key",""));
  cJSON_Delete(o); pthread_mutex_unlock(&config_lock); return ret;
}
/* Overrides are read on app launch, without returning credentials to HTTP. */
int portal_ha_value(const char *key,char *out,size_t capacity)
{
  if(!out||!capacity) return -EINVAL;
  out[0]='\0';
  if(strcmp(key,"ha_url")&&strcmp(key,"ha_token")) return -ENOENT;
  pthread_mutex_lock(&config_lock); cJSON *o=defaults("homeassistant");
  cJSON *v=cJSON_GetObjectItemCaseSensitive(o,!strcmp(key,"ha_url")?"url":"token");
  int ret=cJSON_IsString(v)?0:-ENOENT;
  if(!ret&&strlen(v->valuestring)>=capacity) ret=-ENOSPC;
  if(!ret) strcpy(out,v->valuestring);
  cJSON_Delete(o); pthread_mutex_unlock(&config_lock); return ret;
}
int portal_ha_set(const char *key,const char *value)
{
  if(strcmp(key,"ha_url")&&strcmp(key,"ha_token")) return -ENOENT;
  cJSON *patch=cJSON_CreateObject(); if(!patch) return -ENOMEM;
  cJSON_AddStringToObject(patch,!strcmp(key,"ha_url")?"url":"token",value);
  int ret=portal_config_save("homeassistant",patch); cJSON_Delete(patch); return ret;
}
int portal_ha_save(const char *url,const char *token)
{
  if(!url||!token||strlen(url)>1024||strlen(token)>2048) return -EINVAL;
  cJSON *patch=cJSON_CreateObject(); if(!patch) return -ENOMEM;
  if(!cJSON_AddStringToObject(patch,"url",url)||
     !cJSON_AddStringToObject(patch,"token",token))
    { cJSON_Delete(patch); return -ENOMEM; }
  int ret=portal_config_save("homeassistant",patch); cJSON_Delete(patch); return ret;
}
int portal_ha_load(char *url,size_t url_size,char *token,size_t token_size)
{
  if(!url||!url_size||!token||!token_size) return -EINVAL;
  url[0]=token[0]='\0';
  pthread_mutex_lock(&config_lock);
  cJSON *o=defaults("homeassistant"); int ret=-EIO;
  if(o) {
    const char *u=string(o,"url",""),*t=string(o,"token","");
    ret=strlen(u)>=url_size||strlen(t)>=token_size?-ENOSPC:0;
    if(!ret) { strcpy(url,u); strcpy(token,t); }
  }
  cJSON_Delete(o); pthread_mutex_unlock(&config_lock); return ret;
}
int portal_ha_apply(void)
{
#ifdef CONFIG_SYSTEM_HASS
  pthread_mutex_lock(&config_lock);
  cJSON *o=defaults("homeassistant"); int ret=-EIO;
  if(o) {
    const char *url=string(o,"url",""),*token=string(o,"token","");
    if((*url&&!hass_config_url_valid(url))||!hass_config_token_valid(token)) ret=-EINVAL;
    else {
      int client=hass_open(HASS_READ|HASS_CONTROL|HASS_CONFIGURE);
      ret=client;
      if(client>0) {
        ret=*url&&*token?hass_configure(client,url,token,true):hass_clear_configuration(client);
        hass_close(client);
      }
    }
  }
  cJSON_Delete(o); pthread_mutex_unlock(&config_lock); return ret;
#else
  return -ENOSYS;
#endif
}
void portal_desktop_publish(const uint8_t values[4])
{ pthread_mutex_lock(&config_lock); memcpy(desktop_values,values,4); desktop_values[3]=1; pthread_mutex_unlock(&config_lock); }
bool portal_desktop_take(uint8_t values[4])
{ pthread_mutex_lock(&config_lock); bool ready=desktop_dirty; if(ready) { memcpy(values,desktop_pending,4); desktop_dirty=false; } pthread_mutex_unlock(&config_lock); return ready; }

int portal_agent_load(cJSON **out)
{
  if(!out)return -EINVAL;
  pthread_mutex_lock(&config_lock);*out=defaults("agent");pthread_mutex_unlock(&config_lock);
  return *out?0:-EIO;
}
