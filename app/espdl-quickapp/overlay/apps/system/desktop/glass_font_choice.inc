/* SPDX-License-Identifier: Apache-2.0 */
#ifndef GLASS_FONT_CHOICE_PATH
#define GLASS_FONT_CHOICE_PATH "/sdcard/fonts/global-font.txt"
#endif
static struct {
  pthread_mutex_t lock;
  unsigned loaded_index,requested,generation;
  int result;
  bool loaded,worker;
} font_choice={.lock=PTHREAD_MUTEX_INITIALIZER};

static void font_choice_read(void)
{
  unsigned version=0,index=0;char extra;
  FILE *file=fopen(GLASS_FONT_CHOICE_PATH,"r");
  if(file){
    if(fscanf(file,"VelaGlobalFont %u %u %c",&version,&index,&extra)!=2||
       version!=1||index>=glass_lyric_font_count())index=0;
    fclose(file);
  }
  pthread_mutex_lock(&font_choice.lock);
  font_choice.loaded_index=index;font_choice.loaded=true;
  pthread_mutex_unlock(&font_choice.lock);
}
bool glass_font_choice_load(unsigned *index)
{
  pthread_mutex_lock(&font_choice.lock);
  bool loaded=font_choice.loaded;*index=font_choice.loaded_index;
  pthread_mutex_unlock(&font_choice.lock);return loaded;
}
static void *font_choice_write(void *arg)
{
  LV_UNUSED(arg);
  for(;;){
    pthread_mutex_lock(&font_choice.lock);
    unsigned index=font_choice.requested,generation=font_choice.generation;
    pthread_mutex_unlock(&font_choice.lock);
    const char *temporary=GLASS_FONT_CHOICE_PATH ".tmp";
    int error=0;FILE *file=fopen(temporary,"w");
    if(!file)error=-errno;
    else {
      if(fprintf(file,"VelaGlobalFont 1 %u\n",index)<0||fflush(file)||fsync(fileno(file)))error=-EIO;
      if(fclose(file)&&!error)error=-EIO;
      if(!error&&rename(temporary,GLASS_FONT_CHOICE_PATH))error=-errno;
      if(error)unlink(temporary);
    }
    pthread_mutex_lock(&font_choice.lock);
    if(generation==font_choice.generation){
      font_choice.result=error?error:1;font_choice.worker=false;
      pthread_mutex_unlock(&font_choice.lock);return NULL;
    }
    pthread_mutex_unlock(&font_choice.lock);
  }
}
int glass_font_choice_save(unsigned index)
{
  if(index>=glass_lyric_font_count())return -EINVAL;
  pthread_mutex_lock(&font_choice.lock);
  font_choice.requested=index;font_choice.generation++;font_choice.result=0;
  if(font_choice.worker){pthread_mutex_unlock(&font_choice.lock);return 0;}
  font_choice.worker=true;
  pthread_mutex_unlock(&font_choice.lock);
  pthread_attr_t attr;pthread_t thread;int ret=pthread_attr_init(&attr);
  if(!ret){
    ret=pthread_attr_setstacksize(&attr,16384);
    if(!ret)ret=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(!ret)ret=pthread_create(&thread,&attr,font_choice_write,NULL);
    pthread_attr_destroy(&attr);
  }
  if(ret){pthread_mutex_lock(&font_choice.lock);font_choice.worker=false;pthread_mutex_unlock(&font_choice.lock);}
  return ret?-ret:0;
}
int glass_font_choice_save_status(void)
{
  pthread_mutex_lock(&font_choice.lock);
  int result=font_choice.result;font_choice.result=0;
  pthread_mutex_unlock(&font_choice.lock);return result;
}
