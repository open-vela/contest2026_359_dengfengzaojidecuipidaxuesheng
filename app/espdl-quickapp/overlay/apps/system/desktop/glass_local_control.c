/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_control.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
static const char *text(const cJSON *o,const char *key)
{const cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key);return cJSON_IsString(v)?v->valuestring:"";}
static bool keys(const cJSON *o,const char *allowed)
{
 if(!cJSON_IsObject(o))return false;
 for(const cJSON *v=o->child;v;v=v->next){
  if(!v->string||strlen(v->string)>31||cJSON_GetObjectItemCaseSensitive(o,v->string)!=v)return false;
  char key[36];snprintf(key,sizeof(key),",%s,",v->string);if(!strstr(allowed,key))return false;
 }return true;
}
static bool identifier(const char *s)
{if(!s||!s[0]||strlen(s)>31||!(isalpha((unsigned char)*s)||*s=='_'))return false;for(;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='A'&&*s<='Z')||(*s>='0'&&*s<='9')||*s=='_'))return false;return true;}
static bool label(const char *s)
{if(!s||!s[0]||strlen(s)>64||strpbrk(s,"|@()\r\n\t")||strstr(s,"｜"))return false;for(const unsigned char *p=(const unsigned char *)s;*p;p++)if(*p<32||*p==127)return false;return true;}
static bool entity(const char *s)
{if(!s||strncmp(s,"light.",6)||!s[6]||strlen(s)>95)return false;for(s+=6;*s;s++)if(!((*s>='a'&&*s<='z')||(*s>='0'&&*s<='9')||*s=='_'))return false;return true;}
static const cJSON *find(const struct glc_catalog *c,const char *id)
{const cJSON *r;cJSON_ArrayForEach(r,c->rows)if(!strcmp(text(r,"id"),id))return r;return NULL;}
void glc_close(struct glc_catalog *c){if(c){cJSON_Delete(c->root);memset(c,0,sizeof(*c));}}
int glc_open(struct glc_catalog *c,const char *json)
{
 if(!c)return -EINVAL;memset(c,0,sizeof(*c));if(!json)return -EINVAL;if(!*json)return -ENOENT;if(strlen(json)>4096)return -E2BIG;
 c->root=cJSON_ParseWithOpts(json,NULL,true);if(!keys(c->root,",resources,"))goto invalid;
 c->rows=cJSON_GetObjectItemCaseSensitive(c->root,"resources");int count=cJSON_GetArraySize(c->rows);
 if(!cJSON_IsArray(c->rows)||count<1||count>GLC_MAX_RESOURCES)goto invalid;
 const cJSON *r;cJSON_ArrayForEach(r,c->rows){
  if(!keys(r,",id,,name,,aliases,,entity_id,,ha_device_id,,mijia_did,,members,")||!identifier(text(r,"id"))||!label(text(r,"name")))goto invalid;
  for(const cJSON *prior=c->rows->child;prior!=r;prior=prior->next)if(!strcmp(text(prior,"id"),text(r,"id")))goto invalid;
  const cJSON *aliases=cJSON_GetObjectItemCaseSensitive(r,"aliases"),*a;
  if(aliases){if(!cJSON_IsArray(aliases)||cJSON_GetArraySize(aliases)>3)goto invalid;cJSON_ArrayForEach(a,aliases)if(!cJSON_IsString(a)||!label(a->valuestring))goto invalid;}
  const cJSON *members=cJSON_GetObjectItemCaseSensitive(r,"members");
  if(members){
   if(!cJSON_IsArray(members)||cJSON_GetArraySize(members)<1||cJSON_GetArraySize(members)>GLC_MAX_RESOURCES||cJSON_GetObjectItemCaseSensitive(r,"entity_id")||cJSON_GetObjectItemCaseSensitive(r,"ha_device_id")||cJSON_GetObjectItemCaseSensitive(r,"mijia_did"))goto invalid;
   cJSON_ArrayForEach(a,members){if(!cJSON_IsString(a)||!identifier(a->valuestring))goto invalid;for(const cJSON *p=members->child;p!=a;p=p->next)if(!strcmp(p->valuestring,a->valuestring))goto invalid;}
  }else{
   if(!entity(text(r,"entity_id")))goto invalid;
   const cJSON *device=cJSON_GetObjectItemCaseSensitive(r,"ha_device_id"),*did=cJSON_GetObjectItemCaseSensitive(r,"mijia_did");
   if(device&&(!cJSON_IsString(device)||strlen(device->valuestring)!=32||strspn(device->valuestring,"0123456789abcdef")!=32))goto invalid;
   if(did&&!cJSON_IsNull(did)&&(!cJSON_IsString(did)||!did->valuestring[0]||strlen(did->valuestring)>64||strspn(did->valuestring,"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_.:-")!=strlen(did->valuestring)))goto invalid;
   for(const cJSON *prior=c->rows->child;prior!=r;prior=prior->next)if(!strcmp(text(prior,"entity_id"),text(r,"entity_id")))goto invalid;
  }
 }
 cJSON_ArrayForEach(r,c->rows){const cJSON *members=cJSON_GetObjectItemCaseSensitive(r,"members"),*a;cJSON_ArrayForEach(a,members){const cJSON *member=find(c,a->valuestring);if(!member||cJSON_GetObjectItemCaseSensitive(member,"members"))goto invalid;}}
 return 0;
invalid:glc_close(c);return -EINVAL;
}
int glc_catalog_valid(const char *json){struct glc_catalog c={0};int ret=glc_open(&c,json);glc_close(&c);return ret;}
static int append(char *out,size_t cap,size_t *used,const char *format,...)
{if(*used>=cap)return -E2BIG;va_list ap;va_start(ap,format);int n=vsnprintf(out+*used,cap-*used,format,ap);va_end(ap);if(n<0||(size_t)n>=cap-*used)return -E2BIG;*used+=(size_t)n;return 0;}
int glc_prompt(const struct glc_catalog *c,const char *query,char *out,size_t cap)
{
 if(!c||!c->root||!query||!*query||!out||!cap)return -EINVAL;
 char requested[32];bool write,on;int match=glc_request(c,query,requested,sizeof(requested),&write,&on);if(match)return match;
 const cJSON *target=find(c,requested),*needed=cJSON_GetObjectItemCaseSensitive(target,"members");
 size_t used=0;int ret=append(out,cap,&used,"按清单输出Tool: ID.函数(参数)，on=true/false，level=0/1。条件先read再compare，只按实际回执操作。只有@组是分组；目标不明先问。\n清单:\n");if(ret)return ret;
 const cJSON *r;cJSON_ArrayForEach(r,c->rows){
  bool include=r==target;const cJSON *m;cJSON_ArrayForEach(m,needed)if(!strcmp(text(r,"id"),m->valuestring))include=true;
  if(!include)continue;
  if((ret=append(out,cap,&used,"%s|%s",text(r,"id"),text(r,"name"))))return ret;
  const cJSON *alias;cJSON_ArrayForEach(alias,cJSON_GetObjectItemCaseSensitive(r,"aliases"))if((ret=append(out,cap,&used,"/%s",alias->valuestring)))return ret;
  const cJSON *members=cJSON_GetObjectItemCaseSensitive(r,"members");
  if((ret=append(out,cap,&used,"|set_power(on:bool)%s",members?" @组(":",read()->on:bool")))return ret;
  if(members){const cJSON *m;cJSON_ArrayForEach(m,members)if((ret=append(out,cap,&used,"%s%s",m==members->child?"":"/",m->valuestring)))return ret;if((ret=append(out,cap,&used,")")))return ret;}
  if((ret=append(out,cap,&used,"\n")))return ret;
 }
 /* Resolve ONLY the already uniquely registered name/aliases to its short
  * ID. Keep the original instruction visible and preserve every other byte;
  * do not supply a function, direction or argument on the model's behalf. */
 char normalized[1025];size_t bytes=0;
 for(const char *q=query;*q;){
  size_t longest=0;const char *name=text(target,"name");if(!strncmp(q,name,strlen(name)))longest=strlen(name);
  const cJSON *a;cJSON_ArrayForEach(a,cJSON_GetObjectItemCaseSensitive(target,"aliases"))if(strlen(a->valuestring)>longest&&!strncmp(q,a->valuestring,strlen(a->valuestring)))longest=strlen(a->valuestring);
  if(longest){int ret=append(normalized,sizeof(normalized),&bytes,"%s",requested);if(ret)return ret;q+=longest;}
  else {if(bytes+1>=sizeof(normalized))return -E2BIG;normalized[bytes++]=*q++;normalized[bytes]=0;}
 }
 return append(out,cap,&used,"原始指令:%s\n用户:%s\n调用记录:\n无",query,normalized);
}
static int stopped(const struct glc_transport *t)
{if(t->cancel&&atomic_load(t->cancel))return -ECANCELED;return t->current_revision(t->opaque)!=t->revision?-ESTALE:0;}
static cJSON *call(const struct glc_transport *t,const char *method,const char *id,bool on)
{char *raw=t->call(t->opaque,method,id,on,t->revision,t->cancel);if(!raw)return NULL;cJSON *o=cJSON_ParseWithOpts(raw,NULL,true);free(raw);return o;}
int glc_request(const struct glc_catalog *c,const char *query,char *id,size_t capacity,bool *write,bool *on)
{
 if(!c||!c->root||!query||!id||!capacity||!write||!on)return -EINVAL;
 /* This first live-device release accepts explicit, single-target requests.
  * Conditions, negations and unspecified targets are never approximated by
  * a model guess. The model still proposes the function; this contract
  * checks its target/direction against the user's actual request. */
 const char *blocked[]={"不要","别","不许","禁止","不用","保持","如果","假如","若","超过","高于","低于","大于","小于","然后","再","先","关心","如何","怎么","介绍","解释","告诉"};
 for(unsigned i=0;i<sizeof(blocked)/sizeof(blocked[0]);i++)if(strstr(query,blocked[i]))return -ENOTSUP;
 const cJSON *chosen=NULL,*r;unsigned matches=0;size_t longest=0;const char *term=NULL;
 cJSON_ArrayForEach(r,c->rows){
  size_t length=0;const char *selected=NULL,*s=text(r,"name");if(strstr(query,s)){length=strlen(s);selected=s;}s=text(r,"id");if(strstr(query,s)&&strlen(s)>length){length=strlen(s);selected=s;}
  const cJSON *a;cJSON_ArrayForEach(a,cJSON_GetObjectItemCaseSensitive(r,"aliases"))if(strstr(query,a->valuestring)&&strlen(a->valuestring)>length){length=strlen(a->valuestring);selected=a->valuestring;}
  if(!length)continue;
  if(length>longest){longest=length;chosen=r;term=selected;matches=1;}else if(length==longest)matches++;
 }
 if(!chosen||matches!=1)return -ENOENT;
 /* Other named targets are ambiguous unless their matching name is a
  * substring of the chosen full name (e.g. 客厅所有灯 versus 所有灯). */
 cJSON_ArrayForEach(r,c->rows)if(r!=chosen){
  const char *terms[5]={text(r,"name"),text(r,"id")};unsigned total=2;const cJSON *a;cJSON_ArrayForEach(a,cJSON_GetObjectItemCaseSensitive(r,"aliases"))terms[total++]=a->valuestring;
  for(unsigned k=0;k<total;k++)for(const char *p=strstr(query,terms[k]);p;p=strstr(p+1,terms[k])){
   bool covered=false;for(const char *q=strstr(query,term);q;q=strstr(q+1,term))if(p>=q&&p+strlen(terms[k])<=q+strlen(term)){covered=true;break;}
   if(!covered)return -EINVAL;
  }
 }
 bool open=false,close=false,read=false;
 const char *opens[]={"打开","开启","开一下","开下","点亮","启动"};
 const char *closes[]={"关闭","关掉","关一下","关下","熄灭"};
 const char *reads[]={"状态","查询","看看","读取","开着吗","亮着吗","是否","有没有","吗","？","?"};
 for(unsigned i=0;i<sizeof(opens)/sizeof(opens[0]);i++)open|=strstr(query,opens[i])!=NULL;
 for(unsigned i=0;i<sizeof(closes)/sizeof(closes[0]);i++)close|=strstr(query,closes[i])!=NULL;
 for(unsigned i=0;i<sizeof(reads)/sizeof(reads[0]);i++)read|=strstr(query,reads[i])!=NULL;
 for(const char *p=strstr(query,term);p;p=strstr(p+1,term)){const char *q=p;while(q>query&&q[-1]==' ')q--;if(q-query>=3){open|=!memcmp(q-3,"开",3);close|=!memcmp(q-3,"关",3);}}
 if((open&&close)||((open||close)&&read)||(!open&&!close&&!read))return -EINVAL;
 if(strlen(text(chosen,"id"))>=capacity)return -E2BIG;strcpy(id,text(chosen,"id"));*write=open||close;*on=open;return 0;
}
int glc_execute(const struct glc_catalog *c,const char *query,const char *reply,const struct glc_transport *t,char *out,size_t cap)
{
 if(!c||!reply||!t||!t->call||!t->current_revision||!out||cap<1)return -EINVAL;out[0]=0;
 if(strncmp(reply,"Tool:",5))return -ENOMSG;
 const char *p=reply+5;while(*p==' ')p++;const char *dot=strchr(p,'.');if(!dot||dot-p<1||dot-p>31)return -EINVAL;
 char id[32];memcpy(id,p,(size_t)(dot-p));id[dot-p]=0;if(!identifier(id))return -EINVAL;
 p=dot+1;bool write=false,on=false;
 if(!strcmp(p,"read()"))write=false;
 else if(!strcmp(p,"set_power(on=true)")){write=true;on=true;}
 else if(!strcmp(p,"set_power(on=false)")){write=true;on=false;}
 else return -EINVAL;
 char requested[32];bool requested_write=false,requested_on=false;
 int authorization=glc_request(c,query,requested,sizeof(requested),&requested_write,&requested_on);
 if(authorization)return authorization;
 if(strcmp(requested,id)||requested_write!=write||(write&&requested_on!=on))return -EACCES;
 const cJSON *row=find(c,id);if(!row)return -ENOENT;
 const cJSON *members=cJSON_GetObjectItemCaseSensitive(row,"members");if(members&&!write)return -ENOTSUP;
 const cJSON *targets[GLC_MAX_RESOURCES];unsigned count=0;
 if(members){const cJSON *m;cJSON_ArrayForEach(m,members)targets[count++]=find(c,m->valuestring);}else targets[count++]=row;
 /* Preflight the entire explicit group before the first side effect. Never
  * derive membership from names/rooms; only the registered member list. */
 bool already[GLC_MAX_RESOURCES]={0};
 for(unsigned i=0;i<count;i++){
  int ret=stopped(t);if(ret)return ret;
  cJSON *state=call(t,"home_state",text(targets[i],"entity_id"),false);
  ret=stopped(t);if(ret){cJSON_Delete(state);return ret;}
  bool ok=state&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state,"ok"))&&!strcmp(text(state,"entity"),text(targets[i],"entity_id"));
  const char *value=text(state,"state");bool binary=!strcmp(value,"on")||!strcmp(value,"off");already[i]=binary&&(!strcmp(value,"on")==on);
  if(!ok||!binary){cJSON_Delete(state);snprintf(out,cap,"%s状态不可确认，未发送控制。",text(targets[i],"name"));return -EREMOTEIO;}
  if(!write)snprintf(out,cap,"%s：%s（来自当前 HA 回读）",text(row,"name"),!strcmp(value,"on")?"已打开":"已关闭");
  cJSON_Delete(state);
 }
 if(!write)return stopped(t);
 size_t used=0;
 for(unsigned i=0;i<count;i++){
  int ret=stopped(t);if(ret)return ret;
  if(already[i]){if((ret=append(out,cap,&used,"%s%s已是%s状态（当前回读确认，未重复写入）。",i?"\n":"",text(targets[i],"name"),on?"打开":"关闭")))return ret;continue;}
  cJSON *result=call(t,"home_power",text(targets[i],"entity_id"),on);
  ret=stopped(t);if(ret){cJSON_Delete(result);snprintf(out,cap,"请求已停止，已发送操作不自动回滚或重发，请查询设备状态。");return ret;}
  bool ok=result&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result,"ok"));
  bool verified=result&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(result,"verified"));
  bool identity=result&&!strcmp(text(result,"entity"),text(targets[i],"entity_id"));
  bool state=result&&!strcmp(text(result,"state"),on?"on":"off");
  if(!ok||!verified||!identity||!state){
   cJSON_Delete(result);append(out,cap,&used,"%s%s执行结果未确认，已停止，不会自动重发。",i?"\n":"",text(targets[i],"name"));return -EREMOTEIO;
  }
  cJSON_Delete(result);if((ret=append(out,cap,&used,"%s%s已%s（状态回读已确认）。",i?"\n":"",text(targets[i],"name"),on?"打开":"关闭")))return ret;
 }
 return 0;
}
