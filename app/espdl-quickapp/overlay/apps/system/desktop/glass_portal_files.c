/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_portal.h"
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#ifndef PORTAL_DATA_ROOT
#define PORTAL_DATA_ROOT "/data"
#endif
#include "qpk_volume.h"
static enum qpk_volume install_volume;
static char install_stage[256]=QPK_VOLUME_INTERNAL_ROOT "/.install";
#define STAGE install_stage
static char install_package[32],install_entry[32];

/* One path component. SmartFS reserves 32 bytes per name including the
 * terminator. "." and ".." are the only names that can alias or escape the
 * directory being walked; a name that merely starts with a dot is a real
 * directory on the volume (every application keeps its private data under
 * /data/qpk/.data) and has to stay reachable.
 */
static bool portal_component(const char *name,size_t length)
{
  if(!length || length>31) return false;
  if((length==1&&name[0]=='.')||(length==2&&name[0]=='.'&&name[1]=='.')) return false;
  for(size_t i=0;i<length;i++) {
    unsigned char c=(unsigned char)name[i];
    if(c<32 || c==127 || strchr("\\:*?\"<>|%",c)) return false;
  }
  return true;
}
/* The deepest generated path is /data/qpk/.data plus two package and three
 * key chunks: qpk/.data/@2p..[/p..]/k../k../k../value. */
bool portal_relative_path(const char *p)
{
  if(!p || !*p || strlen(p)>160) return false;
  unsigned depth=0;
  while(*p) {
    const char *slash=strchr(p,'/');
    size_t length=slash?(size_t)(slash-p):strlen(p);
    if(!portal_component(p,length) || ++depth>10) return false;
    if(!slash) break;
    p=slash+1; if(!*p) return false;
  }
  return true;
}
/* Walk every existing component; never follow links outside the public root. */
int portal_safe_path(const char *root,const char *relative,char *out,size_t cap,bool parents)
{
  if(*relative && !portal_relative_path(relative)) return -EINVAL;
  if(snprintf(out,cap,"%s%s%s",root,*relative?"/":"",relative)>=(int)cap) return -ENAMETOOLONG;
  struct stat st;
  for(char *p=out+1;;p++) if(*p=='/' || !*p) {
    char saved=*p; *p=0;
    if(lstat(out,&st)==0) {
      if(S_ISLNK(st.st_mode) || (saved && !S_ISDIR(st.st_mode))) { *p=saved; return -EINVAL; }
    } else if(errno!=ENOENT) { *p=saved; return -errno; }
    else if(saved && parents && mkdir(out,0700)<0 && errno!=EEXIST) { *p=saved; return -errno; }
    *p=saved; if(!saved) break;
  }
  return 0;
}
int portal_public_path(const char *relative,char *out,size_t cap)
{
  if(!relative || !out || !cap) return -EINVAL;
  if(!strcmp(relative,"files") || !strncmp(relative,"files/",6))
    return portal_safe_path(PORTAL_DATA_ROOT "/files",relative+5+!!relative[5],out,cap,false);
  if(!strcmp(relative,"data") || !strncmp(relative,"data/",5))
    return portal_safe_path(PORTAL_DATA_ROOT,relative+4+!!relative[4],out,cap,false);
  if(!strcmp(relative,"sdcard") || !strncmp(relative,"sdcard/",7))
    return portal_safe_path("/sdcard",relative+6+!!relative[6],out,cap,false);
  return -EINVAL;
}
/* Resolving a GET path must not create an internal Flash directory, including
 * while browsing SD. Retain the legacy files/ root's first-write behavior only
 * for explicit mutations, after the normal path/link checks succeeded. */
int portal_public_write_path(const char *relative,char *out,size_t cap)
{
  int ret=portal_public_path(relative,out,cap);
  if(!ret && (!strcmp(relative,"files") || !strncmp(relative,"files/",6))) {
    if(mkdir(PORTAL_DATA_ROOT "/files",0700)<0 && errno!=EEXIST) return -errno;
    ret=portal_public_path(relative,out,cap);
  }
  return ret;
}
int portal_remove_tree(const char *path)
{
  struct stat st; if(lstat(path,&st)<0) return errno==ENOENT?0:-errno;
  if(!S_ISDIR(st.st_mode)) return unlink(path)<0?-errno:0;
  DIR *d=opendir(path); if(!d) return -errno;
  struct dirent *e; int ret=0; char next[256];
  while((e=readdir(d))) {
    if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
    if(snprintf(next,sizeof(next),"%s/%s",path,e->d_name)>=(int)sizeof(next)) { ret=-ENAMETOOLONG; break; }
    if((ret=portal_remove_tree(next))<0) break;
  }
  closedir(d); return ret?ret:rmdir(path)<0?-errno:0;
}
void portal_install_abort(void) { portal_remove_tree(STAGE); install_package[0]=0; }
static const char *str(const cJSON *o,const char *key)
{ cJSON *v=cJSON_GetObjectItemCaseSensitive(o,key); return cJSON_IsString(v)?v->valuestring:""; }
int portal_install_begin(const cJSON *manifest,char *error,size_t cap)
{
  const char *pkg=str(manifest,"package"),*entry=str(manifest,"entry"),*name=str(manifest,"name");
  if(!*entry) entry="app.js";
  if(!portal_relative_path(pkg)||strlen(pkg)>31||strchr(pkg,'/')||*pkg=='.'||
     !portal_relative_path(entry)||strchr(entry,'/')||*entry=='.'||strlen(entry)<4||strcmp(entry+strlen(entry)-3,".js")||
     !*name||strlen(name)>63||strpbrk(name,"\"\\\r\n")) return -EINVAL;
  for(const char *p=pkg;*p;p++) if(!((*p>='a'&&*p<='z')||(*p>='A'&&*p<='Z')||(*p>='0'&&*p<='9')||strchr("._-",*p))) return -EINVAL;
  if(!strcmp(pkg,"music")||!strcmp(pkg,"pinyin")||!strncmp(pkg,"com.openvela.",13)) return -EPERM;
  const cJSON *selection=cJSON_GetObjectItemCaseSensitive(manifest,"storage");
  if(selection&&!cJSON_IsString(selection))return -EINVAL;
  const char *storage=str(manifest,"storage");
  if(*storage&&strcmp(storage,"internal")&&strcmp(storage,"sdcard"))return -EINVAL;
  enum qpk_volume selected=!strcmp(storage,"sdcard")?QPK_VOLUME_SD:QPK_VOLUME_INTERNAL;
  int ready=qpk_volume_ready(selected,true);
  if(ready){snprintf(error,cap,selected==QPK_VOLUME_SD?"SD 卡未挂载或不可写":"内置存储不可用");return ready;}
  char target[192]; struct stat st;
  for(unsigned v=0;v<2;v++){
    snprintf(target,sizeof(target),"%s/%s",qpk_volume_root(v),pkg);
    if(lstat(target,&st)==0) { snprintf(error,cap,"同名应用已存在，请先在设备删除旧应用"); return -EEXIST; }
  }
  portal_install_abort();install_volume=selected;
  snprintf(STAGE,sizeof(install_stage),"%s/.install",qpk_volume_root(selected));
  if(mkdir(STAGE,0700)<0) return -errno;
  cJSON *clean=cJSON_CreateObject(); if(!clean) return -ENOMEM;
  cJSON_AddStringToObject(clean,"name",name); cJSON_AddStringToObject(clean,"package",pkg);
  cJSON_AddStringToObject(clean,"entry",entry); cJSON_AddStringToObject(clean,"versionName","1.0");
  char *data=cJSON_PrintUnformatted(clean); cJSON_Delete(clean); if(!data) return -ENOMEM;
  char manifest_path[288];snprintf(manifest_path,sizeof(manifest_path),"%s/manifest.json",STAGE);
  FILE *f=fopen(manifest_path,"wb"); int ret=-EIO;
  if(f) { bool ok=fwrite(data,1,strlen(data),f)==strlen(data); ret=fclose(f)==0&&ok?0:-EIO; }
  free(data); if(ret) { portal_install_abort(); return ret; }
  snprintf(install_package,sizeof(install_package),"%s",pkg); snprintf(install_entry,sizeof(install_entry),"%s",entry); return 0;
}
int portal_install_target(const char *relative,char *out,size_t cap)
{
  if(!install_package[0] || !strcmp(relative,"manifest.json")) return -EINVAL;
  int ready=qpk_volume_ready(install_volume,false);if(ready)return ready;
  return portal_safe_path(STAGE,relative,out,cap,true);
}
int portal_install_commit(void)
{
  char path[256],target[192]; struct stat st;
  if(!install_package[0]) return -EINVAL;
  int ready=qpk_volume_ready(install_volume,false);if(ready)return ready;
  snprintf(path,sizeof(path),"%s/%s",STAGE,install_entry);
  if(stat(path,&st)<0 || !S_ISREG(st.st_mode)||st.st_size<=0) return -EINVAL;
  snprintf(target,sizeof(target),"%s/%s",qpk_volume_root(install_volume),install_package);
  if(lstat(target,&st)==0) return -EEXIST;
  if(rename(STAGE,target)<0) return -errno;
  install_package[0]=0; return 0;
}
