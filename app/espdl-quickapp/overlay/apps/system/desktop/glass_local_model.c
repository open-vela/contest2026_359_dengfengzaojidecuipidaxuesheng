/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_model.h"
#ifdef GLV_Q4_PIE
#include "local_q4_pie.h"
#include "local_q8_pie.h"
#endif
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef __NuttX__
#include <malloc.h>
#endif

union glv_header {struct {size_t size;struct glv_memory *owner;} allocation;uint64_t alignment[4];};
struct glv_verified_asset {
  struct glv_verified_asset *next;
  FILE *file;unsigned char *io;size_t bytes;
  struct stat identity;
  unsigned char digest[32];char path[384];
};
struct glv_row_cache {
  struct glv_row_cache *next;
  const struct glv_tensor *tensor;
  size_t row_bytes,rows;
  unsigned char contents[]; /* raw rows followed by their valid bitmap */
};
uint64_t glv_now_us(void)
{struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
size_t glv_alloc_charge(size_t n)
{return !n||n>SIZE_MAX-sizeof(union glv_header)?SIZE_MAX:n+sizeof(union glv_header);}
void *glv_alloc(struct glv_memory *m,size_t n)
{
  if(!m||!n||n>SIZE_MAX-sizeof(union glv_header))return NULL;
  size_t total=n+sizeof(union glv_header);
  if(m->used>m->limit||total>m->limit-m->used)return NULL;
#ifdef __NuttX__
  struct mallinfo mi=mallinfo();
  if((size_t)mi.fordblks<total+GLV_SYSTEM_RESERVE)return NULL;
#endif
  union glv_header *p=calloc(1,total);if(!p)return NULL;
  p->allocation.size=total;p->allocation.owner=m;m->used+=total;
  if(m->used>m->peak)m->peak=m->used;
  return p+1;
}
void glv_free(void *data)
{
  if(!data)return;union glv_header *p=(union glv_header *)data-1;
  p->allocation.owner->used-=p->allocation.size;free(p);
}
int glv_cancel(struct glv_memory *m)
{return m&&m->cancelled&&m->cancelled(m->opaque)?-ECANCELED:0;}
uint32_t glv_u32(const unsigned char *p)
{return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
int glv_read_exact(FILE *f,void *p,size_t n)
{return fread(p,1,n,f)==n?0:-EIO;}
int glv_open_asset(const char *path,FILE **out,size_t *bytes)
{
  struct stat st;*out=NULL;
  if(lstat(path,&st))return -errno;
  if(!S_ISREG(st.st_mode)||S_ISLNK(st.st_mode)||st.st_size<0)return -EINVAL;
  FILE *f=fopen(path,"rb");if(!f)return -errno;
  if(fstat(fileno(f),&st)||!S_ISREG(st.st_mode)){fclose(f);return -EIO;}
  *bytes=(size_t)st.st_size;*out=f;return 0;
}
int glv_verified_keep(struct glv_memory *memory,const char *path,FILE *file,unsigned char *io,size_t bytes,const unsigned char digest[32])
{
  if(strlen(path)>=384)return -ENAMETOOLONG;
  struct glv_verified_asset *v=glv_alloc(memory,sizeof(*v));if(!v)return -ENOMEM;
  if(fstat(fileno(file),&v->identity)||(size_t)v->identity.st_size!=bytes){glv_free(v);return -EIO;}
  v->file=file;v->io=io;v->bytes=bytes;memcpy(v->digest,digest,32);strcpy(v->path,path);
  v->next=memory->verified;memory->verified=v;return 0;
}
void glv_verified_clear(struct glv_memory *memory)
{
  while(memory->verified){struct glv_verified_asset *v=memory->verified;memory->verified=v->next;
    fclose(v->file);glv_free(v->io);glv_free(v);}
  memory->retain_verified=false;
}
/* Transfer the exact verified stream, not a pathname-only SHA exemption.
 * A receipt is one-use, belongs to this request, and must agree with the
 * model index digest and current file identity. */
static int verified_take(struct glv_model *model,const char *path,const unsigned char digest[32])
{
  struct glv_memory *memory=model->memory;
  for(struct glv_verified_asset **link=&memory->verified;*link;link=&(*link)->next){
    struct glv_verified_asset *v=*link;if(strcmp(v->path,path))continue;
    struct stat held,named;int ret=glv_cancel(memory);
    if(!ret&&(fstat(fileno(v->file),&held)||lstat(path,&named)))ret=-EIO;
    if(!ret&&(!S_ISREG(named.st_mode)||held.st_dev!=v->identity.st_dev||held.st_ino!=v->identity.st_ino||
      held.st_size!=v->identity.st_size||held.st_mtime!=v->identity.st_mtime||held.st_ctime!=v->identity.st_ctime||
      named.st_dev!=held.st_dev||named.st_ino!=held.st_ino||named.st_size!=held.st_size||
      v->bytes!=model->bytes||memcmp(v->digest,digest,32)))ret=-EBADMSG;
    if(!ret&&fseek(v->file,0,SEEK_SET))ret=-EIO;
    *link=v->next;
    if(!ret){model->file=v->file;model->io=v->io;memory->profile.verified_reuses++;}
    else {fclose(v->file);glv_free(v->io);}
    glv_free(v);return ret?ret:1;
  }return 0;
}
void glv_model_close(struct glv_model *m)
{
  glv_model_block_release(m);
  glv_model_cache_clear(m);
  if(m->file)fclose(m->file);
  glv_free(m->io);glv_free(m->data_storage?m->data_storage:m->data);glv_free(m->w8a8_workspace);glv_free(m->tensors);memset(m,0,sizeof(*m));
}
int glv_model_open(struct glv_model *m,const char *root,struct glv_memory *memory,bool resident)
{
  memset(m,0,sizeof(*m));m->memory=memory;char path[384];FILE *index=NULL;size_t size=0;int ret=-EBADMSG;
  if(snprintf(path,sizeof(path),"%s/model.bin",root)>=(int)sizeof(path))return -ENAMETOOLONG;
  if((ret=glv_open_asset(path,&index,&size)))return ret;
  unsigned char h[48];ret=-EBADMSG;
  if(size<48||glv_read_exact(index,h,48)||memcmp(h,"GLM1",4)||glv_u32(h+4)!=GLV_ABI)goto done;
  m->count=glv_u32(h+8);m->bytes=glv_u32(h+12);
  _Static_assert(sizeof(struct glv_tensor)==116,"Tensor directory ABI");
  if(!m->count||m->count>2048||m->bytes>256u*1024u*1024u||size!=48+m->count*116u)goto done;
  m->tensors=glv_alloc(memory,m->count*116u);if(!m->tensors){ret=-ENOMEM;goto done;}
  if(glv_read_exact(index,m->tensors,m->count*116u))goto done;
  for(unsigned i=0;i<m->count;i++){
    struct glv_tensor *t=&m->tensors[i];size_t elements=1;
    if(!memchr(t->name,0,sizeof(t->name))||t->ndim>4||t->type>2||t->offset%16||
       t->offset>m->bytes||t->bytes>m->bytes-t->offset)goto done;
    for(unsigned j=0;j<t->ndim;j++){
      if(!t->shape[j]||t->shape[j]>1048576||elements>SIZE_MAX/t->shape[j])goto done;
      elements*=t->shape[j];
    }
    if(elements>64u*1024u*1024u)goto done;
    if(t->type==0){if(t->bytes!=elements*4u||t->stride)goto done;}
    else {
      if(t->ndim<2)goto done;
      size_t row=elements/t->shape[0],stride=t->type==1?row/32*20:row+4;
      if((t->type==1&&row%32)||t->stride!=stride||t->bytes!=stride*t->shape[0])goto done;
    }
    if(i&&strcmp(m->tensors[i-1].name,t->name)>=0)goto done;
  }
  fclose(index);index=NULL;
  if(snprintf(path,sizeof(path),"%s/weights.bin",root)>=(int)sizeof(path)){ret=-ENAMETOOLONG;goto done;}
  ret=verified_take(m,path,h+16);if(ret<0)goto done;
  if(!ret){
    if((ret=glv_open_asset(path,&m->file,&size)))goto done;
    ret=-EBADMSG;if(size!=m->bytes)goto done;
    if(!resident){
      m->io=glv_alloc(memory,16384);if(!m->io){ret=-ENOMEM;goto done;}
      if(setvbuf(m->file,(char *)m->io,_IOFBF,16384)){ret=-EIO;goto done;}
    }
    unsigned char digest[32];ret=glv_sha_file(m->file,size,digest,memory);
    if(ret)goto done;
    if(memcmp(digest,h+16,32)){ret=-EBADMSG;goto done;}
    if(fseek(m->file,0,SEEK_SET)){ret=-EIO;goto done;}
  }
  size=m->bytes;
  if(resident){
    m->data_storage=glv_alloc(memory,size+15u);if(!m->data_storage){ret=-ENOMEM;goto done;}
    m->data=(unsigned char *)(((uintptr_t)m->data_storage+15u)&~(uintptr_t)15u);
    for(size_t at=0;at<size;){
      if((ret=glv_cancel(memory)))goto done;
      size_t n=size-at;if(n>16384)n=16384;
      if((ret=glv_read_exact(m->file,m->data+at,n)))goto done;at+=n;
    }
    unsigned char resident_digest[32];
    ret=glv_sha_buffer(m->data,size,resident_digest,memory);
    if(ret)goto done;
    if(memcmp(resident_digest,h+16,32)){ret=-EBADMSG;goto done;}
    fclose(m->file);m->file=NULL;
    /* The transferred stdio buffer is no longer needed once bytes reside. */
    glv_free(m->io);m->io=NULL;
    memory->profile.llm_resident_bytes=size;
    printf("LOCAL_RESIDENT bytes=%lu verified=1 file_closed=1\n",(unsigned long)size);
  }
  if(memory->tts_int8)memcpy(memory->tts_base_digest,h+16,32);
  ret=0;
done:
  if(index)fclose(index);if(ret)glv_model_close(m);return ret;
}
const struct glv_tensor *glv_tensor(struct glv_model *m,const char *name)
{
  unsigned lo=0,hi=m->count;
  while(lo<hi){unsigned mid=(lo+hi)/2;if(strcmp(m->tensors[mid].name,name)<0)lo=mid+1;else hi=mid;}
  return lo<m->count&&!strcmp(m->tensors[lo].name,name)?&m->tensors[lo]:NULL;
}
void glv_model_cache_clear(struct glv_model *m)
{
  while(m->cache){struct glv_row_cache *c=m->cache;m->cache=c->next;glv_free(c);}
  m->cache_last=NULL;m->cache_last_tensor=NULL;m->cache_bytes=0;
}
int glv_model_cache_tensor(struct glv_model *m,const char *name)
{
  const struct glv_tensor *t=glv_tensor(m,name);
  if(!t||!t->ndim||t->ndim>2)return -EINVAL;
  if(m->data)return 0;
  for(struct glv_row_cache *c=m->cache;c;c=c->next)if(c->tensor==t)return 0;
  size_t rows=t->ndim==2?t->shape[0]:1;
  if(!rows||!t->bytes||t->bytes%rows)return -EINVAL;
  size_t n=sizeof(struct glv_row_cache)+(size_t)t->bytes+(rows+7)/8;
  /* This optimization must not take the space needed by the next decoded row
   * or PIE temporary. glv_alloc separately enforces the 4 MiB PSRAM reserve. */
  size_t allowance=n+sizeof(union glv_header)+65536u;
  if(m->memory->used>m->memory->limit||allowance>m->memory->limit-m->memory->used)return -ENOMEM;
#ifdef __NuttX__
  struct mallinfo mi=mallinfo();
  if((size_t)mi.fordblks<allowance+4u*1024u*1024u)return -ENOMEM;
#endif
  struct glv_row_cache *c=glv_alloc(m->memory,n);if(!c)return -ENOMEM;
  c->tensor=t;c->row_bytes=t->bytes/rows;c->rows=rows;c->next=m->cache;m->cache=c;
  m->cache_bytes+=n+sizeof(union glv_header);
  if(m->cache_bytes>m->memory->profile.weight_cache_peak_bytes)
    m->memory->profile.weight_cache_peak_bytes=m->cache_bytes;
  m->cache_last_tensor=NULL;m->cache_last=NULL;return 0;
}
/* Synchronous bounded queue: packed block -> compute -> invalidate -> reuse.
 * Persistent complete-row cache wins first, so already cached rows are never
 * reread just to populate the transient block. Failure/cancellation never
 * publishes a partially read block and never retries an I/O operation. */
int glv_model_block_reserve(struct glv_model *m,size_t bytes)
{
 if(!m||!m->memory||m->data||!bytes||bytes>256u*1024u)return -EINVAL;
 if(m->block_data)return 0;
 m->block_data=glv_alloc(m->memory,bytes);if(!m->block_data)return -ENOMEM;
 m->block_capacity=bytes;m->block_bytes=0;m->block_tensor=NULL;
 if(bytes>m->memory->profile.llm_block_peak_bytes)m->memory->profile.llm_block_peak_bytes=bytes;
 return 0;
}
void glv_model_block_select(struct glv_model *m,const char *name)
{
 if(!m)return;
 m->block_bytes=0;m->block_tensor=m->block_data&&name?glv_tensor(m,name):NULL;
}
void glv_model_block_release(struct glv_model *m)
{
 if(!m)return;glv_free(m->block_data);m->block_data=NULL;
 m->block_capacity=m->block_offset=m->block_bytes=0;m->block_tensor=NULL;
}
static int block_read(struct glv_model *m,size_t offset,void *out,size_t bytes)
{
 const struct glv_tensor *t=m->block_tensor;
 if(!m->block_data||!t||!t->stride||offset<t->offset||offset-t->offset>=t->bytes||bytes>t->bytes-(offset-t->offset)||bytes>m->block_capacity)return 0;
 if(m->block_bytes&&offset>=m->block_offset&&offset-m->block_offset<=m->block_bytes&&bytes<=m->block_bytes-(offset-m->block_offset)){
  memcpy(out,m->block_data+offset-m->block_offset,bytes);m->memory->profile.llm_block_hits++;return 1;
 }
 /* Whole packed rows, starting at the requested row. The final tile can be
  * shorter; tensor bounds and the original row decoder still validate it. */
 size_t relative=(offset-t->offset)/t->stride*t->stride;
 size_t count=m->block_capacity/t->stride*t->stride;
 if(count>t->bytes-relative)count=t->bytes-relative;
 if(!count||offset-(t->offset+relative)>count||bytes>count-(offset-(t->offset+relative)))return 0;
 m->block_bytes=0;size_t start=t->offset+relative;
 if(m->cursor!=start){uint64_t began=glv_now_us();int ret=fseek(m->file,(long)start,SEEK_SET);m->memory->profile.weight_seek_us+=glv_now_us()-began;if(ret){m->cursor=SIZE_MAX;return -EIO;}}
 for(size_t at=0;at<count;){
  int ret=glv_cancel(m->memory);if(ret){m->cursor=SIZE_MAX;return ret;}
  /* At most 64 KiB between cancellation/media checks; each tile is read
   * into its final buffer rather than issuing one stdio call per row. */
  size_t n=count-at;if(n>65536u)n=65536u;
  uint64_t began=glv_now_us();m->memory->profile.weight_read_calls++;
  ret=glv_read_exact(m->file,m->block_data+at,n);
  m->memory->profile.weight_read_us+=glv_now_us()-began;
  if(ret){m->cursor=SIZE_MAX;return ret;}
  m->memory->profile.weight_bytes+=n;m->memory->profile.llm_block_read_bytes+=n;at+=n;
 }
 m->cursor=start+count;
 int ret=glv_cancel(m->memory);if(ret){m->cursor=SIZE_MAX;return ret;}
 m->block_offset=start;m->block_bytes=count;m->memory->profile.llm_block_loads++;
 memcpy(out,m->block_data+offset-start,bytes);return 1;
}
static struct glv_row_cache *row_cache(struct glv_model *m,const struct glv_tensor *t)
{
  if(m->cache_last_tensor==t)return m->cache_last;
  m->cache_last_tensor=t;m->cache_last=NULL;
  for(struct glv_row_cache *c=m->cache;c;c=c->next)if(c->tensor==t){m->cache_last=c;break;}
  return m->cache_last;
}
static int tensor_bytes(struct glv_model *m,struct glv_row_cache *cache,unsigned row,size_t offset,void *out,size_t bytes)
{
  int ret=glv_cancel(m->memory);if(ret)return ret;
  if(offset>m->bytes||bytes>m->bytes-offset)return -EBADMSG;
  if(m->data){memcpy(out,m->data+offset,bytes);return 0;}
  /* Only complete rows are cached. A hit must not move the underlying FILE
   * cursor or discard its read-ahead; cancellation/media checks still run. */
  bool eligible=cache&&row<cache->rows&&bytes==cache->row_bytes&&
    offset==cache->tensor->offset+row*cache->row_bytes;
  unsigned char *valid=eligible?cache->contents+cache->tensor->bytes:NULL;
  if(eligible&&(valid[row/8]&(1u<<(row%8)))){
    memcpy(out,cache->contents+row*bytes,bytes);
    m->memory->profile.weight_cache_hits++;m->memory->profile.weight_cache_hit_bytes+=bytes;return 0;
  }
  int staged=block_read(m,offset,out,bytes);
  if(staged<0)return staged;
  if(staged){
    if(eligible){memcpy(cache->contents+row*bytes,out,bytes);valid[row/8]|=1u<<(row%8);}
    return 0;
  }
  if(m->cursor!=offset){uint64_t began=glv_now_us();int seek=fseek(m->file,(long)offset,SEEK_SET);
    m->memory->profile.weight_seek_us+=glv_now_us()-began;if(seek){m->cursor=SIZE_MAX;return -EIO;}}
  uint64_t began=glv_now_us();m->memory->profile.weight_read_calls++;ret=glv_read_exact(m->file,out,bytes);
  m->memory->profile.weight_read_us+=glv_now_us()-began;
  if(!ret){
    m->cursor=offset+bytes;m->memory->profile.weight_bytes+=bytes;
    if(eligible){memcpy(cache->contents+row*bytes,out,bytes);valid[row/8]|=1u<<(row%8);}
  }else m->cursor=SIZE_MAX;return ret;
}
bool glv_model_q8_planar(const struct glv_model *m,const struct glv_tensor *t)
{
 if(!m||!m->data||!t||!m->tensors)return false;
 uintptr_t offset=(uintptr_t)t-(uintptr_t)m->tensors;
 if(offset%sizeof(*t)||offset/sizeof(*t)>=m->count||m->count>2048)return false;
 unsigned i=(unsigned)(offset/sizeof(*t));return (m->w8a8_planar[i/8]>>(i%8))&1u;
}
int glv_tensor_row(struct glv_model *m,const struct glv_tensor *t,unsigned row,float *out,unsigned count)
{
  if(!t)return -ENOENT;
  size_t elements=1;for(unsigned i=0;i<t->ndim;i++)elements*=t->shape[i];
  unsigned rows=t->ndim>=2?t->shape[0]:1;
  if(row>=rows||count!=elements/rows)return -EINVAL;
  struct glv_row_cache *cache=row_cache(m,t);
  uint64_t began=glv_now_us(),io=m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us;
  int ret;
  if(!t->type)ret=tensor_bytes(m,cache,row,t->offset+row*count*4u,out,count*4u);
  else if(t->type==1){
    unsigned char chunk[640];ret=0;
    for(unsigned i=0;i<count&&!ret;){
      unsigned groups=(count-i)/32;if(groups>32)groups=32;
      ret=tensor_bytes(m,cache,row,t->offset+row*t->stride+i/32*20,chunk,groups*20);
      if(ret)break;
      for(unsigned g=0;g<groups;g++){
        const unsigned char *block=chunk+g*20;float scale;memcpy(&scale,block,4);
        if(!glv_finite(scale)||scale<=0)return -EBADMSG;
        for(unsigned j=0;j<32;j++){int v=(block[4+j/2]>>((j%2)*4))&15;if(v>=8)v-=16;out[i+g*32+j]=scale*v;}
      }i+=groups*32;
    }
  }else if(glv_model_q8_planar(m,t)){
    if((ret=glv_cancel(m->memory)))return ret;
    if(t->offset>m->bytes||t->bytes>m->bytes-t->offset||t->bytes!=(size_t)rows*(count+4u))return -EBADMSG;
    const unsigned char *base=m->data+t->offset;
    float scale=*(const float *)(base+(size_t)rows*count+row*4u);
    if(!glv_finite(scale)||scale<=0)return -EBADMSG;
    const int8_t *codes=(const int8_t *)base+(size_t)row*count;
    for(unsigned i=0;i<count;i++)out[i]=scale*codes[i];
    m->memory->profile.weight_q8_bulk_rows++;
  }else if(m->bulk_q8&&count>=2&&count<=4092){
    /* The packed row fits in the caller's float output. Read into its tail,
     * save the scale, then expand forward. Quant byte i is at 3*count+i;
     * storing float i ends at 4*i+3, below the next unread byte for i<count.
     * No new allocation, stack array, arithmetic order or model format. */
    unsigned char *raw=(unsigned char *)out+(size_t)count*3u-4u;
    ret=tensor_bytes(m,cache,row,t->offset+(size_t)row*t->stride,raw,(size_t)count+4u);
    if(ret)return ret;float scale;memcpy(&scale,raw,4);
    if(!glv_finite(scale)||scale<=0)return -EBADMSG;
    m->memory->profile.weight_q8_bulk_rows++;
    for(unsigned i=0;i<count;i++){int8_t value=(int8_t)raw[4u+i];out[i]=scale*value;}
  }else{
    m->memory->profile.weight_q8_legacy_rows++;
    unsigned char buffer[256];float scale;ret=tensor_bytes(m,cache,row,t->offset+row*t->stride,&scale,4);
    if(ret)return ret;if(!glv_finite(scale)||scale<=0)return -EBADMSG;
    for(unsigned at=0;at<count&&!ret;){
      unsigned n=count-at;if(n>sizeof(buffer))n=sizeof(buffer);
      ret=tensor_bytes(m,cache,row,t->offset+row*t->stride+4+at,buffer,n);
      for(unsigned i=0;i<n&&!ret;i++)out[at+i]=scale*(int8_t)buffer[i];at+=n;
    }
  }
  if(ret)return ret;
  for(unsigned i=0;i<count;i++)if(!glv_finite(out[i]))return -EBADMSG;
  m->memory->profile.weight_decode_us+=glv_now_us()-began-(m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us-io);
  return 0;
}
int glv_tensor_q4_packed(struct glv_model *m,const struct glv_tensor *t,unsigned row,
                         void *packed,size_t capacity)
{
  if(!m||!m->memory||!t||!packed||(!m->data&&!m->file))return -EINVAL;
  if(t->type!=1)return -ENOTSUP;
  if(t->ndim!=2||!t->shape[1]||t->shape[1]%32||row>=t->shape[0]||
     t->stride!=t->shape[1]/32*20||capacity<t->stride)return -EINVAL;
  if(row&&t->stride>(SIZE_MAX-t->offset)/row)return -EBADMSG;
  return tensor_bytes(m,row_cache(m,t),row,t->offset+(size_t)row*t->stride,packed,t->stride);
}
int glv_tensor_q8_packed(struct glv_model *m,const struct glv_tensor *t,unsigned row,
                         void *packed,size_t capacity,unsigned count)
{
  if(!m||!m->memory||!t||!packed||!t->ndim||t->ndim>4)return -EINVAL;
  if(t->type!=2)return -ENOTSUP;
  size_t elements=1;
  for(unsigned i=0;i<t->ndim;i++){
    if(!t->shape[i]||elements>SIZE_MAX/t->shape[i])return -EINVAL;
    elements*=t->shape[i];
  }
  unsigned rows=t->ndim>=2?t->shape[0]:1;
  if(row>=rows||count!=elements/rows||count>SIZE_MAX-4u||capacity<(size_t)count+4u||t->stride<(size_t)count+4u)return -EINVAL;
  if(row&&t->stride>(SIZE_MAX-t->offset)/row)return -EBADMSG;
  size_t offset=t->offset+(size_t)row*t->stride;unsigned char *raw=packed;
  struct glv_row_cache *cache=row_cache(m,t);int ret;
  uint64_t began=glv_now_us(),io=m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us;
  if(glv_model_q8_planar(m,t)){
    if((ret=glv_cancel(m->memory)))return ret;
    if(t->offset>m->bytes||t->bytes>m->bytes-t->offset||t->bytes!=(size_t)rows*(count+4u))return -EBADMSG;
    const unsigned char *base=m->data+t->offset;
    memcpy(raw,base+(size_t)rows*count+row*4u,4);
    memcpy(raw+4,base+(size_t)row*count,count);
    float scale;memcpy(&scale,raw,4);if(!glv_finite(scale)||scale<=0)return -EBADMSG;
    m->memory->profile.weight_q8_bulk_rows++;
  }else if(m->bulk_q8&&count>=2&&count<=4092){
    ret=tensor_bytes(m,cache,row,offset,raw,(size_t)count+4u);if(ret)return ret;
    float scale;memcpy(&scale,raw,4);if(!glv_finite(scale)||scale<=0)return -EBADMSG;
    m->memory->profile.weight_q8_bulk_rows++;
  }else{
    m->memory->profile.weight_q8_legacy_rows++;
    ret=tensor_bytes(m,cache,row,offset,raw,4);if(ret)return ret;
    float scale;memcpy(&scale,raw,4);if(!glv_finite(scale)||scale<=0)return -EBADMSG;
    for(unsigned at=0;at<count;){
      unsigned n=count-at;if(n>256)n=256;
      ret=tensor_bytes(m,cache,row,offset+4u+at,raw+4u+at,n);if(ret)return ret;at+=n;
    }
  }
  m->memory->profile.weight_decode_us+=glv_now_us()-began-(m->memory->profile.weight_read_us+m->memory->profile.weight_seek_us-io);
  return 0;
}
int glv_vector(struct glv_model *m,const char *name,float *out,unsigned n)
{return glv_tensor_row(m,glv_tensor(m,name),0,out,n);}
int glv_matvec(struct glv_model *m,const char *name,const float *x,float *out,unsigned rows,unsigned cols)
{
  const struct glv_tensor *t=glv_tensor(m,name);
#ifdef GLV_Q4_PIE
  if(t&&t->type==2&&m->memory->q4_bits==16)return glv_q8_matbatch(m,name,x,out,rows,cols,1);
  if(t&&t->type==1&&m->memory->q4_bits==16)return glv_q4_matbatch(m,name,x,out,rows,cols,1,16);
#endif
  if(!t||t->ndim!=2||t->shape[0]!=rows||t->shape[1]!=cols)return -EBADMSG;
  float *w=glv_alloc(m->memory,cols*sizeof(float));if(!w)return -ENOMEM;int ret=0;
  for(unsigned r=0;r<rows&&!ret;r++){
    ret=glv_tensor_row(m,t,r,w,cols);float sum=0;
    uint64_t began=glv_now_us();
    for(unsigned c=0;c<cols&&!ret;c++)sum+=w[c]*x[c];out[r]=sum;
    m->memory->profile.matmul_us+=glv_now_us()-began;
    if(!glv_finite(sum))ret=-ERANGE;
  }
  glv_free(w);return ret;
}
/* Decode each SD row once for several prompt positions. Every individual dot
 * product keeps the original ascending-column FP32 accumulation order. */
int glv_matbatch(struct glv_model *m,const char *name,const float *x,float *out,unsigned rows,unsigned cols,unsigned batch)
{
  const struct glv_tensor *t=glv_tensor(m,name);
#ifdef GLV_Q4_PIE
  if(t&&t->type==2&&m->memory->q4_bits==16)return glv_q8_matbatch(m,name,x,out,rows,cols,batch);
  if(t&&t->type==1&&m->memory->q4_bits==16)return glv_q4_matbatch(m,name,x,out,rows,cols,batch,16);
#endif
  if(!batch||batch>GLV_MATBATCH_MAX||!t||t->ndim!=2||t->shape[0]!=rows||t->shape[1]!=cols)return -EINVAL;
  float *w=glv_alloc(m->memory,cols*sizeof(float));if(!w)return -ENOMEM;int ret=0;
  for(unsigned r=0;r<rows&&!ret;r++){
    ret=glv_tensor_row(m,t,r,w,cols);if(ret)break;
    uint64_t began=glv_now_us();
    unsigned b=0;
    /* Four independent positions share each coefficient load. Each sum
     * still visits columns in ascending order, without a tree reduction. */
    for(;b+3<batch;b+=4){
      const float *x0=x+b*cols,*x1=x0+cols,*x2=x1+cols,*x3=x2+cols;
      float sum0=0,sum1=0,sum2=0,sum3=0;
      for(unsigned c=0;c<cols;c++){
        float coefficient=w[c];
        sum0+=coefficient*x0[c];sum1+=coefficient*x1[c];
        sum2+=coefficient*x2[c];sum3+=coefficient*x3[c];
      }
      out[b*rows+r]=sum0;if(!glv_finite(sum0)){ret=-ERANGE;break;}
      out[(b+1)*rows+r]=sum1;if(!glv_finite(sum1)){ret=-ERANGE;break;}
      out[(b+2)*rows+r]=sum2;if(!glv_finite(sum2)){ret=-ERANGE;break;}
      out[(b+3)*rows+r]=sum3;if(!glv_finite(sum3)){ret=-ERANGE;break;}
    }
    for(;b<batch&&!ret;b++){
      float sum=0;for(unsigned c=0;c<cols;c++)sum+=w[c]*x[b*cols+c];
      out[b*rows+r]=sum;if(!glv_finite(sum)){ret=-ERANGE;break;}
    }
    m->memory->profile.matmul_us+=glv_now_us()-began;
  }
  glv_free(w);return ret;
}
int glv_utf8_read(const unsigned char **cursor,const unsigned char *end,uint32_t *out)
{
  const unsigned char *p=*cursor;if(p>=end)return -EINVAL;
  uint32_t c=*p++;unsigned extra=0;uint32_t minimum=0;
  if(c>=0xc2&&c<=0xdf){c&=31;extra=1;minimum=0x80;}
  else if(c>=0xe0&&c<=0xef){c&=15;extra=2;minimum=0x800;}
  else if(c>=0xf0&&c<=0xf4){c&=7;extra=3;minimum=0x10000;}
  else if(c>=0x80)return -EILSEQ;
  if((size_t)(end-p)<extra)return -EILSEQ;
  while(extra--){if((*p&0xc0)!=0x80)return -EILSEQ;c=(c<<6)|(*p++&63);}
  if(c<minimum||c>0x10ffff||(c>=0xd800&&c<=0xdfff))return -EILSEQ;
  *cursor=p;*out=c;return 0;
}
unsigned glv_utf8_write(uint32_t c,unsigned char *p)
{
  if(c<0x80){p[0]=c;return 1;}
  if(c<0x800){p[0]=0xc0|(c>>6);p[1]=0x80|(c&63);return 2;}
  if(c<0x10000){p[0]=0xe0|(c>>12);p[1]=0x80|((c>>6)&63);p[2]=0x80|(c&63);return 3;}
  p[0]=0xf0|(c>>18);p[1]=0x80|((c>>12)&63);p[2]=0x80|((c>>6)&63);p[3]=0x80|(c&63);return 4;
}
