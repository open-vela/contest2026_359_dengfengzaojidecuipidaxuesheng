/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_app_store.h"
#include "qpk_espdl.h"
#include "qpk_limits.h"
#include <cJSON.h>
#include <quickjs.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <time.h>
#include <unistd.h>
#ifndef GLASS_STORE_REPOSITORY
#define GLASS_STORE_REPOSITORY "/sdcard/app-store"
#endif

struct store_seed {struct glass_store_entry entry;const char *source;};
#include "glass_app_store_assets.inc"
static pthread_mutex_t store_lock=PTHREAD_MUTEX_INITIALIZER;
static struct glass_store_snapshot *store;
static bool opened,cancelled;
static atomic_bool store_abort;
static unsigned serial;
struct store_job {enum glass_store_action action;enum qpk_volume volume;struct glass_store_entry entry;char url[GLASS_STORE_URL_CAP];};

static bool stopped(void)
{pthread_mutex_lock(&store_lock);bool b=cancelled;pthread_mutex_unlock(&store_lock);return b;}
static int safe_dir(const char *path)
{struct stat st;if(lstat(path,&st))return -errno;return S_ISDIR(st.st_mode)&&!S_ISLNK(st.st_mode)?0:-EPERM;}
static bool package_valid(const char *s)
{
  /* A separate namespace prevents catalog entries from shadowing firmware,
   * drafts, portal uploads or applications authored by the user. */
  if(!s||strncmp(s,"store.",6)||strlen(s)>=48||!s[6])return false;
  for(const char *p=s+6;*p;p++)if(!(*p>='a'&&*p<='z')&&!isdigit((unsigned char)*p)&&*p!='_')return false;
  return true;
}
static int read_text(const char *path,size_t maximum,char **out)
{
  *out=NULL;struct stat st;
  if(lstat(path,&st))return -errno;
  if(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode))return -EPERM;
  if(st.st_size<=0||(uint64_t)st.st_size>maximum)return -EFBIG;
  int fd=open(path,O_RDONLY|O_NOFOLLOW);if(fd<0)return -errno;
  struct stat actual;int ret=0;
  if(fstat(fd,&actual)||!S_ISREG(actual.st_mode)||actual.st_size!=st.st_size)ret=-EIO;
  char *text=ret?NULL:malloc((size_t)st.st_size+1);
  if(!ret&&!text)ret=-ENOMEM;
  size_t pos=0;
  while(!ret&&pos<(size_t)st.st_size){ssize_t n=read(fd,text+pos,(size_t)st.st_size-pos);if(n<=0)ret=n<0?-errno:-EIO;else pos+=(size_t)n;}
  if(close(fd)&&!ret)ret=-errno;
  if(!ret){text[pos]=0;if(memchr(text,0,pos))ret=-EBADMSG;}
  if(ret)free(text);else *out=text;
  return ret;
}
static void hash_text(const char *text,char out[65])
{uint8_t sum[32];qpk_dl_sha256(text,strlen(text),sum);for(unsigned i=0;i<32;i++)snprintf(out+2*i,3,"%02x",sum[i]);}
static cJSON *parse(const char *text)
{const char *end=NULL;return cJSON_ParseWithOpts(text,&end,true);}
static const char *string(cJSON *obj,const char *key)
{cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);return cJSON_IsString(v)?v->valuestring:NULL;}
static bool field(cJSON *obj,const char *key,char *out,size_t cap)
{
  const char *s=string(obj,key);if(!s||!*s||strlen(s)>=cap)return false;
  for(const unsigned char *p=(const unsigned char *)s;*p;p++)if(*p<32||*p==127)return false;
  strcpy(out,s);return true;
}
static bool optional_text(cJSON *obj,const char *key,char *out,size_t cap,bool multiline)
{
  cJSON *value=cJSON_GetObjectItemCaseSensitive(obj,key);if(!value)return true;
  if(!cJSON_IsString(value)||strlen(value->valuestring)>=cap)return false;
  unsigned lines=0;
  for(const unsigned char *p=(const unsigned char *)value->valuestring;*p;p++){
    if(*p=='\n'&&multiline){if(++lines>1)return false;}
    else if(*p<32||*p==127)return false;
  }
  strcpy(out,value->valuestring);return true;
}
static bool preset(cJSON *obj,const char *key,const char *const *names,unsigned count,unsigned fallback,unsigned *out)
{
  cJSON *v=cJSON_GetObjectItemCaseSensitive(obj,key);*out=fallback;if(!v)return true;
  if(!cJSON_IsString(v))return false;
  for(unsigned i=0;i<count;i++)if(!strcmp(v->valuestring,names[i])){*out=i;return true;}
  return false;
}
static bool feature_parse(cJSON *obj,const struct glass_store_snapshot *s,struct glass_store_feature *f)
{
  memset(f,0,sizeof(*f));
  if(!cJSON_IsObject(obj)||!field(obj,"package",f->package,sizeof(f->package)))return false;
  bool found=false;for(unsigned i=0;i<s->count;i++)if(!strcmp(s->entries[i].package,f->package))found=true;
  if(!found||!optional_text(obj,"title",f->title,sizeof(f->title),true)||!f->title[0]||
     !optional_text(obj,"subtitle",f->subtitle,sizeof(f->subtitle),false)||
     !optional_text(obj,"value",f->value,sizeof(f->value),false)||!optional_text(obj,"unit",f->unit,sizeof(f->unit),false))return false;
  static const char *const arts[]={"timer","bars","none"},*const tones[]={"forest","sand","lavender"};
  return preset(obj,"art",arts,3,2,&f->art)&&preset(obj,"tone",tones,3,0,&f->tone);
}
static bool entry_parse(cJSON *obj,struct glass_store_entry *e)
{
  memset(e,0,sizeof(*e));
  if(!cJSON_IsObject(obj)||!field(obj,"package",e->package,sizeof(e->package))||!package_valid(e->package)||
     !field(obj,"name",e->name,48)||!field(obj,"version",e->version,sizeof(e->version))||
     !field(obj,"summary",e->summary,sizeof(e->summary))||!field(obj,"sha256",e->sha256,sizeof(e->sha256)))return false;
  if(strlen(e->sha256)!=64)return false;
  for(unsigned i=0;i<64;i++)if(!isdigit((unsigned char)e->sha256[i])&&(e->sha256[i]<'a'||e->sha256[i]>'f'))return false;
  cJSON *bytes=cJSON_GetObjectItemCaseSensitive(obj,"bytes"),*cat=cJSON_GetObjectItemCaseSensitive(obj,"category");
  if(!cJSON_IsNumber(bytes)||bytes->valuedouble<1||bytes->valuedouble>GLASS_STORE_SOURCE_MAX||bytes->valuedouble!=(unsigned)bytes->valuedouble||
     !cJSON_IsNumber(cat)||cat->valuedouble<0||cat->valuedouble>2||cat->valuedouble!=(unsigned)cat->valuedouble)return false;
  e->bytes=(unsigned)bytes->valuedouble;e->category=(unsigned)cat->valuedouble;e->local=true;
  static const char *const icons[]={"calculator","timer","counter","grid"};
  return preset(obj,"icon",icons,4,e->category==1?1:3,&e->icon);
}
static int merge_catalog(struct glass_store_snapshot *s,const char *text,bool cloud)
{
  int ret=0;cJSON *root=parse(text),*apps=cJSON_GetObjectItemCaseSensitive(root,"apps");
  cJSON *schema=cJSON_GetObjectItemCaseSensitive(root,"schema");
  if(!cJSON_IsNumber(schema)||schema->valuedouble!=1||!cJSON_IsArray(apps)){ret=-EBADMSG;goto end;}
  if(cJSON_GetArraySize(apps)>GLASS_STORE_MAX-(int)s->count){ret=-E2BIG;goto end;}
  unsigned original=s->count;cJSON *app;
  cJSON_ArrayForEach(app,apps){
    struct glass_store_entry e;
    if(!entry_parse(app,&e)){ret=-EBADMSG;break;}
    for(unsigned i=0;i<s->count;i++)if(!strcmp(s->entries[i].package,e.package)){ret=-EEXIST;break;}
    if(ret)break;
    e.local=!cloud;e.cloud=cloud;
    s->entries[s->count++]=e;
  }
  struct glass_store_feature features[2];unsigned count=s->featured_count;
  memcpy(features,s->featured,sizeof(features));
  char headline[96],tagline[128];strcpy(headline,s->headline);strcpy(tagline,s->tagline);
  if(!ret&&(!optional_text(root,"headline",headline,sizeof(headline),false)||!headline[0]||
            !optional_text(root,"tagline",tagline,sizeof(tagline),false)||!tagline[0]))ret=-EBADMSG;
  cJSON *featured=cJSON_GetObjectItemCaseSensitive(root,"featured");
  if(!ret&&featured){
    if(!cJSON_IsArray(featured)||cJSON_GetArraySize(featured)>2)ret=-EBADMSG;
    else {count=0;cJSON *item;cJSON_ArrayForEach(item,featured){
      if(!feature_parse(item,s,&features[count])||(count&&!strcmp(features[0].package,features[count].package))){ret=-EBADMSG;break;}
      count++;
    }}
  }
  if(ret)s->count=original;
  else {
    if(cloud)s->cloud_repository=true;else s->sd_repository=true;
    if(!cloud||!s->local_home){s->featured_count=count;memcpy(s->featured,features,sizeof(features));strcpy(s->headline,headline);strcpy(s->tagline,tagline);}
    if(!cloud&&(featured||cJSON_GetObjectItemCaseSensitive(root,"headline")||cJSON_GetObjectItemCaseSensitive(root,"tagline")))s->local_home=true;
  }
end:cJSON_Delete(root);return ret;
}
static int local_catalog(struct glass_store_snapshot *s)
{
  int ret=qpk_volume_mounted(QPK_VOLUME_SD);if(ret)return ret;
  ret=safe_dir(GLASS_STORE_REPOSITORY);if(ret)return ret;
  char *text=NULL;ret=read_text(GLASS_STORE_REPOSITORY "/index.json",24*1024,&text);
  if(!ret)ret=merge_catalog(s,text,false);
  free(text);return ret;
}
static int cloud_catalog(struct glass_store_snapshot *s)
{
  int ret=qpk_volume_ready(QPK_VOLUME_INTERNAL,false);if(ret)return ret==-ENOENT?0:ret;
  char *text=NULL;ret=read_text(QPK_VOLUME_INTERNAL_ROOT "/.store-cloud.json",512,&text);
  if(ret)return ret==-ENOENT?0:ret;
  cJSON *config=parse(text);free(text);const char *url=string(config,"base_url");
  if(!url||strlen(url)>=sizeof(s->cloud_url)||(*url&&!glass_store_http_base_valid(url)))ret=-EBADMSG;
  else strcpy(s->cloud_url,url);
  cJSON_Delete(config);if(ret||!s->cloud_url[0])return ret;
  ret=glass_store_http_fetch(s->cloud_url,"/v1/catalog",24*1024,&store_abort,&text);
  if(!ret)ret=merge_catalog(s,text,true);
  free(text);return ret;
}
static void installed_scan(struct glass_store_snapshot *s)
{
  for(unsigned v=0;v<2;v++){
    if(qpk_volume_mounted((enum qpk_volume)v))continue;
    char mount[256];strcpy(mount,qpk_volume_root(v));*strrchr(mount,'/')=0;
    struct statfs fs;if(!statfs(mount,&fs))s->free_bytes[v]=(uint64_t)fs.f_bavail*fs.f_bsize;
    if(qpk_volume_ready((enum qpk_volume)v,false))continue;
    for(unsigned i=0;i<s->count;i++){
      struct glass_store_entry *e=&s->entries[i];char dir[256],path[280];
      snprintf(dir,sizeof(dir),"%s/%s",qpk_volume_root(v),e->package);
      struct stat st;if(lstat(dir,&st))continue;
      e->installed|=1u<<v;if(safe_dir(dir))continue;
      snprintf(path,sizeof(path),"%s/manifest.json",dir);char *text=NULL;
      if(read_text(path,2048,&text))continue;
      cJSON *m=parse(text);free(text);
      const char *id=string(m,"package"),*owner=string(m,"storeOwner"),*entry=string(m,"entry");
      if(id&&owner&&entry&&!strcmp(id,e->package)&&!strcmp(owner,"glass-store-v1")&&!strcmp(entry,"app.js"))e->managed|=1u<<v;
      cJSON_Delete(m);
    }
  }
}
static int source_load(const struct glass_store_entry *e,char **out)
{
  *out=NULL;int ret=0;
  if(e->cloud){
    char base[GLASS_STORE_URL_CAP]={0},path[100];
    pthread_mutex_lock(&store_lock);if(store)strcpy(base,store->cloud_url);pthread_mutex_unlock(&store_lock);
    snprintf(path,sizeof(path),"/v1/blobs/%s/app.js",e->sha256);
    ret=glass_store_http_fetch(base,path,GLASS_STORE_SOURCE_MAX,&store_abort,out);
  }else if(e->local){
    ret=qpk_volume_mounted(QPK_VOLUME_SD);if(ret)return ret;
    ret=safe_dir(GLASS_STORE_REPOSITORY);if(ret)return ret;
    char dir[256],path[280];snprintf(dir,sizeof(dir),"%s/%s",GLASS_STORE_REPOSITORY,e->package);
    ret=safe_dir(dir);if(ret)return ret;
    snprintf(path,sizeof(path),"%s/app.js",dir);ret=read_text(path,GLASS_STORE_SOURCE_MAX,out);
  }else{
    for(unsigned i=0;i<sizeof(store_seeds)/sizeof(store_seeds[0]);i++)
      if(!strcmp(store_seeds[i].entry.package,e->package))*out=strdup(store_seeds[i].source);
    if(!*out)ret=-ENOMEM;
  }
  if(!ret){char hex[65];hash_text(*out,hex);if(strlen(*out)!=e->bytes||strcmp(hex,e->sha256))ret=-EBADMSG;}
  if(ret){free(*out);*out=NULL;}return ret;
}
struct compile_watch {uint64_t deadline;};
static uint64_t clock_ms(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static int compile_interrupt(JSRuntime *rt,void *data)
{(void)rt;return stopped()||clock_ms()>=((struct compile_watch *)data)->deadline;}
static int compile_source(const char *source)
{
  JSRuntime *rt=JS_NewRuntime();if(!rt)return -ENOMEM;
  JS_SetMemoryLimit(rt,qpk_js_memory_budget());
#ifdef __NuttX__
  JS_SetMaxStackSize(rt,16*1024);
#else
  JS_SetMaxStackSize(rt,256*1024);
#endif
  struct compile_watch watch={clock_ms()+QPK_JS_COMPILE_TIMEOUT_MS};JS_SetInterruptHandler(rt,compile_interrupt,&watch);
  JSContext *ctx=JS_NewContext(rt);int ret=-ENOMEM;
  if(ctx){JSValue value=JS_Eval(ctx,source,strlen(source),"app.js",JS_EVAL_TYPE_GLOBAL|JS_EVAL_FLAG_COMPILE_ONLY);
    ret=JS_IsException(value)?-ENOEXEC:0;JS_FreeValue(ctx,value);JS_FreeContext(ctx);}
  JS_FreeRuntime(rt);return stopped()?-ECANCELED:ret;
}
static int write_text(const char *path,const char *text)
{
  int fd=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return -errno;
  size_t n=strlen(text),pos=0;int ret=0;
  while(pos<n&&!ret){if(stopped()){ret=-ECANCELED;break;}ssize_t w=write(fd,text+pos,n-pos);if(w<=0)ret=w<0?-errno:-EIO;else pos+=(size_t)w;}
  if(!ret&&fsync(fd))ret=-errno;
  if(close(fd)&&!ret)ret=-errno;
  return ret;
}
static void clean_stage(const char *stage)
{
  /* Only our two known files; never follow a catalog path or recurse. */
  char path[280];snprintf(path,sizeof(path),"%s/app.js",stage);unlink(path);
  snprintf(path,sizeof(path),"%s/manifest.json",stage);unlink(path);rmdir(stage);
}
static int configure_cloud(const char *url)
{
  int ret=qpk_volume_ready(QPK_VOLUME_INTERNAL,true);if(ret)return ret;
  cJSON *config=cJSON_CreateObject();if(!config)return -ENOMEM;
  if(!cJSON_AddStringToObject(config,"base_url",url)){cJSON_Delete(config);return -ENOMEM;}
  char *text=cJSON_PrintUnformatted(config);cJSON_Delete(config);if(!text)return -ENOMEM;
  char temporary[256];snprintf(temporary,sizeof(temporary),"%s/.store-cloud-%08x",QPK_VOLUME_INTERNAL_ROOT,++serial);
  ret=write_text(temporary,text);free(text);
  if(!ret&&stopped())ret=-ECANCELED;
  if(!ret&&rename(temporary,QPK_VOLUME_INTERNAL_ROOT "/.store-cloud.json"))ret=-errno;
  if(ret)unlink(temporary);
  return ret;
}
static int install(const struct store_job *job)
{
  const struct glass_store_entry *e=&job->entry;char *source=NULL;
  int ret=source_load(e,&source);if(ret)return ret;
  ret=compile_source(source);if(ret){free(source);return ret;}
  ret=qpk_volume_ready(job->volume,true);if(ret){free(source);return ret;}
  const char *root=qpk_volume_root(job->volume);char target[256],stage[256],path[280];
  /* No upgrades/overwrites in v1. A foreign or older app remains untouched. */
  for(unsigned v=0;v<2;v++)if(!qpk_volume_ready(v,false)){
    struct stat st;snprintf(target,sizeof(target),"%s/%s",qpk_volume_root(v),e->package);
    if(!lstat(target,&st)){free(source);return -EEXIST;}
    if(errno!=ENOENT){ret=-errno;free(source);return ret;}
  }
  struct statfs fs;if(statfs(root,&fs)){ret=-errno;free(source);return ret;}
  if((uint64_t)fs.f_bavail*fs.f_bsize<(uint64_t)e->bytes+16*1024){free(source);return -ENOSPC;}
  snprintf(target,sizeof(target),"%s/%s",root,e->package);
  snprintf(stage,sizeof(stage),"%s/.store-%08x",root,++serial);
  if(mkdir(stage,0700)){free(source);return -errno;} /* Never clear a previous incomplete job. */
  snprintf(path,sizeof(path),"%s/app.js",stage);ret=write_text(path,source);free(source);
  cJSON *m=cJSON_CreateObject();char *manifest=NULL;
  if(!ret){
    if(!m||!cJSON_AddStringToObject(m,"name",e->name)||!cJSON_AddStringToObject(m,"package",e->package)||
       !cJSON_AddStringToObject(m,"versionName",e->version)||!cJSON_AddStringToObject(m,"entry","app.js")||
       !cJSON_AddStringToObject(m,"storeOwner","glass-store-v1")||!cJSON_AddStringToObject(m,"storeSHA256",e->sha256))ret=-ENOMEM;
    if(!ret&&!(manifest=cJSON_PrintUnformatted(m)))ret=-ENOMEM;
    if(!ret){snprintf(path,sizeof(path),"%s/manifest.json",stage);ret=write_text(path,manifest);}
  }
  free(manifest);cJSON_Delete(m);
  if(!ret&&stopped())ret=-ECANCELED;
  if(!ret)ret=qpk_volume_ready(job->volume,false);
  /* mkdir is the exclusive destination claim: a check followed by rename alone
   * could overwrite a concurrently created empty directory. Readers ignore
   * this empty claim until the complete staged directory replaces it. */
  bool claimed=false;
  if(!ret){if(mkdir(target,0700))ret=-errno;else claimed=true;}
  if(!ret&&rename(stage,target))ret=-errno;
  if(ret&&claimed)rmdir(target);
  if(ret)clean_stage(stage);
  return ret;
}
static int remove_app(const struct store_job *job)
{
  int ret=qpk_volume_ready(job->volume,false);if(ret)return ret;
  char dir[256],path[280],stage[256],*text=NULL;
  snprintf(dir,sizeof(dir),"%s/%s",qpk_volume_root(job->volume),job->entry.package);
  ret=safe_dir(dir);if(ret)return ret;
  snprintf(path,sizeof(path),"%s/manifest.json",dir);ret=read_text(path,2048,&text);if(ret)return ret;
  cJSON *m=parse(text);free(text);const char *owner=string(m,"storeOwner"),*id=string(m,"package"),*sum=string(m,"storeSHA256"),*entry=string(m,"entry");
  char expected[65]={0};
  if(!owner||strcmp(owner,"glass-store-v1")||!id||strcmp(id,job->entry.package)||!entry||strcmp(entry,"app.js")||!sum||strlen(sum)!=64)ret=-EPERM;
  else strcpy(expected,sum);
  cJSON_Delete(m);if(ret)return ret;
  snprintf(path,sizeof(path),"%s/app.js",dir);ret=read_text(path,GLASS_STORE_SOURCE_MAX,&text);if(ret)return ret;
  char actual[65];hash_text(text,actual);free(text);if(strcmp(actual,expected))return -EBADMSG;
  /* Unknown files signal user changes; never partially remove such apps. */
  /* rmdir after known-file removal cannot safely discover extras. Check first. */
  DIR *d=opendir(dir);if(!d)return -errno;struct dirent *de;
  while((de=readdir(d)))if(strcmp(de->d_name,".")&&strcmp(de->d_name,"..")&&strcmp(de->d_name,"app.js")&&strcmp(de->d_name,"manifest.json"))ret=-ENOTEMPTY;
  closedir(d);if(ret)return ret;if(stopped())return -ECANCELED;
  snprintf(stage,sizeof(stage),"%s/.store-%08x",qpk_volume_root(job->volume),++serial);
  struct stat st;if(!lstat(stage,&st)||errno!=ENOENT)return -EEXIST;
  if(rename(dir,stage))return -errno;
  /* Commit point: cancellation after rename does not turn a completed removal
   * into a cancelled receipt. Per-package .data is deliberately retained. */
  snprintf(path,sizeof(path),"%s/app.js",stage);if(unlink(path))return -errno;
  snprintf(path,sizeof(path),"%s/manifest.json",stage);if(unlink(path))return -errno;
  return rmdir(stage)?-errno:0;
}
static void *background(void *opaque)
{
  struct store_job *job=opaque;int ret=0;enum glass_store_action action=job->action;
  /* Reserve the result before making changes: an allocation failure after
   * publication must not turn a successful install into a failure receipt. */
  struct glass_store_snapshot *next=calloc(1,sizeof(*next));
  if(!next)ret=-ENOMEM;
  else if(stopped())ret=-ECANCELED;
  else if(job->action==GLASS_STORE_INSTALL)ret=install(job);
  else if(job->action==GLASS_STORE_REMOVE)ret=remove_app(job);
  else if(job->action==GLASS_STORE_CONFIGURE)ret=configure_cloud(job->url);
  free(job);
  if(next){
    for(unsigned i=0;i<sizeof(store_seeds)/sizeof(store_seeds[0]);i++)next->entries[next->count++]=store_seeds[i].entry;
    strcpy(next->headline,store_headline);strcpy(next->tagline,store_tagline);
    next->featured_count=sizeof(store_feature_seeds)/sizeof(store_feature_seeds[0]);
    memcpy(next->featured,store_feature_seeds,sizeof(store_feature_seeds));
    next->catalog_error=local_catalog(next);next->cloud_error=cloud_catalog(next);installed_scan(next);next->result=ret;
  }else if(!ret)ret=-ENOMEM;
  pthread_mutex_lock(&store_lock);
  if(opened){unsigned revision=store->revision+1;if(next){free(store);store=next;next=NULL;}store->busy=false;store->revision=revision;store->result=ret;store->completed_action=action;}
  else {free(store);store=NULL;}
  pthread_mutex_unlock(&store_lock);free(next);return NULL;
}
int glass_store_open(void)
{
  pthread_mutex_lock(&store_lock);
  if(store&&store->busy){pthread_mutex_unlock(&store_lock);return -EBUSY;}
  if(!store)store=calloc(1,sizeof(*store));
  if(!store){pthread_mutex_unlock(&store_lock);return -ENOMEM;}
  opened=true;pthread_mutex_unlock(&store_lock);return 0;
}
void glass_store_close(void)
{pthread_mutex_lock(&store_lock);opened=false;cancelled=true;atomic_store(&store_abort,true);if(store&&!store->busy){free(store);store=NULL;}pthread_mutex_unlock(&store_lock);}
void glass_store_cancel(void)
{pthread_mutex_lock(&store_lock);cancelled=true;atomic_store(&store_abort,true);pthread_mutex_unlock(&store_lock);}
void glass_store_get(struct glass_store_snapshot *out)
{pthread_mutex_lock(&store_lock);if(store)*out=*store;else memset(out,0,sizeof(*out));pthread_mutex_unlock(&store_lock);}
static int submit_job(enum glass_store_action action,unsigned index,enum qpk_volume volume,const char *url)
{
  if(action<GLASS_STORE_REFRESH||action>GLASS_STORE_CONFIGURE||volume<QPK_VOLUME_INTERNAL||volume>QPK_VOLUME_SD)return -EINVAL;
  pthread_mutex_lock(&store_lock);int ret=0;
  if(!opened||!store)ret=-ENODEV;else if(store->busy)ret=-EBUSY;else if((action==GLASS_STORE_INSTALL||action==GLASS_STORE_REMOVE)&&index>=store->count)ret=-EINVAL;
  struct store_job *job=ret?NULL:calloc(1,sizeof(*job));if(!ret&&!job)ret=-ENOMEM;
  if(!ret){
    job->action=action;job->volume=volume;if(action==GLASS_STORE_INSTALL||action==GLASS_STORE_REMOVE)job->entry=store->entries[index];
    if(url)strcpy(job->url,url);
    cancelled=false;atomic_store(&store_abort,false);store->busy=true;store->result=0;store->revision++;
    pthread_attr_t attr;pthread_attr_init(&attr);
#ifdef __NuttX__
    pthread_attr_setstacksize(&attr,32768);
#else
    pthread_attr_setstacksize(&attr,1024*1024);
#endif
    pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);pthread_t thread;
    ret=pthread_create(&thread,&attr,background,job);pthread_attr_destroy(&attr);
    if(ret){ret=-ret;free(job);store->busy=false;store->result=ret;store->revision++;}
  }
  pthread_mutex_unlock(&store_lock);return ret;
}
int glass_store_submit(enum glass_store_action action,unsigned index,enum qpk_volume volume)
{if(action==GLASS_STORE_CONFIGURE)return -EINVAL;return submit_job(action,index,volume,NULL);}
int glass_store_configure(const char *url)
{
  if(!url||(*url&&!glass_store_http_base_valid(url)))return -EINVAL;
  return submit_job(GLASS_STORE_CONFIGURE,0,QPK_VOLUME_INTERNAL,url);
}
