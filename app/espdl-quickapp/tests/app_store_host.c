/* SPDX-License-Identifier: Apache-2.0 */
/* Real filesystem, SHA, QuickJS compiler and asynchronous service. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/statfs.h>
#include <stdbool.h>
#include <sys/stat.h>
static bool fail_write,full_disk,fail_rename;
static ssize_t fault_write(int fd,const void *p,size_t n)
{if(fail_write){errno=EIO;return -1;}return write(fd,p,n);}
static int fault_statfs(const char *p,struct statfs *fs)
{int ret=statfs(p,fs);if(!ret&&full_disk)fs->f_bavail=0;return ret;}
static int fault_rename(const char *a,const char *b)
{if(fail_rename){errno=EIO;return -1;}return rename(a,b);}
#define write fault_write
#define statfs(...) fault_statfs(__VA_ARGS__)
#define rename fault_rename
#include "glass_app_store.c"
#undef write
#undef statfs
#undef rename
static char *remote_catalog,*remote_source;
static int remote_error;
int glass_store_http_fetch(const char *base,const char *path,size_t maximum,atomic_bool *cancel,char **out)
{
  *out=NULL;if(atomic_load(cancel))return -ECANCELED;
  assert(glass_store_http_base_valid(base));if(remote_error)return remote_error;
  const char *text=!strcmp(path,"/v1/catalog")?remote_catalog:remote_source;
  if(!text)return -ENOENT;
  if(strcmp(path,"/v1/catalog")){char digest[65],expected[100];hash_text(remote_source,digest);snprintf(expected,sizeof(expected),"/v1/blobs/%s/app.js",digest);if(strcmp(path,expected))return -EBADMSG;}
  if(strlen(text)>maximum)return -EFBIG;
  *out=strdup(text);return *out?0:-ENOMEM;
}
static void put(const char *path,const char *text)
{FILE *f=fopen(path,"wb");assert(f);assert(fwrite(text,1,strlen(text),f)==strlen(text));assert(!fclose(f));}
static bool exists(const char *p){struct stat st;return !lstat(p,&st);}
static void wait_done(struct glass_store_snapshot *s)
{for(unsigned n=0;n<10000;n++){glass_store_get(s);if(!s->busy)return;usleep(1000);}assert(!"worker timeout");}
static void refresh(struct glass_store_snapshot *s)
{assert(!glass_store_submit(GLASS_STORE_REFRESH,0,QPK_VOLUME_INTERNAL));wait_done(s);assert(!s->result);}
static int job(enum glass_store_action a,unsigned i,enum qpk_volume v,struct glass_store_snapshot *s)
{assert(!glass_store_submit(a,i,v));wait_done(s);return s->result;}
static void catalog(const char *package,const char *source,bool bad_hash)
{
  char dir[256],path[280];snprintf(dir,sizeof(dir),"%s/%s",GLASS_STORE_REPOSITORY,package);
  if(!exists(dir))assert(!mkdir(dir,0700));
  snprintf(path,sizeof(path),"%s/app.js",dir);put(path,source);
  char hex[65];hash_text(source,hex);if(bad_hash)hex[0]=hex[0]=='a'?'b':'a';
  cJSON *root=cJSON_CreateObject(),*apps=cJSON_AddArrayToObject(root,"apps"),*app=cJSON_CreateObject();
  cJSON_AddNumberToObject(root,"schema",1);cJSON_AddItemToArray(apps,app);
  cJSON_AddStringToObject(app,"package",package);cJSON_AddStringToObject(app,"name","SD 应用");
  cJSON_AddStringToObject(app,"version","1.2.0");cJSON_AddStringToObject(app,"summary","Local fixture");
  cJSON_AddStringToObject(app,"sha256",hex);cJSON_AddNumberToObject(app,"bytes",strlen(source));cJSON_AddNumberToObject(app,"category",2);
  char *text=cJSON_PrintUnformatted(root);put(GLASS_STORE_REPOSITORY "/index.json",text);free(text);cJSON_Delete(root);
}
int main(void)
{
  assert(!mkdir(TEST_ROOT "/data",0700));assert(!mkdir(TEST_ROOT "/sd",0700));
  assert(!mkdir(GLASS_STORE_REPOSITORY,0700));assert(!glass_store_open());
  struct glass_store_snapshot *s=calloc(1,sizeof(*s));assert(s);refresh(s);
  assert(s->count==3&&s->free_bytes[0]&&s->free_bytes[1]); /* roots not yet created */
  assert(s->featured_count==2&&!strcmp(s->featured[0].package,"store.focus"));
  assert(s->entries[0].icon==0&&s->entries[1].icon==1);
  for(unsigned i=0;i<s->count;i++){char *source=NULL;assert(!source_load(&s->entries[i],&source));assert(!compile_source(source));free(source);}
  assert(!package_valid("store../x")&&!package_valid("ai.mine")&&!package_valid("com.openvela.homeassistant"));
  assert(!package_valid("store.a.b")&&!package_valid("store.")&&package_valid("store.sd_test"));
  full_disk=true;assert(job(GLASS_STORE_INSTALL,0,QPK_VOLUME_SD,s)==-ENOSPC);full_disk=false;
  assert(!exists(QPK_VOLUME_SD_ROOT "/store.calculator"));
  fail_write=true;assert(job(GLASS_STORE_INSTALL,0,QPK_VOLUME_SD,s)==-EIO);fail_write=false;
  fail_rename=true;assert(job(GLASS_STORE_INSTALL,0,QPK_VOLUME_SD,s)==-EIO);fail_rename=false;
  assert(!exists(QPK_VOLUME_SD_ROOT "/store.calculator"));
  assert(!job(GLASS_STORE_INSTALL,0,QPK_VOLUME_SD,s));assert(s->entries[0].installed==2&&s->entries[0].managed==2);
  assert(job(GLASS_STORE_INSTALL,0,QPK_VOLUME_INTERNAL,s)==-EEXIST);
  assert(!exists(QPK_VOLUME_INTERNAL_ROOT "/store.calculator"));
  /* Namespace data survives uninstall and reinstall. */
  assert(!mkdir(QPK_VOLUME_SD_ROOT "/.data",0700));assert(!mkdir(QPK_VOLUME_SD_ROOT "/.data/store.calculator",0700));
  put(QPK_VOLUME_SD_ROOT "/.data/store.calculator/state","keep");
  put(QPK_VOLUME_SD_ROOT "/store.calculator/extra","user");
  assert(job(GLASS_STORE_REMOVE,0,QPK_VOLUME_SD,s)==-ENOTEMPTY);
  assert(!unlink(QPK_VOLUME_SD_ROOT "/store.calculator/extra"));
  put(QPK_VOLUME_SD_ROOT "/store.calculator/app.js","user changes");assert(job(GLASS_STORE_REMOVE,0,QPK_VOLUME_SD,s)==-EBADMSG);
  put(QPK_VOLUME_SD_ROOT "/store.calculator/app.js",store_seeds[0].source);
  assert(!job(GLASS_STORE_REMOVE,0,QPK_VOLUME_SD,s));assert(!s->entries[0].installed);
  assert(exists(QPK_VOLUME_SD_ROOT "/.data/store.calculator/state"));
  assert(!job(GLASS_STORE_INSTALL,0,QPK_VOLUME_SD,s));
  assert(!job(GLASS_STORE_INSTALL,1,QPK_VOLUME_INTERNAL,s));
  assert(s->entries[1].installed==1);
  /* Catalog hash mismatch, invalid JS and source symlinks never publish. */
  catalog("store.sd_test","globalThis.mustNotExecute = true;",true);refresh(s);assert(s->count==4&&s->sd_repository);
  assert(job(GLASS_STORE_INSTALL,3,QPK_VOLUME_SD,s)==-EBADMSG);
  catalog("store.sd_test","function {",false);refresh(s);assert(job(GLASS_STORE_INSTALL,3,QPK_VOLUME_SD,s)==-ENOEXEC);
  catalog("store.sd_test","throw Error('compile only');",false);refresh(s);
  assert(!job(GLASS_STORE_INSTALL,3,QPK_VOLUME_SD,s));assert(!job(GLASS_STORE_REMOVE,3,QPK_VOLUME_SD,s));
  assert(!unlink(GLASS_STORE_REPOSITORY "/store.sd_test/app.js"));
  assert(!symlink(QPK_VOLUME_INTERNAL_ROOT "/store.focus/app.js",GLASS_STORE_REPOSITORY "/store.sd_test/app.js"));
  assert(job(GLASS_STORE_INSTALL,3,QPK_VOLUME_SD,s)==-EPERM);
  assert(!exists(QPK_VOLUME_SD_ROOT "/store.sd_test"));
  /* Match the launcher's 48-byte name buffer, not just the catalog snapshot. */
  catalog("store.name_test","1;",false);
  char *index_text=NULL;assert(!read_text(GLASS_STORE_REPOSITORY "/index.json",24576,&index_text));
  cJSON *index=parse(index_text);free(index_text);cJSON *named=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(index,"apps"),0);
  char long_name[49];memset(long_name,'n',48);long_name[48]=0;
  cJSON_ReplaceItemInObjectCaseSensitive(named,"name",cJSON_CreateString(long_name));
  assert(!strcmp(string(named,"name"),long_name));
  index_text=cJSON_PrintUnformatted(index);put(GLASS_STORE_REPOSITORY "/index.json",index_text);free(index_text);cJSON_Delete(index);
  refresh(s);assert(s->count==3&&s->catalog_error==-EBADMSG);
  /* Homepage-only SD configuration can feature a builtin without duplicating
   * its package/source, and zero recommendations explicitly disables posters. */
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[],\"headline\":\"My store\",\"featured\":[{\"package\":\"store.counter\",\"title\":\"Config driven\",\"art\":\"timer\",\"tone\":\"lavender\",\"value\":\"7\",\"unit\":\"DAYS\"}]}");
  refresh(s);assert(!s->catalog_error&&s->count==3&&s->featured_count==1);
  assert(!strcmp(s->headline,"My store")&&!strcmp(s->featured[0].package,"store.counter")&&s->featured[0].tone==2&&s->featured[0].art==0);
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[],\"featured\":[]}");refresh(s);assert(!s->featured_count);
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[],\"featured\":[{\"package\":\"store.missing\",\"title\":\"Bad\"}]}");refresh(s);assert(s->catalog_error==-EBADMSG&&s->featured_count==2);
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[],\"featured\":[{\"package\":\"store.focus\",\"title\":\"Bad\",\"art\":\"script\"}]}");refresh(s);assert(s->catalog_error==-EBADMSG&&s->featured_count==2);
  /* Whole invalid catalogs are rejected; no partial/replacement seed entry. */
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[{\"package\":\"../evil\"}]}");refresh(s);assert(s->count==3&&s->catalog_error==-EBADMSG);
  catalog("store.calculator","1;",false);refresh(s);assert(s->count==3&&s->catalog_error==-EEXIST);
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[]}garbage");refresh(s);assert(s->catalog_error==-EBADMSG);
  /* Public cloud API data uses the same parser, hash and installer. */
  assert(glass_store_http_base_valid("https://store.example.com"));
  assert(glass_store_http_base_valid("http://192.168.1.2:8080/store"));
  assert(!glass_store_http_base_valid("http://public.example.com"));
  assert(!glass_store_http_base_valid("https://user:token@example.com"));
  assert(!glass_store_http_base_valid("https://example.com/../x"));
  assert(glass_store_configure("http://public.example.com")==-EINVAL);
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[]}");
  const char *fixture=getenv("STORE_CLOUD_FIXTURE");
  if(fixture){
    char path[512];snprintf(path,sizeof(path),"%s/catalog.json",fixture);assert(!read_text(path,24576,&remote_catalog));
    snprintf(path,sizeof(path),"%s/app.js",fixture);assert(!read_text(path,65536,&remote_source));
  }else{
    remote_source=strdup(store_seeds[0].source);char digest[65];hash_text(remote_source,digest);
    cJSON *root=cJSON_CreateObject(),*apps=cJSON_AddArrayToObject(root,"apps"),*app=cJSON_CreateObject();cJSON_AddNumberToObject(root,"schema",1);cJSON_AddItemToArray(apps,app);
    cJSON_AddStringToObject(app,"package","store.cloud_demo");cJSON_AddStringToObject(app,"name","Cloud demo");cJSON_AddStringToObject(app,"version","1.0.0");cJSON_AddStringToObject(app,"summary","Cloud script");cJSON_AddStringToObject(app,"sha256",digest);cJSON_AddNumberToObject(app,"bytes",strlen(remote_source));cJSON_AddNumberToObject(app,"category",0);
    remote_catalog=cJSON_PrintUnformatted(root);cJSON_Delete(root);
  }
  fail_write=true;assert(!glass_store_configure("http://127.0.0.1:8080"));wait_done(s);assert(s->result==-EIO&&!s->cloud_url[0]);fail_write=false;
  assert(!glass_store_configure("http://127.0.0.1:8080"));wait_done(s);assert(!s->result&&!s->cloud_error&&s->cloud_repository&&s->count==4&&s->entries[3].cloud);
  assert(!strcmp(s->entries[3].package,"store.cloud_demo"));
  if(fixture)assert(!strcmp(s->headline,"Cloud contract")&&s->featured_count==1&&!strcmp(s->featured[0].package,"store.cloud_demo"));
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[],\"headline\":\"Local edition\",\"featured\":[{\"package\":\"store.counter\",\"title\":\"Local wins\"}]}");
  refresh(s);assert(s->local_home&&s->cloud_repository&&!strcmp(s->headline,"Local edition")&&!strcmp(s->featured[0].package,"store.counter"));
  put(GLASS_STORE_REPOSITORY "/index.json","{\"schema\":1,\"apps\":[]}");refresh(s);
  assert(!job(GLASS_STORE_INSTALL,3,QPK_VOLUME_SD,s));assert(s->entries[3].installed==2);
  assert(!job(GLASS_STORE_REMOVE,3,QPK_VOLUME_SD,s));
  remote_error=-ENETDOWN;refresh(s);assert(s->cloud_error==-ENETDOWN&&s->count==3&&s->cloud_url[0]);remote_error=0;
  char *original_remote=remote_source;remote_source=strdup("bad source;");refresh(s);assert(job(GLASS_STORE_INSTALL,3,QPK_VOLUME_SD,s)==-EBADMSG);free(remote_source);remote_source=original_remote;
  assert(!glass_store_configure(""));wait_done(s);assert(!s->cloud_url[0]&&s->count==3);
  free(remote_catalog);free(remote_source);remote_catalog=remote_source=NULL;
  /* A selected missing SD mount never creates an internal application. */
  assert(!rename(TEST_ROOT "/sd",TEST_ROOT "/sd-away"));
  refresh(s);assert(!s->free_bytes[1]);assert(job(GLASS_STORE_INSTALL,2,QPK_VOLUME_SD,s)<0);
  assert(!exists(QPK_VOLUME_INTERNAL_ROOT "/store.counter"));
  assert(!rename(TEST_ROOT "/sd-away",TEST_ROOT "/sd"));
  cancelled=true;struct store_job j={.action=GLASS_STORE_INSTALL,.volume=QPK_VOLUME_INTERNAL,.entry=store_seeds[2].entry};
  assert(install(&j)==-ECANCELED);cancelled=false;assert(!exists(QPK_VOLUME_INTERNAL_ROOT "/store.counter"));
  /* Closing while a worker is pending cannot retain a page or its catalog. */
  assert(!glass_store_submit(GLASS_STORE_REFRESH,0,QPK_VOLUME_INTERNAL));glass_store_close();
  for(unsigned n=0;n<10000;n++){pthread_mutex_lock(&store_lock);bool pending=store!=NULL;pthread_mutex_unlock(&store_lock);if(!pending)break;usleep(1000);}
  pthread_mutex_lock(&store_lock);assert(!store);pthread_mutex_unlock(&store_lock);
  for(unsigned n=0;n<30;n++){assert(!glass_store_open());refresh(s);glass_store_close();}
  free(s);puts("APP_STORE_SERVICE_OK: real QuickJS/SHA; builtin/SD/cloud; directory-driven posters; both volumes; integrity/syntax/symlink; malformed/duplicate catalog; ENOSPC/write/rename rollback; no fallback; cloud address persistence/offline/corrupt download/clear; edited-file protection; retained data; cancel/close; 30 reopen cycles");
}
