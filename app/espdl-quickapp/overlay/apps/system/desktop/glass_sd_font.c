/* SPDX-License-Identifier: Apache-2.0
 * Private file-enabled TinyTTF instance. Reuse the pinned LVGL implementation,
 * with private public symbols; the firmware's existing memory fonts and Kconfig
 * stay unchanged. STB symbols inside LVGL are already static.
 */
#include "glass_sd_font.h"
#include "glass_i18n.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#undef LV_TINY_TTF_FILE_SUPPORT
#define LV_TINY_TTF_FILE_SUPPORT 1
#define lv_tiny_ttf_set_size sd_ttf_set_size
#define lv_tiny_ttf_destroy sd_ttf_destroy
#define lv_tiny_ttf_create_file_ex sd_ttf_create_file_ex
#define lv_tiny_ttf_create_file sd_ttf_create_file
#define lv_tiny_ttf_create_data_ex sd_ttf_create_data_ex
#define lv_tiny_ttf_create_data sd_ttf_create_data
#include <lvgl/src/libs/tiny_ttf/lv_tiny_ttf.c>

#ifndef GLASS_SD_FONT_PATH
#define GLASS_SD_FONT_PATH SD_FONT_DEFAULT_PATH
#endif
#define SD_FONT_SIZES 12
#define SD_FONT_PAGE_BYTES 4096u
#define SD_FONT_PAGE_COUNT 32u
struct sd_font_page {
  uint32_t offset,used;
  uint64_t stamp;
  bool valid;
  unsigned char data[SD_FONT_PAGE_BYTES];
};
struct sd_font_state {
  pthread_mutex_t lock;
  lv_fs_drv_t driver;
  lv_font_t *fonts[SD_FONT_SIZES];
  FILE *file;
  unsigned open_count;
  struct sd_font_page *pages;
  uint64_t clock;
  int state,error;
  bool checked,consumed,io_failed;
};
/* Keep descriptors/caches in PSRAM, not scarce internal static RAM. */
static struct sd_font_state *sd_fonts;
static const unsigned sd_sizes[SD_FONT_SIZES]={12,14,16,20,28,32,34,40,48,64,72,128};
static const unsigned sd_counts[SD_FONT_SIZES]={48,64,96,128,128,24,24,24,32,16,24,4};
struct sd_font_file { uint32_t position; };
#include "glass_font_choice.inc"

static void *sd_font_check(void *arg)
{
  struct sd_font_state *s=arg;
  FILE *file=NULL;struct stat st;int error=-ENOENT;
  /* SD mounting may finish after desktop startup. */
  for(unsigned attempt=0;attempt<15;attempt++) {
    if(stat(GLASS_SD_FONT_PATH,&st)==0) {
      if(!S_ISREG(st.st_mode)||st.st_size!=SD_FONT_BYTES){error=-EINVAL;break;}
      file=fopen(GLASS_SD_FONT_PATH,"rb");error=file?0:-errno;break;
    }
    usleep(1000000);
  }
  unsigned char *buffer=file?malloc(16384):NULL;
  if(file&&!buffer)error=-ENOMEM;
  uint32_t hash=UINT32_C(2166136261);size_t total=0;
  if(!error) {
    size_t n;
    while((n=fread(buffer,1,16384,file))!=0) {
      total+=n;
      for(size_t i=0;i<n;i++)hash=(hash^buffer[i])*UINT32_C(16777619);
      if(total>SD_FONT_BYTES){error=-EINVAL;break;}
    }
    if(ferror(file)||total!=SD_FONT_BYTES||hash!=SD_FONT_FNV)error=-EIO;
  }
  free(buffer);if(file)fclose(file);
  font_choice_read();
  pthread_mutex_lock(&s->lock);
  s->error=error;s->checked=true;
  pthread_mutex_unlock(&s->lock);
  return NULL;
}

static void *sd_font_open(lv_fs_drv_t *drv,const char *path,lv_fs_mode_t mode)
{
  (void)drv;
  if(mode!=LV_FS_MODE_RD||strcmp(path,"/font.ttf"))return NULL;
  struct sd_font_file *handle=calloc(1,sizeof(*handle));if(!handle)return NULL;
  if(!sd_fonts->file) {
    FILE *f=fopen(GLASS_SD_FONT_PATH,"rb");struct stat st;
    if(f&&(fstat(fileno(f),&st)<0||!S_ISREG(st.st_mode)||st.st_size!=SD_FONT_BYTES)) {
      fclose(f);f=NULL;
    }
    sd_fonts->file=f;
  }
  if(!sd_fonts->file){sd_fonts->io_failed=true;free(handle);return NULL;}
  sd_fonts->open_count++;return handle;
}
static lv_fs_res_t sd_font_close(lv_fs_drv_t *drv,void *file)
{
  (void)drv;free(file);
  if(--sd_fonts->open_count==0){fclose(sd_fonts->file);sd_fonts->file=NULL;}
  return LV_FS_RES_OK;
}
static lv_fs_res_t sd_font_read(lv_fs_drv_t *drv,void *file,void *buf,uint32_t n,uint32_t *read)
{
  (void)drv;
  /* TinyTTF does not propagate stream errors. Never leave uninitialized
   * bytes in its parser; detach all fallbacks on the next UI iteration. */
  struct sd_font_file *handle=file;FILE *source=sd_fonts->file;
  memset(buf,0,n);*read=0;
  if(sd_fonts->io_failed)return LV_FS_RES_FS_ERR;
  while(*read<n&&handle->position<SD_FONT_BYTES){
    uint32_t offset=handle->position&~(SD_FONT_PAGE_BYTES-1);
    struct sd_font_page *page=NULL,*victim=&sd_fonts->pages[0];
    for(unsigned i=0;i<SD_FONT_PAGE_COUNT;i++){
      struct sd_font_page *candidate=&sd_fonts->pages[i];
      if(candidate->valid&&candidate->offset==offset){page=candidate;break;}
      if(!candidate->valid||(victim->valid&&candidate->stamp<victim->stamp))victim=candidate;
    }
    if(!page){
      page=victim;page->valid=false;
      uint32_t wanted=SD_FONT_BYTES-offset;
      if(wanted>SD_FONT_PAGE_BYTES)wanted=SD_FONT_PAGE_BYTES;
      if(fseek(source,offset,SEEK_SET)||fread(page->data,1,wanted,source)!=wanted){
        sd_fonts->io_failed=true;return LV_FS_RES_FS_ERR;
      }
      page->offset=offset;page->used=wanted;page->valid=true;
    }
    page->stamp=++sd_fonts->clock;
    uint32_t at=handle->position-offset,copy=page->used-at;
    if(copy>n-*read)copy=n-*read;
    memcpy((unsigned char *)buf+*read,page->data+at,copy);
    handle->position+=copy;*read+=copy;
  }
  return LV_FS_RES_OK;
}
static lv_fs_res_t sd_font_seek(lv_fs_drv_t *drv,void *file,uint32_t pos,lv_fs_whence_t whence)
{
  (void)drv;
  struct sd_font_file *handle=file;
  uint64_t at=whence==LV_FS_SEEK_SET?pos:
    whence==LV_FS_SEEK_CUR?(uint64_t)handle->position+pos:(uint64_t)SD_FONT_BYTES+pos;
  if(at>SD_FONT_BYTES)return LV_FS_RES_INV_PARAM;
  handle->position=(uint32_t)at;return LV_FS_RES_OK;
}
static lv_fs_res_t sd_font_tell(lv_fs_drv_t *drv,void *file,uint32_t *pos)
{
  (void)drv;struct sd_font_file *handle=file;*pos=handle->position;return LV_FS_RES_OK;
}
void glass_sd_font_start(void)
{
  if(sd_fonts)return;
  struct sd_font_state *s=calloc(1,sizeof(*s));if(!s)return;
  if(pthread_mutex_init(&s->lock,NULL)){free(s);return;}
  sd_fonts=s;s->state=1;
  pthread_attr_t attr;pthread_t thread;
  int ret=pthread_attr_init(&attr);
  if(!ret){ret=pthread_attr_setstacksize(&attr,16384);
    if(!ret)ret=pthread_create(&thread,&attr,sd_font_check,s);
    pthread_attr_destroy(&attr);}
  if(ret){s->checked=true;s->error=-ret;}else pthread_detach(thread);
}
int glass_sd_font_poll(void)
{
  struct sd_font_state *s=sd_fonts;if(!s)return 0;
  if(s->io_failed&&s->state==2){s->state=3;s->error=-EIO;return -1;}
  if(s->consumed)return 0;
  pthread_mutex_lock(&s->lock);
  bool done=s->checked;
  pthread_mutex_unlock(&s->lock);
  if(!done)return 0;
  s->consumed=true;
  if(s->error){s->state=3;return 0;}
  if(lv_fs_get_drv('J')){s->state=3;s->error=-EBUSY;return 0;}
  s->pages=calloc(SD_FONT_PAGE_COUNT,sizeof(*s->pages));
  if(!s->pages){s->state=3;s->error=-ENOMEM;return 0;}
  /* One shared LRU keeps cmap/hmtx/loca and outlines hot across all sizes.
   * A single LVGL stream cache otherwise evicts these tables on every glyph. */
  lv_fs_drv_init(&s->driver);s->driver.letter='J';s->driver.cache_size=0;
  s->driver.open_cb=sd_font_open;s->driver.close_cb=sd_font_close;
  s->driver.read_cb=sd_font_read;s->driver.seek_cb=sd_font_seek;s->driver.tell_cb=sd_font_tell;
  lv_fs_drv_register(&s->driver);
  for(unsigned i=0;i<SD_FONT_SIZES;i++) {
    s->fonts[i]=sd_ttf_create_file_ex("J:/font.ttf",sd_sizes[i],LV_FONT_KERNING_NONE,sd_counts[i]);
    if(!s->fonts[i]||s->io_failed) {
      for(unsigned j=0;j<=i;j++)if(s->fonts[j]){sd_ttf_destroy(s->fonts[j]);s->fonts[j]=NULL;}
      free(s->pages);s->pages=NULL;
      s->state=3;s->error=-ENOMEM;return 0;
    }
  }
  s->state=2;return 1;
}
lv_font_t *glass_sd_font_get(unsigned size)
{
  if(sd_fonts&&sd_fonts->state==2)
    for(unsigned i=0;i<SD_FONT_SIZES;i++)if(sd_sizes[i]==size)return sd_fonts->fonts[i];
  return NULL;
}
void glass_sd_font_status(int *state,int *error)
{
  *state=0;*error=0;
  if(sd_fonts){pthread_mutex_lock(&sd_fonts->lock);
    *state=sd_fonts->state;*error=sd_fonts->error;
    pthread_mutex_unlock(&sd_fonts->lock);}
}

#include "glass_lyric_fonts.inc"
