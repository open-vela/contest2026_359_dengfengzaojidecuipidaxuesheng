/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_i18n.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "glass_i18n_catalog.h"

#ifndef GLASS_LANGUAGE_PATH
#define GLASS_LANGUAGE_PATH "/sdcard/veladesk-language.txt"
#endif
static enum glass_language language;
static struct {
  pthread_mutex_t lock;
  unsigned requested,generation,restored;
  int result;
  bool started,worker,loaded,consumed,dirty;
} preferences={.lock=PTHREAD_MUTEX_INITIALIZER};

const char *glass_tr(const char *source)
{
  if(!source)return "";
  if(language!=GLASS_LANGUAGE_EN)return source;
  unsigned lo=0,hi=sizeof(glass_english)/sizeof(glass_english[0]);
  while(lo<hi){
    unsigned mid=lo+(hi-lo)/2;int order=strcmp(source,glass_english[mid].source);
    if(!order)return glass_english[mid].english;
    if(order<0)hi=mid;else lo=mid+1;
  }
  return source;
}
enum glass_language glass_i18n_language(void){return language;}
const char *glass_i18n_code(void){return language==GLASS_LANGUAGE_EN?"en":"zh-CN";}
const char *glass_i18n_name(unsigned index){return index==GLASS_LANGUAGE_EN?"English":"简体中文";}

static void *language_worker(void *arg)
{
  (void)arg;
  pthread_mutex_lock(&preferences.lock);
  bool load=!preferences.loaded;
  pthread_mutex_unlock(&preferences.lock);
  if(load){
    unsigned restored=GLASS_LANGUAGE_ZH_CN;FILE *file=NULL;
    /* Mounting can finish after desktop startup. Explicit user selection
     * takes precedence over this delayed restore. */
    for(unsigned i=0;i<15;i++){
      file=fopen(GLASS_LANGUAGE_PATH,"r");if(file)break;
      pthread_mutex_lock(&preferences.lock);bool selected=preferences.generation!=0;pthread_mutex_unlock(&preferences.lock);
      if(selected)break;
      usleep(1000000);
    }
    if(file){
      char code[16],extra;
      if(fscanf(file,"VelaLanguage 1 %15s %c",code,&extra)==1&&!strcmp(code,"en"))restored=GLASS_LANGUAGE_EN;
      fclose(file);
    }
    pthread_mutex_lock(&preferences.lock);
    preferences.restored=restored;preferences.loaded=true;
    pthread_mutex_unlock(&preferences.lock);
  }
  for(;;){
    pthread_mutex_lock(&preferences.lock);
    if(!preferences.dirty){preferences.worker=false;pthread_mutex_unlock(&preferences.lock);return NULL;}
    unsigned requested=preferences.requested,generation=preferences.generation;
    preferences.dirty=false;
    pthread_mutex_unlock(&preferences.lock);
    int error=0;const char *temporary=GLASS_LANGUAGE_PATH ".tmp";
    FILE *file=fopen(temporary,"w");
    if(!file)error=-errno;
    else {
      if(fprintf(file,"VelaLanguage 1 %s\n",requested==GLASS_LANGUAGE_EN?"en":"zh-CN")<0||
         fflush(file)||fsync(fileno(file)))error=-EIO;
      if(fclose(file)&&!error)error=-EIO;
      if(!error&&rename(temporary,GLASS_LANGUAGE_PATH))error=-errno;
      if(error)unlink(temporary);
    }
    pthread_mutex_lock(&preferences.lock);
    if(generation==preferences.generation)preferences.result=error?error:1;
    pthread_mutex_unlock(&preferences.lock);
  }
}
static int language_worker_start(void)
{
  pthread_mutex_lock(&preferences.lock);
  if(preferences.worker){pthread_mutex_unlock(&preferences.lock);return 0;}
  preferences.worker=true;
  pthread_mutex_unlock(&preferences.lock);
  pthread_attr_t attr;pthread_t thread;int ret=pthread_attr_init(&attr);
  if(!ret){
    ret=pthread_attr_setstacksize(&attr,16384);
    if(!ret)ret=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(!ret)ret=pthread_create(&thread,&attr,language_worker,NULL);
    pthread_attr_destroy(&attr);
  }
  if(ret){pthread_mutex_lock(&preferences.lock);preferences.worker=false;pthread_mutex_unlock(&preferences.lock);}
  return ret?-ret:0;
}
void glass_i18n_start(void)
{
  if(preferences.started)return;
  preferences.started=true;language_worker_start();
}
bool glass_i18n_poll(void)
{
  pthread_mutex_lock(&preferences.lock);
  bool changed=preferences.loaded&&!preferences.consumed&&!preferences.generation&&language!=preferences.restored;
  if(preferences.loaded){
    preferences.consumed=true;
    if(!preferences.generation)language=preferences.restored;
  }
  pthread_mutex_unlock(&preferences.lock);return changed;
}
int glass_i18n_set_language(unsigned selected)
{
  if(selected>=GLASS_LANGUAGE_COUNT)return -EINVAL;
  pthread_mutex_lock(&preferences.lock);
  /* Even selecting the visible default cancels delayed restoration. */
  bool changed=language!=selected;
  bool save=changed||!preferences.consumed;
  preferences.generation++;
  if(save){preferences.requested=selected;preferences.dirty=true;preferences.result=0;}
  pthread_mutex_unlock(&preferences.lock);
  language=selected;
  return save?language_worker_start():0;
}
int glass_i18n_save_status(void)
{
  pthread_mutex_lock(&preferences.lock);
  int result=preferences.result;preferences.result=0;
  pthread_mutex_unlock(&preferences.lock);return result;
}
