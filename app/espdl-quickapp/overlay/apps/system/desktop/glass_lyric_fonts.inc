/* SPDX-License-Identifier: Apache-2.0 */
/* Optional SD families reuse the private, file-enabled TinyTTF backend above.
 * Only checksumming runs on a worker; parsing, caches and fonts stay on UI. */
#include "glass_lyric_font_assets.h"
#ifndef GLASS_LYRIC_FONT_ROOT
#define GLASS_LYRIC_FONT_ROOT "/sdcard/fonts/lyrics"
#endif
#define LYRIC_PAGE_COUNT 64
#define LYRIC_PRELOAD_LIMIT (2u * 1024u * 1024u)
struct lyric_font_face {
  lv_font_t *fonts[SD_FONT_SIZES];
  FILE *file;
  unsigned char *data;
  uint64_t reads,page_misses;
  unsigned opens,created;
  int state;
  bool io_failed;
};
struct lyric_font_page {
  uint32_t offset,used;
  uint64_t stamp;
  unsigned face;
  bool valid;
  unsigned char data[SD_FONT_PAGE_BYTES];
};
struct lyric_font_pool {
  pthread_mutex_t lock;
  struct lyric_font_face faces[GLASS_LYRIC_FACE_COUNT];
  struct lyric_font_page *pages;
  lv_fs_drv_t driver;
  uint64_t clock;
  bool worker,registered;
};
struct lyric_font_file { unsigned face;uint32_t position; };
static struct lyric_font_pool *lyric_fonts;
static const unsigned lyric_counts[SD_FONT_SIZES]={32,48,48,64,64,16,16,16,16,8,12,4};
/* Only the active global face gets the text-page budget. Others retain the
 * bounded lyric budget; descriptor addresses remain stable for live labels. */
static const unsigned lyric_global_counts[SD_FONT_SIZES]={128,192,384,512,384,96,96,96,64,24,32,4};
static unsigned lyric_global_face;
void glass_lyric_font_set_global(unsigned index)
{
  lyric_global_face=index;
  if(!lyric_fonts)return;
  for(unsigned face=0;face<GLASS_LYRIC_FACE_COUNT;face++)
    for(unsigned size=0;size<SD_FONT_SIZES;size++){
      lv_font_t *font=lyric_fonts->faces[face].fonts[size];if(!font)continue;
      ttf_font_desc_t *dsc=(ttf_font_desc_t *)font->dsc;
      unsigned count=face+1==index?lyric_global_counts[size]:lyric_counts[size];
      if(dsc->cache_size==(int)count)continue;
      dsc->cache_size=(int)count;
      lv_cache_set_max_size(dsc->glyph_cache,count,dsc);
      lv_cache_set_max_size(dsc->draw_data_cache,count,dsc);
      /* LVGL changes the limit lazily. Reclaim inactive entries now, while
       * on the GUI owner and outside a draw, without evicting pinned data. */
      while(lv_cache_get_size(dsc->glyph_cache,dsc)>count&&
            lv_cache_evict_one(dsc->glyph_cache,dsc)){}
      while(lv_cache_get_size(dsc->draw_data_cache,dsc)>count&&
            lv_cache_evict_one(dsc->draw_data_cache,dsc)){}

    }
}
void glass_lyric_font_cache_status(unsigned index,struct glass_font_cache_status *status)
{
  memset(status,0,sizeof(*status));status->index=index;
  if(!lyric_fonts||!index||index>GLASS_LYRIC_FACE_COUNT)return;
  struct lyric_font_face *face=&lyric_fonts->faces[index-1];
  status->reads=face->reads;status->page_misses=face->page_misses;status->ready_sizes=face->created;
  status->preloaded_bytes=face->data?lyric_font_assets[index-1].bytes:0;
  if(face->fonts[3]){
    ttf_font_desc_t *dsc=(ttf_font_desc_t *)face->fonts[3]->dsc;
    status->glyph_capacity=dsc->cache_size;
    status->glyph_cached=lv_cache_get_size(dsc->draw_data_cache,dsc);
  }
}

unsigned glass_lyric_font_count(void){return GLASS_LYRIC_FACE_COUNT+1;}
const char *glass_lyric_font_name(unsigned index)
{return glass_tr(index&&index<=GLASS_LYRIC_FACE_COUNT?lyric_font_assets[index-1].name:"默认普惠");}

static bool lyric_font_path(unsigned i,char *path,size_t size)
{
  const char *root=GLASS_LYRIC_FONT_ROOT;
#ifdef GROK_HOST_TEST
  const char *fixture=getenv("LYRIC_FONT_TEST_ROOT");if(fixture)root=fixture;
#endif
  int n=snprintf(path,size,"%s/%s",root,lyric_font_assets[i].file);
  return n>0&&(size_t)n<size;
}
static void *lyric_font_verify(void *arg)
{
  struct lyric_font_pool *pool=arg;
  for(;;){
    pthread_mutex_lock(&pool->lock);
    unsigned i=0;
    while(i<GLASS_LYRIC_FACE_COUNT&&pool->faces[i].state!=1)i++;
    if(i==GLASS_LYRIC_FACE_COUNT){pool->worker=false;pthread_mutex_unlock(&pool->lock);return NULL;}
    pthread_mutex_unlock(&pool->lock);
    char path[256];struct stat st;
    FILE *file=lyric_font_path(i,path,sizeof(path))?fopen(path,"rb"):NULL;
    bool valid=file&&fstat(fileno(file),&st)==0&&S_ISREG(st.st_mode)&&
      st.st_size==lyric_font_assets[i].bytes;
    unsigned char *buffer=valid?malloc(16384):NULL;
    if(!buffer)valid=false;
    /* Cache a small verified font on the checksum worker, never on GUI.
     * At most 2 MiB per eligible pack entry; only the 1.4 MiB Happy face
     * currently qualifies. Allocation failure preserves streaming fallback. */
    unsigned char *preload=valid&&lyric_font_assets[i].bytes<=LYRIC_PRELOAD_LIMIT?
      malloc(lyric_font_assets[i].bytes):NULL;
    uint32_t hash=UINT32_C(2166136261);size_t total=0;
    while(valid&&total<lyric_font_assets[i].bytes){
      size_t wanted=lyric_font_assets[i].bytes-total;if(wanted>16384)wanted=16384;
      size_t n=fread(buffer,1,wanted,file);
      if(n!=wanted){valid=false;break;}
      if(preload)memcpy(preload+total,buffer,n);
      for(size_t j=0;j<n;j++)hash=(hash^buffer[j])*UINT32_C(16777619);
      total+=n;
    }
    valid=valid&&hash==lyric_font_assets[i].fnv;
    free(buffer);if(file)fclose(file);
    if(!valid){free(preload);preload=NULL;}
    pthread_mutex_lock(&pool->lock);free(pool->faces[i].data);pool->faces[i].data=preload;pool->faces[i].state=valid?2:4;pthread_mutex_unlock(&pool->lock);
  }
}

int glass_lyric_font_request(unsigned index)
{
  if(!index)return 0;
  if(index>GLASS_LYRIC_FACE_COUNT)return -EINVAL;
  if(!lyric_fonts){
    struct lyric_font_pool *pool=calloc(1,sizeof(*pool));if(!pool)return -ENOMEM;
    if(pthread_mutex_init(&pool->lock,NULL)){free(pool);return -ENOMEM;}
    lyric_fonts=pool;
  }
  struct lyric_font_pool *pool=lyric_fonts;unsigned i=index-1;
  pthread_mutex_lock(&pool->lock);
  struct lyric_font_face *face=&pool->faces[i];
  if(face->state==4&&face->fonts[0]){pthread_mutex_unlock(&pool->lock);return -EIO;}
  if(face->state==0||face->state==4)face->state=1;
  bool start=!pool->worker&&face->state==1;
  if(start)pool->worker=true;
  pthread_mutex_unlock(&pool->lock);
  if(!start)return 0;
  pthread_attr_t attr;pthread_t thread;int ret=pthread_attr_init(&attr);
  if(!ret){
    ret=pthread_attr_setstacksize(&attr,16384);
    if(!ret)ret=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(!ret)ret=pthread_create(&thread,&attr,lyric_font_verify,pool);
    pthread_attr_destroy(&attr);
  }
  if(ret){pthread_mutex_lock(&pool->lock);pool->worker=false;face->state=4;pthread_mutex_unlock(&pool->lock);}
  return ret?-ret:0;
}
int glass_lyric_font_state(unsigned index)
{
  if(!index)return 3;
  if(index>GLASS_LYRIC_FACE_COUNT)return 4;
  if(!lyric_fonts)return 0;
  pthread_mutex_lock(&lyric_fonts->lock);
  int state=lyric_fonts->faces[index-1].state;
  pthread_mutex_unlock(&lyric_fonts->lock);return state;
}
static void *lyric_font_open(lv_fs_drv_t *drv,const char *path,lv_fs_mode_t mode)
{
  (void)drv;if(mode!=LV_FS_MODE_RD)return NULL;
  unsigned i=0;
  while(i<GLASS_LYRIC_FACE_COUNT&&(path[0]!='/'||strcmp(path+1,lyric_font_assets[i].file)))i++;
  if(i==GLASS_LYRIC_FACE_COUNT)return NULL;
  struct lyric_font_face *face=&lyric_fonts->faces[i];
  struct lyric_font_file *handle=calloc(1,sizeof(*handle));if(!handle)return NULL;
  if(!face->file){
    char full[256];struct stat st;
    FILE *file=lyric_font_path(i,full,sizeof(full))?fopen(full,"rb"):NULL;
    if(file&&(fstat(fileno(file),&st)||!S_ISREG(st.st_mode)||st.st_size!=lyric_font_assets[i].bytes)){
      fclose(file);file=NULL;
    }
    face->file=file;
  }
  if(!face->file){face->io_failed=true;free(handle);return NULL;}
  handle->face=i;face->opens++;return handle;
}
static lv_fs_res_t lyric_font_close(lv_fs_drv_t *drv,void *file)
{
  (void)drv;struct lyric_font_file *handle=file;
  struct lyric_font_face *face=&lyric_fonts->faces[handle->face];
  if(--face->opens==0){fclose(face->file);face->file=NULL;}
  free(handle);return LV_FS_RES_OK;
}
static lv_fs_res_t lyric_font_read(lv_fs_drv_t *drv,void *file,void *buf,uint32_t n,uint32_t *read)
{
  (void)drv;struct lyric_font_file *handle=file;unsigned i=handle->face;
  struct lyric_font_face *face=&lyric_fonts->faces[i];
  face->reads++;
  memset(buf,0,n);*read=0;
  if(face->io_failed)return LV_FS_RES_FS_ERR;
  while(*read<n&&handle->position<lyric_font_assets[i].bytes){
    uint32_t offset=handle->position&~(SD_FONT_PAGE_BYTES-1);
    struct lyric_font_page *page=NULL,*victim=&lyric_fonts->pages[0];
    for(unsigned j=0;j<LYRIC_PAGE_COUNT;j++){
      struct lyric_font_page *candidate=&lyric_fonts->pages[j];
      if(candidate->valid&&candidate->face==i&&candidate->offset==offset){page=candidate;break;}
      if(!candidate->valid||(victim->valid&&candidate->stamp<victim->stamp))victim=candidate;
    }
    if(!page){
      face->page_misses++;
      page=victim;page->valid=false;
      uint32_t wanted=lyric_font_assets[i].bytes-offset;if(wanted>SD_FONT_PAGE_BYTES)wanted=SD_FONT_PAGE_BYTES;
      if(fseek(face->file,offset,SEEK_SET)||fread(page->data,1,wanted,face->file)!=wanted){
        face->io_failed=true;return LV_FS_RES_FS_ERR;
      }
      page->face=i;page->offset=offset;page->used=wanted;page->valid=true;
    }
    page->stamp=++lyric_fonts->clock;
    uint32_t at=handle->position-offset,copy=page->used-at;if(copy>n-*read)copy=n-*read;
    memcpy((unsigned char *)buf+*read,page->data+at,copy);handle->position+=copy;*read+=copy;
  }
  return LV_FS_RES_OK;
}
static lv_fs_res_t lyric_font_seek(lv_fs_drv_t *drv,void *file,uint32_t pos,lv_fs_whence_t whence)
{
  (void)drv;struct lyric_font_file *handle=file;
  uint64_t at=whence==LV_FS_SEEK_SET?pos:whence==LV_FS_SEEK_CUR?(uint64_t)handle->position+pos:
    (uint64_t)lyric_font_assets[handle->face].bytes+pos;
  if(at>lyric_font_assets[handle->face].bytes)return LV_FS_RES_INV_PARAM;
  handle->position=(uint32_t)at;return LV_FS_RES_OK;
}
static lv_fs_res_t lyric_font_tell(lv_fs_drv_t *drv,void *file,uint32_t *pos)
{(void)drv;*pos=((struct lyric_font_file *)file)->position;return LV_FS_RES_OK;}

bool glass_lyric_font_poll(void)
{
  struct lyric_font_pool *pool=lyric_fonts;if(!pool)return false;
  bool changed=false;
  for(unsigned i=0;i<GLASS_LYRIC_FACE_COUNT;i++){
    struct lyric_font_face *face=&pool->faces[i];int state=glass_lyric_font_state(i+1);
    if(state==3&&face->io_failed){
      pthread_mutex_lock(&pool->lock);face->state=4;pthread_mutex_unlock(&pool->lock);changed=true;
    }
    if(state!=2)continue;
    if(!pool->pages)pool->pages=calloc(LYRIC_PAGE_COUNT,sizeof(*pool->pages));
    bool valid=pool->pages!=NULL;
    if(valid&&!pool->registered){
      valid=lv_fs_get_drv('L')==NULL;
      if(valid){
        lv_fs_drv_init(&pool->driver);pool->driver.letter='L';pool->driver.cache_size=0;
        pool->driver.open_cb=lyric_font_open;pool->driver.close_cb=lyric_font_close;
        pool->driver.read_cb=lyric_font_read;pool->driver.seek_cb=lyric_font_seek;pool->driver.tell_cb=lyric_font_tell;
        lv_fs_drv_register(&pool->driver);pool->registered=true;
      }
    }
    if(face->created==0){
      /* Retry a failed check after insertion; keep shared pages hot while
       * building the remaining sizes across later GUI-loop iterations. */
      face->io_failed=false;
      if(pool->pages)for(unsigned page=0;page<LYRIC_PAGE_COUNT;page++)
        if(pool->pages[page].face==i)pool->pages[page].valid=false;
    }
    if(valid){
      unsigned size=face->created;
      char path[96];snprintf(path,sizeof(path),"L:/%s",lyric_font_assets[i].file);
      unsigned count=i+1==lyric_global_face?lyric_global_counts[size]:lyric_counts[size];
      face->fonts[size]=face->data?
        sd_ttf_create_data_ex(face->data,lyric_font_assets[i].bytes,sd_sizes[size],LV_FONT_KERNING_NONE,count):
        sd_ttf_create_file_ex(path,sd_sizes[size],LV_FONT_KERNING_NONE,count);
      valid=face->fonts[size]&&!face->io_failed;
      if(valid&&++face->created<SD_FONT_SIZES)break;
    }
    if(!valid){
      for(unsigned size=0;size<SD_FONT_SIZES;size++)if(face->fonts[size]){
        sd_ttf_destroy(face->fonts[size]);face->fonts[size]=NULL;
      }
      face->created=0;
    }
    pthread_mutex_lock(&pool->lock);face->state=valid?3:4;pthread_mutex_unlock(&pool->lock);
    changed=true;
    break; /* Parse at most one TinyTTF size per GUI poll, keeping touch live. */
  }
  return changed;
}
lv_font_t *glass_lyric_font_get(unsigned index,unsigned pixels)
{
  if(!index||index>GLASS_LYRIC_FACE_COUNT||!lyric_fonts)return NULL;
  struct lyric_font_face *face=&lyric_fonts->faces[index-1];
  if(glass_lyric_font_state(index)!=3||face->io_failed)return NULL;
  for(unsigned size=0;size<SD_FONT_SIZES;size++)if(sd_sizes[size]==pixels)return face->fonts[size];
  return NULL;
}
