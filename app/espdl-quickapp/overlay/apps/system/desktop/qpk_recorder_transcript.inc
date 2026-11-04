/* SPDX-License-Identifier: Apache-2.0 */
/* Included by qpk_hw_audio.c. Only the built-in recorder can use this bridge. */
#if defined(GLASS_LOCAL_VOICE) && defined(GLV_ASR_STREAMING) && defined(GLV_ASR_MICROPHONE)
#include "glass_local_voice.h"
#include "glass_local_asr.h"
#include <ctype.h>
#ifndef QPK_RECORDER_TEXT_ROOT
#define QPK_RECORDER_TEXT_ROOT "/sdcard/recorder-text"
#endif
static bool recorder_text_cancel(void *opaque)
{struct qpk_hw_progress *p=opaque;return atomic_load(&p->cancel)||atomic_load(&p->finish);}
static int recorder_text_hash(const char *path,char hex[65],struct qpk_hw_progress *progress)
{
 FILE *f=NULL;size_t bytes=0;int ret=glv_open_asset(path,&f,&bytes);if(ret)return ret;
 struct glv_memory memory={.limit=128u*1024u,.cancelled=recorder_text_cancel,.opaque=progress};unsigned char sha[32];
 ret=glv_sha_file(f,bytes,sha,&memory);fclose(f);
 if(!ret){for(unsigned i=0;i<32;i++)snprintf(hex+i*2,3,"%02x",sha[i]);hex[64]=0;}
 return ret;
}
static int recorder_text_paths(const cJSON *a,char source[QPK_STORAGE_PATH_MAX],char sidecar[QPK_STORAGE_PATH_MAX])
{
 const cJSON *v=cJSON_GetObjectItemCaseSensitive(a,"clip");if(!cJSON_IsString(v))return -EINVAL;
 bool legacy=!strncmp(v->valuestring,"flash:",6);const char *name=v->valuestring+(legacy?6:0);size_t n=strlen(name);
 if(!n||n>58)return -EINVAL;
 for(size_t i=0;i<n;i++)if(!isalnum((unsigned char)name[i])&&name[i]!='_'&&name[i]!='-')return -EINVAL;
 enum qpk_volume volume=legacy?QPK_VOLUME_INTERNAL:QPK_VOLUME_SD;
 int ret=qpk_volume_ready(volume,false);if(ret)return ret;
 ret=qpk_audio_path(qpk_volume_data_root(volume),"com.openvela.recorder",name,source,false);if(ret)return ret;
 ret=qpk_volume_ready(QPK_VOLUME_SD,false);if(ret)return ret;
 int count=snprintf(sidecar,QPK_STORAGE_PATH_MAX,QPK_RECORDER_TEXT_ROOT "/%s-%s.json",legacy?"flash":"sd",name);
 return count<0||count>=QPK_STORAGE_PATH_MAX?-ENAMETOOLONG:0;
}
static int recorder_text_validate(const cJSON *doc,const char *hash)
{
 const cJSON *version=cJSON_GetObjectItemCaseSensitive(doc,"version"),*sha=cJSON_GetObjectItemCaseSensitive(doc,"sha256"),
 *model=cJSON_GetObjectItemCaseSensitive(doc,"model"),*text=cJSON_GetObjectItemCaseSensitive(doc,"text"),
 *duration=cJSON_GetObjectItemCaseSensitive(doc,"durationMs"),*segments=cJSON_GetObjectItemCaseSensitive(doc,"segments");
 if(!cJSON_IsNumber(version)||version->valuedouble!=1||!cJSON_IsString(sha)||strcmp(sha->valuestring,hash)||
  !cJSON_IsString(model)||strcmp(model->valuestring,GLV_ASR_RELEASE_ROOT))return -ESTALE;
 if(!cJSON_IsString(text)||!text->valuestring[0]||strlen(text->valuestring)>=GLV_MAX_REPLY||
  !cJSON_IsNumber(duration)||duration->valuedouble<1||duration->valuedouble>600000||duration->valuedouble!=duration->valueint||
  !cJSON_IsArray(segments)||!cJSON_GetArraySize(segments)||cJSON_GetArraySize(segments)>GLV_FILE_MARKS)return -EBADMSG;
 const unsigned char *utf=(const unsigned char *)text->valuestring,*end=utf+strlen(text->valuestring);
 while(utf<end){uint32_t cp;if(glv_utf8_read(&utf,end,&cp)||!cp)return -EBADMSG;}
 size_t used=0;unsigned previous=0;const cJSON *segment;
 cJSON_ArrayForEach(segment,segments){const cJSON *at=cJSON_GetObjectItemCaseSensitive(segment,"atMs"),*part=cJSON_GetObjectItemCaseSensitive(segment,"text");
  if(!cJSON_IsNumber(at)||at->valuedouble<previous||at->valuedouble>duration->valueint||at->valuedouble!=at->valueint||
   !cJSON_IsString(part)||!part->valuestring[0])return -EBADMSG;
  size_t n=strlen(part->valuestring);if(n>strlen(text->valuestring)-used||memcmp(text->valuestring+used,part->valuestring,n))return -EBADMSG;
  const unsigned char *part_utf=(const unsigned char *)part->valuestring,*part_end=part_utf+n;
  while(part_utf<part_end){uint32_t cp;if(glv_utf8_read(&part_utf,part_end,&cp)||!cp)return -EBADMSG;}
  used+=n;previous=at->valueint;
 }
 return used==strlen(text->valuestring)?0:-EBADMSG;
}
static int recorder_text_load(const char *path,const char *hash,cJSON **doc)
{
 *doc=NULL;FILE *f=NULL;size_t bytes=0;int ret=glv_open_asset(path,&f,&bytes);if(ret)return ret;
 if(!bytes||bytes>192u*1024u){fclose(f);return -E2BIG;}
 char *data=malloc(bytes+1);if(!data){fclose(f);return -ENOMEM;}
 ret=glv_read_exact(f,data,bytes);fclose(f);data[bytes]=0;
 if(!ret){unsigned depth=0;bool quoted=false,escaped=false;
  for(size_t i=0;i<bytes;i++){char ch=data[i];if(quoted){if(escaped)escaped=false;else if(ch=='\\')escaped=true;else if(ch=='"')quoted=false;}
   else if(ch=='"')quoted=true;else if(ch=='{'||ch=='['){if(++depth>8){ret=-EBADMSG;break;}}else if(ch=='}'||ch==']'){if(!depth){ret=-EBADMSG;break;}depth--;}}
  if(!ret&&(!quoted&&!depth))*doc=cJSON_ParseWithOpts(data,NULL,true);if(!*doc)ret=-EBADMSG;else ret=recorder_text_validate(*doc,hash);}
 free(data);if(ret){cJSON_Delete(*doc);*doc=NULL;}return ret;
}
static int recorder_text_copy(cJSON *out,const cJSON *doc,bool cached)
{
 const char *keys[]={"text","segments","durationMs"};
 for(unsigned i=0;i<3;i++){cJSON *v=cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(doc,keys[i]),true);if(!v)return -ENOMEM;cJSON_AddItemToObject(out,keys[i],v);}
 if(!cJSON_AddBoolToObject(out,"available",true)||!cJSON_AddBoolToObject(out,"cached",cached))return -ENOMEM;return 0;
}
static int recorder_text_save(const char *path,const cJSON *doc,struct qpk_hw_progress *progress)
{
 struct stat st;int ret=qpk_volume_ready(QPK_VOLUME_SD,false);if(ret)return ret;
 if(mkdir(QPK_RECORDER_TEXT_ROOT,0700)&&errno!=EEXIST)return -errno;
 if(lstat(QPK_RECORDER_TEXT_ROOT,&st)||!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -EINVAL;
 char temporary[QPK_STORAGE_PATH_MAX+4];int fd=-1;ret=start_file(path,temporary,&fd);if(ret)return ret;
 char *data=cJSON_PrintUnformatted(doc);if(!data)ret=-ENOMEM;
 if(!ret&&recorder_text_cancel(progress))ret=-ECANCELED;
 if(!ret)ret=write_all(fd,data,strlen(data));free(data);
 if(!ret&&fsync(fd))ret=-errno;if(close(fd)&&!ret)ret=-errno;
 if(!ret&&recorder_text_cancel(progress))ret=-ECANCELED;
 if(!ret&&rename(temporary,path))ret=-errno;if(ret)unlink(temporary);return ret;
}
static int recorder_text_remove(const cJSON *a)
{
 char source[QPK_STORAGE_PATH_MAX],path[QPK_STORAGE_PATH_MAX];int ret=recorder_text_paths(a,source,path);if(ret)return ret;
 if(unlink(path)&&errno!=ENOENT)return -errno;return 0;
}
static int recorder_text_execute(const char *op,const cJSON *a,cJSON *out,struct qpk_hw_progress *progress)
{
 char source[QPK_STORAGE_PATH_MAX],path[QPK_STORAGE_PATH_MAX],hash[65];
 int ret=recorder_text_paths(a,source,path);if(ret)return ret;
 ret=recorder_text_hash(source,hash,progress);if(ret)return ret;
 cJSON *doc=NULL;int loaded=recorder_text_load(path,hash,&doc);
 if(!strcmp(op,"transcript")){
  if(loaded==-ENOENT||loaded==-ESTALE){cJSON_AddBoolToObject(out,"available",false);return 0;}
  if(loaded)return loaded;ret=recorder_text_copy(out,doc,true);cJSON_Delete(doc);return ret;
 }
 const cJSON *force=cJSON_GetObjectItemCaseSensitive(a,"force");
 if(!loaded&&!cJSON_IsTrue(force)){ret=recorder_text_copy(out,doc,true);cJSON_Delete(doc);return ret;}
 cJSON_Delete(doc);doc=NULL;
 uint32_t request=0;ret=glass_local_voice_transcribe_file(source,&request);if(ret)return ret;
 bool stopped=false;
 for(;;){
  if(recorder_text_cancel(progress)&&!stopped){glass_local_voice_stop_from(request,GLV_CANCEL_API);stopped=true;}
  struct glv_snapshot state;glass_local_voice_get(&state,NULL,0);
  if(state.request!=request)return -ESTALE;
  atomic_store(&progress->elapsed_ms,state.asr_decoded_ms);atomic_store(&progress->stage,state.phase);
  if(!state.busy){ret=state.error;break;}usleep(50000);
 }
 if(ret)return ret;if(recorder_text_cancel(progress))return -ECANCELED;
 struct glv_file_transcript *result=malloc(sizeof(*result));if(!result)return -ENOMEM;
 ret=glass_local_voice_file_result(request,result);
 char after[65];if(!ret)ret=recorder_text_hash(source,after,progress);if(!ret&&strcmp(after,hash))ret=-ESTALE;
 if(!ret){
  doc=cJSON_CreateObject();cJSON *segments=doc?cJSON_AddArrayToObject(doc,"segments"):NULL;
  if(!segments||!cJSON_AddNumberToObject(doc,"version",1)||!cJSON_AddStringToObject(doc,"sha256",hash)||
    !cJSON_AddStringToObject(doc,"model",GLV_ASR_RELEASE_ROOT)||!cJSON_AddNumberToObject(doc,"durationMs",result->duration_ms)||
    !cJSON_AddStringToObject(doc,"text",result->text))ret=-ENOMEM;
  unsigned previous=0;
  for(unsigned i=0;!ret&&i<result->count;i++){
   unsigned end=result->marks[i].end;if(end<=previous||end>=GLV_MAX_REPLY){ret=-EBADMSG;break;}
   char fragment[GLV_MAX_REPLY];memcpy(fragment,result->text+previous,end-previous);fragment[end-previous]=0;previous=end;
   cJSON *segment=cJSON_CreateObject();if(!segment){ret=-ENOMEM;break;}
   if(!cJSON_AddNumberToObject(segment,"atMs",result->marks[i].ms)||!cJSON_AddStringToObject(segment,"text",fragment)){cJSON_Delete(segment);ret=-ENOMEM;break;}
   cJSON_AddItemToArray(segments,segment);
  }
 }
 free(result);
 if(!ret)ret=recorder_text_validate(doc,hash);
 if(!ret)ret=recorder_text_save(path,doc,progress);
 if(!ret)ret=recorder_text_copy(out,doc,false);
 cJSON_Delete(doc);return ret;
}
#else
static int recorder_text_remove(const cJSON *a){(void)a;return 0;}
static int recorder_text_execute(const char *op,const cJSON *a,cJSON *out,struct qpk_hw_progress *progress)
{(void)op;(void)a;(void)out;(void)progress;return -ENOTSUP;}
#endif
