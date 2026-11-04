/* SPDX-License-Identifier: Apache-2.0
 * SD material data only; no executable code or model/configuration loading.
 * Calls copy bytes while holding the lock, never publish cache-page pointers.
 * Pressure drops only unborrowed pages. Explicit blobs belong to their caller.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "glass_material.h"
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stdatomic.h>
#ifndef GLASS_MATERIAL_ROOT
#define GLASS_MATERIAL_ROOT "/sdcard/desktop-assets/v2"
#endif
#define GMAT_BLOCK 16384u
#define GMAT_PAGES 4u
struct gmat_record {const char *path;uint32_t raw,stored,crc,prefix_crc,blocks;bool compressed;};
#include "glass_material_catalog.inc"
#include "glass_material_pinyin.inc"
struct gmat_page {unsigned id,block;uint64_t stamp;unsigned char *data;size_t bytes;};
struct gmat_state {struct gmat_stats stats;struct gmat_page pages[GMAT_PAGES];uint64_t clock;};
static struct gmat_state *state;
static pthread_mutex_t material_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_bool boot_enabled=true;
void gmat_boot_enable(bool enabled){atomic_store(&boot_enabled,enabled);}
bool gmat_boot_enabled(void){return atomic_load(&boot_enabled);}
static uint32_t u32(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static uint32_t crc_update(uint32_t crc,const unsigned char *p,size_t n)
{while(n--){crc^=*p++;for(unsigned i=0;i<8;i++)crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);}return crc;}
static uint32_t crc32(const void *p,size_t n){return ~crc_update(UINT32_MAX,p,n);}
int gmat_lz4_decode(const unsigned char *in,size_t n,unsigned char *out,size_t capacity)
{
 if((!in&&n)||(!out&&capacity)||n>INT_MAX||capacity>INT_MAX)return -EINVAL;
 size_t ip=0,op=0;
 while(ip<n){unsigned token=in[ip++];size_t literal=token>>4;
  if(literal==15){unsigned v;do{if(ip>=n)return -EBADMSG;v=in[ip++];if(v>capacity||literal>capacity-v)return -EBADMSG;literal+=v;}while(v==255);}
  if(literal>n-ip||literal>capacity-op)return -EBADMSG;
  memcpy(out+op,in+ip,literal);ip+=literal;op+=literal;
  if(ip==n)return op==capacity?(int)op:-EBADMSG;
  if(n-ip<2)return -EBADMSG;size_t distance=in[ip]|(size_t)in[ip+1]<<8;ip+=2;
  if(!distance||distance>op)return -EBADMSG;size_t length=(token&15u)+4u;
  if((token&15u)==15){unsigned v;do{if(ip>=n)return -EBADMSG;v=in[ip++];if(v>capacity||length>capacity-v)return -EBADMSG;length+=v;}while(v==255);}
  if(length>capacity-op)return -EBADMSG;
  for(size_t i=0;i<length;i++){out[op]=out[op-distance];op++;}
 }
 return op==capacity?(int)op:-EBADMSG;
}
size_t gmat_size(unsigned id){return id<sizeof(gmat_records)/sizeof(gmat_records[0])?gmat_records[id].raw:0;}
static struct gmat_state *get_state(void)
{if(!state)state=calloc(1,sizeof(*state));return state;}
static void peak(void)
{size_t n=state->stats.cache_bytes+state->stats.blob_bytes;if(n>state->stats.peak_bytes)state->stats.peak_bytes=n;}
static void drop_pages(void)
{if(state)for(unsigned i=0;i<GMAT_PAGES;i++){struct gmat_page *p=&state->pages[i];if(p->data){state->stats.cache_bytes-=GMAT_BLOCK;free(p->data);memset(p,0,sizeof(*p));}}}
void gmat_trim(void){pthread_mutex_lock(&material_lock);drop_pages();pthread_mutex_unlock(&material_lock);}
void gmat_pressure(bool active)
{pthread_mutex_lock(&material_lock);struct gmat_state *s=get_state();if(s){s->stats.pressure=active;drop_pages();}pthread_mutex_unlock(&material_lock);}
bool gmat_under_pressure(void)
{pthread_mutex_lock(&material_lock);bool active=state&&state->stats.pressure;pthread_mutex_unlock(&material_lock);return active;}
void gmat_status(struct gmat_stats *out)
{if(!out)return;pthread_mutex_lock(&material_lock);if(state)*out=state->stats;else memset(out,0,sizeof(*out));pthread_mutex_unlock(&material_lock);}
/* Prefix/header and every block's expected CRC are bound by generated catalog.
 * Do not trust offsets, sizes, codec IDs or raw flags from the removable media. */
static int load_block(unsigned id,unsigned block,unsigned char *out,size_t expected)
{
 const struct gmat_record *r=&gmat_records[id];char path[384];
 int length=snprintf(path,sizeof(path),"%s/%s",GLASS_MATERIAL_ROOT,r->path);
 if(length<0||(size_t)length>=sizeof(path))return -ENAMETOOLONG;
 bool builtin=id==GMAT_BUILTIN_PINYIN;
 FILE *f=builtin?fmemopen((void *)material_pinyin_fallback,sizeof(material_pinyin_fallback),"rb"):fopen(path,"rb");if(!f)return -errno;struct stat st;int ret=-EBADMSG;
 if(!builtin&&(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)||st.st_size!=(off_t)r->stored))goto done;
 if(!r->compressed){
  /* Raw JPEGs are small; verify the complete source before passing it to JPEG. */
  unsigned char scratch[1024];uint32_t crc=UINT32_MAX;size_t total=0,n;
  while((n=fread(scratch,1,sizeof(scratch),f))){crc=crc_update(crc,scratch,n);total+=n;}
  if(ferror(f)||total!=r->raw||~crc!=r->crc)goto done;
  if(fseek(f,(long)((size_t)block*GMAT_BLOCK),SEEK_SET)||fread(out,1,expected,f)!=expected)goto done;
 }else{
  unsigned char h[32],entry[16];
  if(fread(h,1,32,f)!=32||memcmp(h,"GAC1",4)||h[4]!=1||h[5]||h[6]!=2||h[7]||u32(h+8)!=GMAT_BLOCK||u32(h+12)!=r->blocks||u32(h+16)!=r->raw||u32(h+20)||u32(h+24)!=r->crc||u32(h+28))goto done;
  uint32_t crc=crc_update(UINT32_MAX,h,32);size_t next=32u+16u*r->blocks;bool found=false;
  for(unsigned i=0;i<r->blocks;i++){
   unsigned char e[16];if(fread(e,1,16,f)!=16)goto done;crc=crc_update(crc,e,16);
   uint32_t at=u32(e),stored=u32(e+4),raw=u32(e+8);size_t count=stored&0x7fffffffu;
   size_t want=r->raw-(size_t)i*GMAT_BLOCK;if(want>GMAT_BLOCK)want=GMAT_BLOCK;
   if(at!=next||!count||count>GMAT_BLOCK||raw!=want||at>r->stored||count>r->stored-at||((stored>>31)&&count!=raw))goto done;
   next+=count;if(i==block){memcpy(entry,e,16);found=true;}
  }
  if(!found||next!=r->stored||~crc!=r->prefix_crc)goto done;
  size_t encoded=u32(entry+4)&0x7fffffffu;unsigned char *input=malloc(encoded);if(!input){ret=-ENOMEM;goto done;}
  if(fseek(f,u32(entry),SEEK_SET)||fread(input,1,encoded,f)!=encoded){free(input);goto done;}
  if(u32(entry+4)>>31){memcpy(out,input,expected);ret=0;}else ret=gmat_lz4_decode(input,encoded,out,expected)==(int)expected?0:-EBADMSG;
  free(input);if(ret||crc32(out,expected)!=u32(entry+12)){ret=-EBADMSG;goto done;}
 }
 state->stats.reads++;state->stats.decoded_bytes+=expected;ret=0;
 done:fclose(f);return ret;
}
int gmat_read(unsigned id,size_t offset,void *dst,size_t bytes)
{
 size_t size=gmat_size(id);if(!size||offset>size||bytes>size-offset||(!dst&&bytes))return -EINVAL;if(!bytes)return 0;
 if(id!=GMAT_BUILTIN_PINYIN&&!gmat_boot_enabled()){memset(dst,0,bytes);return -EAGAIN;}
 memset(dst,0,bytes);pthread_mutex_lock(&material_lock);struct gmat_state *s=get_state();if(!s){pthread_mutex_unlock(&material_lock);return -ENOMEM;}
 int ret=0;size_t copied=0;
 while(copied<bytes){unsigned block=(unsigned)(offset/GMAT_BLOCK);size_t at=offset%GMAT_BLOCK;struct gmat_page *p=NULL,*victim=&s->pages[0];
  for(unsigned i=0;i<(s->stats.pressure?1u:GMAT_PAGES);i++){struct gmat_page *c=&s->pages[i];if(c->data&&c->id==id&&c->block==block){p=c;break;}if(!c->data||(victim->data&&c->stamp<victim->stamp))victim=c;}
  if(!p){p=victim;if(!p->data){p->data=malloc(GMAT_BLOCK);if(!p->data){ret=-ENOMEM;break;}s->stats.cache_bytes+=GMAT_BLOCK;peak();}
   size_t wanted=size-(size_t)block*GMAT_BLOCK;if(wanted>GMAT_BLOCK)wanted=GMAT_BLOCK;
   p->bytes=0;ret=load_block(id,block,p->data,wanted);if(ret){s->stats.cache_bytes-=GMAT_BLOCK;free(p->data);memset(p,0,sizeof(*p));break;}
   p->id=id;p->block=block;p->bytes=wanted;
  }else s->stats.hits++;
  p->stamp=++s->clock;size_t count=p->bytes-at;if(count>bytes-copied)count=bytes-copied;
  memcpy((unsigned char *)dst+copied,p->data+at,count);offset+=count;copied+=count;
 }
 if(ret){s->stats.errors++;memset(dst,0,bytes);}pthread_mutex_unlock(&material_lock);return ret;
}
int gmat_verify(unsigned id)
{
 size_t n=gmat_size(id);if(!n)return -EINVAL;unsigned char *buffer=malloc(GMAT_BLOCK);if(!buffer)return -ENOMEM;uint32_t crc=UINT32_MAX;int ret=0;
 for(size_t at=0;at<n;){size_t count=n-at;if(count>GMAT_BLOCK)count=GMAT_BLOCK;ret=gmat_read(id,at,buffer,count);if(ret)break;crc=crc_update(crc,buffer,count);at+=count;}
 free(buffer);return ret?ret:(~crc==gmat_records[id].crc?0:-EBADMSG);
}
union blob_header {struct {size_t size;uint32_t magic;} meta;long double alignment;void *pointer;};
void *gmat_acquire_background(unsigned id,bool (*cancel)(void))
{
 size_t bytes=gmat_size(id);if(!bytes||bytes>SIZE_MAX-sizeof(union blob_header)||!gmat_boot_enabled()||gmat_under_pressure())return NULL;
 union blob_header *h=malloc(sizeof(*h)+bytes);if(!h)return NULL;h->meta.size=bytes;h->meta.magic=0x474d4231u;
 pthread_mutex_lock(&material_lock);struct gmat_state *s=get_state();
 if(!s){pthread_mutex_unlock(&material_lock);free(h);return NULL;}
 s->stats.blob_bytes+=bytes;s->stats.blobs++;peak();pthread_mutex_unlock(&material_lock);
 uint32_t crc=UINT32_MAX;
 for(size_t at=0;at<bytes;){
  size_t count=bytes-at;if(count>GMAT_BLOCK)count=GMAT_BLOCK;
  if((cancel&&cancel())||!gmat_boot_enabled()||gmat_under_pressure()||gmat_read(id,at,(unsigned char *)(h+1)+at,count)){gmat_release(h+1);return NULL;}
  crc=crc_update(crc,(unsigned char *)(h+1)+at,count);at+=count;
 }
 if((cancel&&cancel())||gmat_under_pressure()||~crc!=gmat_records[id].crc){gmat_release(h+1);return NULL;}
 return h+1;
}
void *gmat_acquire(unsigned id,size_t offset,size_t bytes)
{
 size_t size=gmat_size(id);if(!bytes||offset>size||bytes>size-offset||bytes>SIZE_MAX-sizeof(union blob_header))return NULL;
 union blob_header *h=malloc(sizeof(*h)+bytes);if(!h)return NULL;
 h->meta.size=bytes;h->meta.magic=0x474d4231u;
 if(gmat_read(id,offset,h+1,bytes)){free(h);return NULL;}
 pthread_mutex_lock(&material_lock);state->stats.blob_bytes+=bytes;state->stats.blobs++;peak();pthread_mutex_unlock(&material_lock);return h+1;
}
void gmat_release(void *data)
{
 if(!data)return;union blob_header *h=(union blob_header *)data-1;
 if(h->meta.magic!=0x474d4231u)return;
 pthread_mutex_lock(&material_lock);state->stats.blob_bytes-=h->meta.size;state->stats.blobs--;h->meta.magic=0;pthread_mutex_unlock(&material_lock);free(h);
}
struct material_file {unsigned id;size_t at;};
static ssize_t material_read(void *p,char *dst,size_t n)
{struct material_file *f=p;size_t left=gmat_size(f->id)-f->at;if(n>left)n=left;int ret=gmat_read(f->id,f->at,dst,n);if(ret){errno=-ret;return -1;}f->at+=n;return (ssize_t)n;}
#ifdef __NuttX__
static off_t material_seek(void *p,off_t *offset,int whence)
#else
static int material_seek(void *p,off_t *offset,int whence)
#endif
{struct material_file *f=p;int64_t base=whence==SEEK_SET?0:whence==SEEK_CUR?(int64_t)f->at:whence==SEEK_END?(int64_t)gmat_size(f->id):-1;
 if(base<0||*offset< -base||*offset>(int64_t)gmat_size(f->id)-base){errno=EINVAL;return -1;}f->at=(size_t)(base+*offset);*offset=(off_t)f->at;
#ifdef __NuttX__
 return *offset;
#else
 return 0;
#endif
}
static int material_close(void *p){free(p);return 0;}
FILE *gmat_fopen(unsigned id)
{if(!gmat_size(id)){errno=EINVAL;return NULL;}struct material_file *f=calloc(1,sizeof(*f));if(!f)return NULL;f->id=id;
 cookie_io_functions_t ops={.read=material_read,.seek=material_seek,.close=material_close};FILE *file=fopencookie(f,"rb",ops);if(!file)free(f);return file;}
