/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr_graph.h"
#ifdef GLV_ASR_ACCEL_ENABLED
#include "local_asr_accel.h"
#endif
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* GLA1 is deliberately fixed width/little endian. Do not fread C structures.
 * Exporter owns ONNX specialization; this reader distrusts all counts,
 * offsets, producer order, shapes and indices. ABI2 adds published INT8 ops. */
#define HEADER_BYTES 80u
#define TENSOR_BYTES 72u
#define NODE_BYTES 192u
#define MAX_TENSORS 8192u
#define MAX_NODES 4096u
#define MAX_IO 64u
#define CONST 1u
#define INPUT 2u
#define OUTPUT 4u
#define F32 1u
#define U8 2u
#define I8 3u
#define I32 6u
#define I64 7u
#define BOOL 9u
enum {COPY=1,TRANSPOSE,GATHER,GATHER_ELEMENTS,SLICE,CONCAT,SPLIT,EXPAND,CAST,
      CUMSUM,ADD,SUB,MUL,DIV,POW,NEG,RELU,SIGMOID,TANH,RECIPROCAL,SOFTMAX,
      REDUCE_MEAN,REDUCE_SUM,MATMUL,CONV,GEMM,DYNAMIC_QUANTIZE,MATMUL_INTEGER,DEQUANTIZE};
struct tensor {
  union {const struct glv_asr_shape *shape;size_t shape_id;};
  uint32_t type,flags,offset,stored,format,rows,columns,padded;
  size_t count,bytes;
  void *data;
  unsigned uses,left;
  bool borrowed;
};
/* GLA1 repeats the same zero/axis/shape parameter tuples on most nodes.
 * Intern complete bit patterns within this graph; this changes storage only,
 * never the serialized plan, model values or arithmetic. */
struct parameters {int32_t p[16];float a,b;};
struct node {uint16_t op,ni,no,in[8],out[4],parameters;};
_Static_assert(sizeof(struct node)==32,"Compact ASR node metadata");
struct glv_asr_graph {
  struct glv_memory *memory;
  FILE *data;
  struct tensor *t;
  struct node *n;
  struct parameters *parameters;unsigned parameter_count,parameter_capacity;
  struct glv_asr_shape *shapes;unsigned shape_count,shape_capacity;
  unsigned nt,nn,ni,no,last;
  uint32_t inputs[MAX_IO],outputs[MAX_IO];
  unsigned char *row;
  unsigned char *io,*cache,*hot;
  size_t hot_bytes;
  unsigned char expected_digest[32];
#ifdef GLV_ASR_ACCEL_ENABLED
  unsigned char *block;
  size_t block_capacity,block_offset,block_bytes,block_limit,block_headroom;
  const struct tensor *block_tensor;
  bool block_tried;
#endif
  size_t row_bytes,cursor,data_bytes;
  struct glv_asr_io_stats io_stats;
};
static bool mul(size_t a,size_t b,size_t *r)
{if(b&&a>SIZE_MAX/b)return false;*r=a*b;return true;}
static size_t width(unsigned type)
{return type==F32||type==I32?4:type==I64?8:type==BOOL||type==U8||type==I8?1:0;}
/* Shapes are immutable too. During loading the tensor union holds an index;
 * pointers are assigned only after the pool stops growing. */
static int intern_shape(struct glv_asr_graph *g,const struct glv_asr_shape *value,size_t *index)
{
 for(unsigned i=0;i<g->shape_count;i++)
  if(!memcmp(g->shapes+i,value,sizeof(*value))){*index=i;return 0;}
 if(g->shape_count==g->shape_capacity){
  if(g->shape_capacity>=MAX_TENSORS)return -E2BIG;
  unsigned capacity=g->shape_capacity?g->shape_capacity*2u:16u;
  if(capacity>MAX_TENSORS)capacity=MAX_TENSORS;
  struct glv_asr_shape *next=glv_alloc(g->memory,(size_t)capacity*sizeof(*next));
  if(!next)return -ENOMEM;
  if(g->shape_count)memcpy(next,g->shapes,(size_t)g->shape_count*sizeof(*next));
  glv_free(g->shapes);g->shapes=next;g->shape_capacity=capacity;
 }
 *index=g->shape_count;g->shapes[g->shape_count++]=*value;return 0;
}
static int intern_parameters(struct glv_asr_graph *g,const unsigned char *raw,uint16_t *index)
{
 struct parameters value;
 for(unsigned i=0;i<16;i++)value.p[i]=(int32_t)glv_u32(raw+60+i*4);
 /* memcpy retains NaN/signed-zero bit patterns too; validation stays at use. */
 uint32_t a=glv_u32(raw+180),b=glv_u32(raw+184);
 memcpy(&value.a,&a,4);memcpy(&value.b,&b,4);
 for(unsigned i=0;i<g->parameter_count;i++)
  if(!memcmp(g->parameters+i,&value,sizeof(value))){*index=(uint16_t)i;return 0;}
 if(g->parameter_count==g->parameter_capacity){
  if(g->parameter_capacity>=MAX_NODES)return -E2BIG;
  unsigned capacity=g->parameter_capacity?g->parameter_capacity*2u:16u;
  if(capacity>MAX_NODES)capacity=MAX_NODES;
  struct parameters *next=glv_alloc(g->memory,(size_t)capacity*sizeof(*next));
  if(!next)return -ENOMEM;
  if(g->parameter_count)memcpy(next,g->parameters,(size_t)g->parameter_count*sizeof(*next));
  glv_free(g->parameters);g->parameters=next;g->parameter_capacity=capacity;
 }
 *index=(uint16_t)g->parameter_count;g->parameters[g->parameter_count++]=value;return 0;
}
static int exact(FILE *f,void *data,size_t n)
{return glv_read_exact(f,data,n);}
static uint32_t u32(const unsigned char *p){return glv_u32(p);}
static float f32(const unsigned char *p)
{uint32_t v=u32(p);float f;memcpy(&f,&v,4);return f;}
static bool same(const struct tensor *a,const struct tensor *b)
{return a->type==b->type&&a->shape->rank==b->shape->rank&&
 !memcmp(a->shape->dims,b->shape->dims,a->shape->rank*sizeof(uint32_t));}
static bool aliases(const void *a,size_t na,const void *b,size_t nb)
{
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 if(na>UINTPTR_MAX-x||nb>UINTPTR_MAX-y)return true;
 return na&&nb&&x<y+nb&&y<x+na;
}
#ifdef GLV_ASR_ACCEL_ENABLED
static void select_block(struct glv_asr_graph *g,struct tensor *t)
{
 if(g->block_tensor!=t){g->block_tensor=t;g->block_bytes=0;}
 if(!g->block_limit||g->cache||g->block||g->block_tried||g->memory->asr_reference||g->memory->asr_no_blocks)return;
 g->block_tried=true;
 /* Optional, leave activation/scratch headroom. Allocator also enforces
  * the real system reserve. Buffer is private to the owning graph. */
 for(size_t n=g->block_limit;n>=32u*1024u;n/=2){
  if(g->memory->used>g->memory->limit||glv_alloc_charge(n)+g->block_headroom>g->memory->limit-g->memory->used)continue;
  g->block=glv_alloc(g->memory,n);if(g->block){g->block_capacity=n;g->memory->profile.asr_block_peak_bytes+=n;break;}
 }
}
#endif
int glv_asr_graph_block_limit(struct glv_asr_graph *g,size_t limit)
{
 if(!g||limit>1024u*1024u||(limit&&limit<32u*1024u))return -EINVAL;
#ifdef GLV_ASR_ACCEL_ENABLED
 /* Single owner, before inference. Optional buffer can later be evicted
  * by mandatory graph allocations; decoder deliberately has no block. */
 glv_free(g->block);g->block=NULL;g->block_capacity=g->block_bytes=0;
 g->block_tensor=NULL;
 /* A tensor is streamed sequentially, and only one is selected at a time.
  * Never reserve a1MiB block when this verified graph's largest constant is
  * smaller; a complete largest tensor still fits without extra I/O. */
 size_t largest=0;
 for(unsigned i=0;i<g->nt;i++)if((g->t[i].flags&CONST)&&g->t[i].stored>largest)largest=g->t[i].stored;
 if(limit>largest)limit=largest;
 g->block_limit=limit>=32u*1024u?limit:0;
 g->block_headroom=1024u*1024u;g->block_tried=false;
#else
 (void)limit;
#endif
 return 0;
}
static int data_read(struct glv_asr_graph *g,void *p,size_t n)
{uint64_t began=glv_now_us();int ret=exact(g->data,p,n);g->memory->profile.asr_io_us+=glv_now_us()-began;return ret;}
static int data_seek(struct glv_asr_graph *g,uint32_t off)
{uint64_t began=glv_now_us();int ret=fseek(g->data,(long)off,SEEK_SET);g->memory->profile.asr_io_us+=glv_now_us()-began;return ret;}
static int seek_read(struct glv_asr_graph *g,uint32_t off,void *p,size_t bytes)
{
 int r=glv_cancel(g->memory);if(r)return r;
 if(off>g->data_bytes||bytes>g->data_bytes-off)return -EBADMSG;
 if(g->cache){memcpy(p,g->cache+off,bytes);return 0;}
 if(g->hot&&off<=g->hot_bytes&&bytes<=g->hot_bytes-off){memcpy(p,g->hot+off,bytes);g->memory->profile.asr_hot_hits++;return 0;}
 if(off>INT32_MAX)return -EIO;
#ifdef GLV_ASR_ACCEL_ENABLED
 const struct tensor *t=g->block_tensor;
 if(g->block&&t&&off>=t->offset&&off-t->offset<t->stored&&bytes<=t->stored-(off-t->offset)&&bytes<=g->block_capacity){
  if(g->block_bytes&&off>=g->block_offset&&off-g->block_offset<=g->block_bytes&&bytes<=g->block_bytes-(off-g->block_offset)){
   memcpy(p,g->block+off-g->block_offset,bytes);g->memory->profile.asr_block_hits++;return 0;
  }
  size_t count=t->stored-(off-t->offset);if(count>g->block_capacity)count=g->block_capacity;g->block_bytes=0;
  if(g->cursor!=off){if(data_seek(g,off)){g->cursor=SIZE_MAX;return -EIO;}g->io_stats.seeks++;}
  for(size_t at=0;at<count;){
   if((r=glv_cancel(g->memory))){g->cursor=SIZE_MAX;return r;}
   size_t n=count-at;if(n>65536u)n=65536u;g->io_stats.reads++;r=data_read(g,g->block+at,n);
   if(r){g->cursor=SIZE_MAX;return r;}g->io_stats.bytes+=n;g->memory->profile.asr_block_bytes+=n;at+=n;
  }
  g->cursor=off+count;if((r=glv_cancel(g->memory))){g->cursor=SIZE_MAX;return r;}
  g->block_offset=off;g->block_bytes=count;g->memory->profile.asr_block_loads++;memcpy(p,g->block,bytes);return 0;
 }
#endif
 /* NuttX fseeko flushes the read buffer even when the next row is exactly
  * contiguous. Keep the FILE cursor and its owned read-ahead buffer intact
  * on sequential reads; never change tensor order or arithmetic. */
 if(g->cursor!=off){
   if(data_seek(g,off))return -EIO;
   g->io_stats.seeks++;g->cursor=off;
 }
 g->io_stats.reads++;r=data_read(g,p,bytes);
 if(r)g->cursor=SIZE_MAX;else{g->cursor=off+bytes;g->io_stats.bytes+=bytes;}
 return r;
}
/* Decode one stored output row. Format 1/2 is group32 symmetric Q4/Q8,
 * scales FP32 then packed values within each row (same as export tool). */
static int read_row_inner(struct glv_asr_graph *g,struct tensor *t,unsigned row,float *dest,unsigned count)
{
 if(t->type!=F32||row>=t->rows||count!=t->columns)return -EINVAL;
 unsigned format=t->format&255u;
 if(!format){
   if(t->data){memcpy(dest,(float *)t->data+(size_t)row*count,count*4u);return 0;}
   return seek_read(g,t->offset+(uint32_t)((size_t)row*count*4),dest,(size_t)count*4);
 }
 size_t groups=t->padded/32,groupbytes=format==1?20u:36u,rowbytes=groups*groupbytes;
 if(rowbytes>g->row_bytes)return -EINVAL;
 int ret=seek_read(g,t->offset+(uint32_t)(row*rowbytes),g->row,rowbytes);if(ret)return ret;
 /* Existing exporter interleaves one FP32 scale with 16/32 value bytes. */
 for(unsigned i=0;i<count;i++){
   const unsigned char *block=g->row+(i/32)*groupbytes;
   float scale=f32(block);int value;unsigned at=i%32;
   const unsigned char *q=block+4;
   if(format==1){unsigned nibble=(q[at/2]>>(4u*(at&1u)))&15u;value=nibble<8?(int)nibble:(int)nibble-16;}
   else value=q[at]<128?q[at]:(int)q[at]-256;
   if(!glv_finite(scale)||scale<0)return -EBADMSG;
   dest[i]=(float)value*scale;
 }
 return glv_cancel(g->memory);
}
static int read_row(struct glv_asr_graph *g,struct tensor *t,unsigned row,float *dest,unsigned count)
{uint64_t began=glv_now_us();int ret=read_row_inner(g,t,row,dest,count);g->memory->profile.asr_rows_us+=glv_now_us()-began;return ret;}
struct row_context {struct glv_asr_graph *g;struct tensor *t;size_t batch;bool transpose;};
static int rows(void *opaque,unsigned row,float *dest,unsigned count)
{
 struct row_context *r=opaque;struct tensor *t=r->t;
 if((t->flags&CONST)&&(t->format||!t->data)){
#ifdef GLV_ASR_ACCEL_ENABLED
  select_block(r->g,t);
#endif
  return read_row(r->g,t,row,dest,count);
 }
 if(t->type!=F32||!t->data)return -EINVAL;
 const float *src=(const float *)t->data+r->batch;
 if(r->transpose){unsigned n=t->shape->dims[t->shape->rank-1];
   if(row>=n||count!=t->shape->dims[t->shape->rank-2])return -EINVAL;
   for(unsigned k=0;k<count;k++)dest[k]=src[(size_t)k*n+row];
 }else {
   if(!t->shape->rank||row>=t->shape->dims[0]||count!=t->count/t->shape->dims[0]||r->batch)return -EINVAL;
   memcpy(dest,src+(size_t)row*count,(size_t)count*4);
 }
 return glv_cancel(r->g->memory);
}
#ifdef GLV_ASR_ACCEL_ENABLED
static int prepared_rows_inner(void *opaque,unsigned row,float *dest,int16_t *codes,float *scales,float *l1,unsigned count)
{
 struct row_context *r=opaque;struct tensor *t=r->t;struct glv_asr_graph *g=r->g;unsigned fmt=t->format&255u;
 if(!(t->flags&CONST)||!fmt||r->batch){int ret=rows(opaque,row,dest,count);return ret?ret:0;}
 if(t->type!=F32||row>=t->rows||count!=t->columns||t->padded!=((count+31u)&~31u))return -EINVAL;
 size_t groups=t->padded/32,groupbytes=fmt==1?20u:36u,rowbytes=groups*groupbytes;
 if(rowbytes>g->row_bytes)return -EINVAL;select_block(g,t);
 int ret=seek_read(g,t->offset+(uint32_t)(row*rowbytes),g->row,rowbytes);if(ret)return ret;
 for(unsigned group=0;group<groups;group++){
  const unsigned char *block=g->row+(size_t)group*groupbytes;float scale=f32(block);if(!glv_finite(scale)||scale<0)return -EBADMSG;scales[group]=scale;l1[group]=0;
  for(unsigned i=0;i<32;i++){
   unsigned col=group*32+i;int v=0;
   if(col<count){if(fmt==1){unsigned nibble=(block[4+i/2]>>(4u*(i&1u)))&15u;v=nibble<8?(int)nibble:(int)nibble-16;}else v=(int8_t)block[4+i];}
   codes[col]=(int16_t)v;if(col<count){dest[col]=(float)v*scale;l1[group]+=fabsf(dest[col]);}
  }
 }
 ret=glv_cancel(g->memory);return ret?ret:1;
}
#endif
#ifdef GLV_ASR_ACCEL_ENABLED
static int prepared_rows(void *opaque,unsigned row,float *dest,int16_t *codes,float *scales,float *l1,unsigned count)
{struct row_context *r=opaque;uint64_t began=glv_now_us();int ret=prepared_rows_inner(opaque,row,dest,codes,scales,l1,count);r->g->memory->profile.asr_rows_us+=glv_now_us()-began;return ret;}
#endif
static void release(struct tensor *t)
{if(!t->borrowed)glv_free(t->data);t->data=NULL;t->borrowed=false;}
static void *graph_alloc(struct glv_asr_graph *g,size_t bytes)
{
 void *p=glv_alloc(g->memory,bytes);
#ifdef GLV_ASR_ACCEL_ENABLED
 if(!p&&g->block){glv_free(g->block);g->block=NULL;g->block_bytes=g->block_capacity=0;g->block_tried=true;p=glv_alloc(g->memory,bytes);}
#endif
 if(!p&&g->hot){glv_free(g->hot);g->hot=NULL;g->hot_bytes=0;g->memory->profile.asr_hot_evictions++;p=glv_alloc(g->memory,bytes);}
 return p;
}
static int materialize(struct glv_asr_graph *g,struct tensor *t)
{
 if(t->data)return 0;
 if(!(t->flags&CONST))return -EINVAL;
 if(g->cache&&!t->format&&!(t->offset%width(t->type))){t->data=g->cache+t->offset;t->borrowed=true;return 0;}
 t->data=graph_alloc(g,t->bytes?t->bytes:1);if(!t->data)return -ENOMEM;
 int ret=0;
 if(!t->format)ret=seek_read(g,t->offset,t->data,t->bytes);
 else {
   /* Matrix-RHS transposition is format bit 8; reconstruct only for a
    * non-matrix consumer. Standard MatMul/Conv/Gather never needs this. */
   unsigned fmt=t->format;t->format&=255u;
   float *scratch=graph_alloc(g,(size_t)t->columns*4);
   if(!scratch)ret=-ENOMEM;
   for(unsigned row=0;row<t->rows&&!ret;row++){
     ret=read_row(g,t,row,scratch,t->columns);if(ret)break;
     if(fmt&256u)for(unsigned c=0;c<t->columns;c++)((float *)t->data)[(size_t)c*t->rows+row]=scratch[c];
     else memcpy((float *)t->data+(size_t)row*t->columns,scratch,(size_t)t->columns*4);
   }glv_free(scratch);t->format=fmt;
 }
 if(ret)release(t);return ret;
}
static int make_output(struct glv_asr_graph *g,struct tensor *t)
{
 if(t->data)return t->borrowed?0:-EINVAL;
 t->data=graph_alloc(g,t->bytes?t->bytes:1);
 return t->data?0:-ENOMEM;
}
static int axis_value(int a,unsigned rank,unsigned *out)
{if(a<0)a+=(int)rank;if(a<0||(unsigned)a>=rank)return -EINVAL;*out=(unsigned)a;return 0;}
static size_t stride(const struct tensor *t,unsigned axis)
{size_t r=1;for(unsigned i=axis+1;i<t->shape->rank;i++)r*=t->shape->dims[i];return r;}
static int checked_index(int64_t index,uint32_t dim,size_t *out)
{if(index<0)index+=dim;if(index<0||(uint64_t)index>=dim)return -ERANGE;*out=(size_t)index;return 0;}
static int move(struct glv_asr_graph *g,struct node *n)
{
 const struct parameters *parameters=&g->parameters[n->parameters];
 struct tensor *x=&g->t[n->in[0]],*y=&g->t[n->out[0]];
 size_t size=width(x->type);int ret;
 if(n->op!=CAST&&x->type!=y->type)return -EINVAL;
 if(n->op==COPY){if(x->bytes!=y->bytes)return -EINVAL;if(y->data!=x->data)memcpy(y->data,x->data,y->bytes);return glv_cancel(g->memory);}
 if(n->op==CONCAT||n->op==SPLIT){
   unsigned axis;if((ret=axis_value(parameters->p[0],x->shape->rank,&axis)))return ret;
   if(y->shape->rank!=x->shape->rank)return -EINVAL;
   if(n->op==CONCAT)for(unsigned d=0;d<x->shape->rank;d++)if(d!=axis&&y->shape->dims[d]!=x->shape->dims[d])return -EINVAL;
   size_t inner=stride(x,axis),outer=x->count/(inner*x->shape->dims[axis]),sum=0;
   unsigned total=n->op==CONCAT?n->ni:n->no;
   for(unsigned i=0;i<total;i++){
     struct tensor *v=&g->t[n->op==CONCAT?n->in[i]:n->out[i]];
     if(v->type!=x->type||v->shape->rank!=x->shape->rank)return -EINVAL;
     for(unsigned d=0;d<x->shape->rank;d++)if(d!=axis&&v->shape->dims[d]!=x->shape->dims[d])return -EINVAL;
     sum+=v->shape->dims[axis];
   }
   if(sum!=(n->op==CONCAT?y:x)->shape->dims[axis])return -EINVAL;
   size_t at=0;
   for(unsigned i=0;i<total;i++){
     struct tensor *v=&g->t[n->op==CONCAT?n->in[i]:n->out[i]];
     size_t block=(size_t)v->shape->dims[axis]*inner;
     for(size_t j=0;j<outer;j++){
       if((ret=glv_cancel(g->memory)))return ret;
       if(n->op==CONCAT)memcpy((char *)y->data+(j*sum*inner+at)*size,(char *)v->data+j*block*size,block*size);
       else memcpy((char *)v->data+j*block*size,(char *)x->data+(j*sum*inner+at)*size,block*size);
     }at+=block;
   }return 0;
 }
 if(n->op==GATHER||n->op==GATHER_ELEMENTS){
   struct tensor *indices=&g->t[n->in[1]];unsigned axis;
   if(indices->type!=I64||(ret=axis_value(parameters->p[0],x->shape->rank,&axis)))return -EINVAL;
   size_t inner=stride(x,axis),dim=x->shape->dims[axis];
   if(n->op==GATHER){
     size_t outer=x->count/(inner*dim);
     size_t count;if(!mul(outer,indices->count,&count)||!mul(count,inner,&count))return -EOVERFLOW;
     if(y->count!=count)return -EINVAL;
     if(!x->data){
       if(axis||inner!=x->columns||x->shape->dims[0]!=x->rows)return -EINVAL;
       for(size_t i=0;i<indices->count;i++){
         size_t index;if((ret=checked_index(((int64_t *)indices->data)[i],(uint32_t)dim,&index)))return ret;
         if(x->type==F32){if((ret=read_row(g,x,(unsigned)index,(float *)y->data+i*inner,(unsigned)inner)))return ret;}
         else if(!x->format&&(x->type==U8||x->type==I8)){if((ret=seek_read(g,x->offset+(uint32_t)(index*inner),(char *)y->data+i*inner,inner)))return ret;}
         else return -ENOTSUP;
       }return 0;
     }
     for(size_t o=0;o<outer;o++)for(size_t i=0;i<indices->count;i++){
       if((ret=glv_cancel(g->memory)))return ret;
       size_t index;if((ret=checked_index(((int64_t *)indices->data)[i],(uint32_t)dim,&index)))return ret;
       memcpy((char *)y->data+(o*indices->count+i)*inner*size,(char *)x->data+(o*dim+index)*inner*size,inner*size);
     }return 0;
   }
   if(indices->shape->rank!=x->shape->rank||indices->shape->rank!=y->shape->rank||indices->count!=y->count)return -EINVAL;
   for(unsigned d=0;d<y->shape->rank;d++)if(indices->shape->dims[d]!=y->shape->dims[d])return -EINVAL;
   for(unsigned d=0;d<x->shape->rank;d++)if(d!=axis&&indices->shape->dims[d]>x->shape->dims[d])return -EINVAL;
   for(size_t i=0;i<y->count;i++){
     if(!(i&1023)&&(ret=glv_cancel(g->memory)))return ret;
     size_t q=i,at=0,s=1,index;
     if((ret=checked_index(((int64_t *)indices->data)[i],(uint32_t)dim,&index)))return ret;
     for(unsigned r=x->shape->rank;r>0;r--){unsigned d=r-1;size_t c=q%y->shape->dims[d];q/=y->shape->dims[d];at+=(d==axis?index:c)*s;s*=x->shape->dims[d];}
     memcpy((char *)y->data+i*size,(char *)x->data+at*size,size);
   }return 0;
 }
 if(n->op==CAST){
   if(x->count!=y->count)return -EINVAL;
   for(size_t i=0;i<x->count;i++){
     if(!(i&1023)&&(ret=glv_cancel(g->memory)))return ret;
     /* Only model-required I64->F32 and identity casts are accepted. */
     if(x->type==y->type)memcpy((char *)y->data+i*size,(char *)x->data+i*size,size);
     else if(x->type==I64&&y->type==F32)((float *)y->data)[i]=(float)((int64_t *)x->data)[i];
     else if(x->type==I32&&y->type==F32)((float *)y->data)[i]=(float)((int32_t *)x->data)[i];
     else return -ENOTSUP;
   }return 0;
 }
 if(n->op==CUMSUM){
   unsigned axis;if((ret=axis_value(parameters->p[0],x->shape->rank,&axis)))return ret;
   if(!same(x,y)||parameters->p[1]||parameters->p[2])return -ENOTSUP;
   size_t inner=stride(x,axis),dim=x->shape->dims[axis],outer=x->count/(inner*dim);
   for(size_t o=0;o<outer;o++)for(size_t in=0;in<inner;in++){
     if((ret=glv_cancel(g->memory)))return ret;
     if(x->type==F32){float sum=0;for(size_t j=0;j<dim;j++){size_t at=(o*dim+j)*inner+in;sum+=((float *)x->data)[at];((float *)y->data)[at]=sum;}}
     else if(x->type==I64){int64_t sum=0;for(size_t j=0;j<dim;j++){size_t at=(o*dim+j)*inner+in;int64_t v=((int64_t *)x->data)[at];if((v>0&&sum>INT64_MAX-v)||(v<0&&sum<INT64_MIN-v))return -EOVERFLOW;sum+=v;((int64_t *)y->data)[at]=sum;}}
     else return -ENOTSUP;
   }return 0;
 }
 if(n->op!=TRANSPOSE&&n->op!=SLICE&&n->op!=EXPAND)return -ENOTSUP;
 if(n->op!=EXPAND&&x->shape->rank!=y->shape->rank)return -EINVAL;
 if(n->op==EXPAND&&x->shape->rank>y->shape->rank)return -EINVAL;
 unsigned seen=0;
 for(unsigned i=0;i<y->shape->rank;i++){
   if(n->op==TRANSPOSE){int d=parameters->p[i];if(d<0||(unsigned)d>=x->shape->rank||(seen&(1u<<d))||y->shape->dims[i]!=x->shape->dims[d])return -EINVAL;seen|=1u<<d;}
   else if(n->op==SLICE){int64_t start=parameters->p[i],step=parameters->p[8+i],end=start+(int64_t)(y->shape->dims[i]?y->shape->dims[i]-1:0)*step;
     if(!step||start<0||end<0||start>=x->shape->dims[i]||end>=x->shape->dims[i])return -EINVAL;}
   else if(i+x->shape->rank>=y->shape->rank){unsigned dim=x->shape->dims[i+x->shape->rank-y->shape->rank];if(dim!=1&&dim!=y->shape->dims[i])return -EINVAL;}
 }
 for(size_t i=0;i<y->count;i++){
   if(!(i&1023)&&(ret=glv_cancel(g->memory)))return ret;
   size_t q=i,index=0;
   for(unsigned r=y->shape->rank;r>0;r--){unsigned d=r-1;size_t c=q%y->shape->dims[d];q/=y->shape->dims[d];
     if(n->op==TRANSPOSE)index+=c*stride(x,(unsigned)parameters->p[d]);
     else if(n->op==SLICE)index+=(size_t)((int64_t)parameters->p[d]+(int64_t)c*parameters->p[8+d])*stride(x,d);
     else if(d+x->shape->rank>=y->shape->rank){unsigned k=d+x->shape->rank-y->shape->rank;index+=(x->shape->dims[k]==1?0:c)*stride(x,k);}
   }memcpy((char *)y->data+i*size,(char *)x->data+index*size,size);
 }return 0;
}
static size_t broadcast_index(const struct tensor *a,const struct tensor *out,size_t at)
{
 size_t index=0,s=1;
 for(unsigned r=out->shape->rank;r>0;r--){unsigned d=r-1;size_t c=at%out->shape->dims[d];at/=out->shape->dims[d];
   if(d+a->shape->rank>=out->shape->rank){unsigned k=d+a->shape->rank-out->shape->rank;index+=(a->shape->dims[k]==1?0:c)*s;s*=a->shape->dims[k];}
 }return index;
}
static int integer_binary(struct glv_asr_graph *g,struct node *n)
{
 struct tensor *x=&g->t[n->in[0]],*z=&g->t[n->in[1]],*y=&g->t[n->out[0]];
 if(x->type!=I64||z->type!=I64||y->type!=I64||x->shape->rank>y->shape->rank||z->shape->rank>y->shape->rank)return -ENOTSUP;
 for(unsigned i=0;i<y->shape->rank;i++)for(unsigned j=0;j<2;j++){
   struct tensor *v=j?z:x;if(i+v->shape->rank>=y->shape->rank){unsigned dim=v->shape->dims[i+v->shape->rank-y->shape->rank];if(dim!=1&&dim!=y->shape->dims[i])return -EINVAL;}
 }
 for(size_t i=0;i<y->count;i++){
   if(!(i&1023)){int ret=glv_cancel(g->memory);if(ret)return ret;}
   int64_t a=((int64_t *)x->data)[broadcast_index(x,y,i)],b=((int64_t *)z->data)[broadcast_index(z,y,i)],v;
   if(n->op==ADD){if(__builtin_add_overflow(a,b,&v))return -EOVERFLOW;}
   else if(n->op==SUB){if(__builtin_sub_overflow(a,b,&v))return -EOVERFLOW;}
   else if(n->op==MUL){if(__builtin_mul_overflow(a,b,&v))return -EOVERFLOW;}
   else if(n->op==DIV){if(!b||(a==INT64_MIN&&b==-1))return -ERANGE;v=a/b;}
   else return -ENOTSUP;
   ((int64_t *)y->data)[i]=v;
 }return 0;
}
static int matrix(struct glv_asr_graph *g,struct node *n)
{
 const struct parameters *parameters=&g->parameters[n->parameters];
 struct tensor *x=&g->t[n->in[0]],*w=&g->t[n->in[1]],*y=&g->t[n->out[0]];
 if(x->type!=F32||w->type!=F32||y->type!=F32||x->shape->rank<2||w->shape->rank<2)return -EINVAL;
 unsigned k=x->shape->dims[x->shape->rank-1],m=x->shape->dims[x->shape->rank-2];
 struct row_context context={g,w,0,true};struct glv_asr_rows callback={.read=rows,.opaque=&context
#ifdef GLV_ASR_ACCEL_ENABLED
 ,.prepare=((w->flags&CONST)&&(w->format&255u))?prepared_rows:NULL
#endif
 };int ret;
 if(n->op==GEMM){
   if(parameters->p[0]||x->shape->rank!=2||w->shape->rank!=2)return -ENOTSUP;
   unsigned out=w->shape->dims[parameters->p[1]?0:1];context.transpose=!parameters->p[1];
   if(k!=w->shape->dims[parameters->p[1]?1:0]||y->count!=(size_t)m*out)return -EINVAL;
   struct tensor *b=n->ni==3?&g->t[n->in[2]]:NULL;
   if(b&&b->type!=F32)return -EINVAL;
   return glv_asr_gemm(g->memory,x->data,x->count,y->data,y->count,m,k,out,&callback,parameters->a,parameters->b,b?b->data:NULL,b?b->count:0,b?b->shape:NULL);
 }
 unsigned out=w->shape->dims[w->shape->rank-1];
 if(k!=w->shape->dims[w->shape->rank-2]||y->shape->rank<2||y->shape->dims[y->shape->rank-2]!=m||y->shape->dims[y->shape->rank-1]!=out)return -EINVAL;
 unsigned rank=x->shape->rank>w->shape->rank?x->shape->rank:w->shape->rank;
 if(y->shape->rank!=rank)return -EINVAL;
 size_t batches=y->count/((size_t)m*out);
 for(unsigned d=0;d<rank-2;d++){
   unsigned a=d+x->shape->rank>=rank?x->shape->dims[d+x->shape->rank-rank]:1;
   unsigned b=d+w->shape->rank>=rank?w->shape->dims[d+w->shape->rank-rank]:1;
   if((a!=b&&a!=1&&b!=1)||y->shape->dims[d]!=(a==1?b:a))return -EINVAL;
 }
 /* A single 2-D RHS is shared by every leading input dimension. Fold these
  * dimensions into M so each SD weight row is read once for the whole node,
  * retaining the original K-order FP32 sum for every output element. */
 if(w->shape->rank==2){
   if(w->format&&!(w->format&256u))return -ENOTSUP;
   size_t total=x->count/k;if(total>UINT_MAX)return -EOVERFLOW;
   return glv_asr_matmul(g->memory,x->data,x->count,y->data,y->count,(unsigned)total,k,out,&callback);
 }
 for(size_t batch=0;batch<batches;batch++){
   size_t q=batch,ax=0,aw=0,sx=(size_t)m*k,sw=(size_t)k*out;
   for(unsigned r=rank-2;r>0;r--){unsigned d=r-1;size_t c=q%y->shape->dims[d];q/=y->shape->dims[d];
     if(d+x->shape->rank>=rank){unsigned dim=x->shape->dims[d+x->shape->rank-rank];ax+=(dim==1?0:c)*sx;sx*=dim;}
     if(d+w->shape->rank>=rank){unsigned dim=w->shape->dims[d+w->shape->rank-rank];aw+=(dim==1?0:c)*sw;sw*=dim;}
   }
   if(w->format&&(w->shape->rank!=2||!(w->format&256u)))return -ENOTSUP;
   context.batch=aw;
   ret=glv_asr_matmul(g->memory,(float *)x->data+ax,(size_t)m*k,(float *)y->data+batch*m*out,(size_t)m*out,m,k,out,&callback);if(ret)return ret;
 }return 0;
}
/* ABI2 preserves the published ONNX dynamic UINT8/INT8 arithmetic. It never
 * passes an already-quantized integer matrix through the Q4/Q8 float path. */
static float round_even(float value)
{
 float low=floorf(value),fraction=value-low;
 return fraction>.5f||(fraction==.5f&&fmodf(low,2.f)!=0.f)?low+1.f:low;
}
static int dynamic_quantize(struct glv_asr_graph *g,struct node *n)
{
 struct tensor *x=&g->t[n->in[0]],*y=&g->t[n->out[0]],*scale=&g->t[n->out[1]],*zero=&g->t[n->out[2]];
 if(x->type!=F32||y->type!=U8||x->count!=y->count||scale->type!=F32||scale->count!=1||zero->type!=U8||zero->count!=1)return -EINVAL;
 const float *src=x->data;float low=0,high=0;
 for(size_t i=0;i<x->count;i++){
  if(!(i&1023)){int ret=glv_cancel(g->memory);if(ret)return ret;}
  float v=src[i];if(!glv_finite(v))return -ERANGE;if(v<low)low=v;if(v>high)high=v;
 }
 float s=high==low?1.f:(high-low)/255.f;
 if(!glv_finite(s)||s<=0)return -ERANGE;
 float z=round_even(-low/s);if(z<0)z=0;if(z>255)z=255;
 *(float *)scale->data=s;*(uint8_t *)zero->data=(uint8_t)z;
 for(size_t i=0;i<x->count;i++){
  if(!(i&1023)){int ret=glv_cancel(g->memory);if(ret)return ret;}
  float v=round_even(src[i]/s)+z;if(v<0)v=0;if(v>255)v=255;
  ((uint8_t *)y->data)[i]=(uint8_t)v;
 }
 return 0;
}
static int dequantize(struct glv_asr_graph *g,struct node *n)
{
 struct tensor *x=&g->t[n->in[0]],*s=&g->t[n->in[1]],*z=&g->t[n->in[2]],*y=&g->t[n->out[0]];
 if((x->type!=I8&&x->type!=U8)||z->type!=x->type||z->count!=1||s->type!=F32||s->count!=1||y->type!=F32||y->count!=x->count)return -ENOTSUP;
 float scale=*(float *)s->data;if(!glv_finite(scale)||scale<=0)return -EBADMSG;
 int zero=x->type==U8?*(uint8_t *)z->data:*(int8_t *)z->data;
 for(size_t i=0;i<x->count;i++){
  if(!(i&1023)){int ret=glv_cancel(g->memory);if(ret)return ret;}
  int value=x->type==U8?((uint8_t *)x->data)[i]:((int8_t *)x->data)[i];
  ((float *)y->data)[i]=(float)(value-zero)*scale;
 }
 return 0;
}
#ifdef GLV_ASR_ACCEL_ENABLED
static int integer_row(void *opaque,unsigned row,int8_t *out,unsigned k)
{
 struct row_context *c=opaque;struct tensor *w=c->t;
 if(row>=w->rows||k!=w->columns)return -EINVAL;
 select_block(c->g,w);return seek_read(c->g,w->offset+(uint32_t)((size_t)row*k),out,k);
}
#endif
static int integer_matrix(struct glv_asr_graph *g,struct node *n)
{
 struct tensor *x=&g->t[n->in[0]],*w=&g->t[n->in[1]],*zx=&g->t[n->in[2]],*zw=&g->t[n->in[3]],*y=&g->t[n->out[0]];
 if(x->type!=U8||w->type!=I8||zx->type!=U8||zw->type!=I8||zx->count!=1||zw->count!=1||y->type!=I32||x->shape->rank<2||w->shape->rank!=2||y->shape->rank!=x->shape->rank)return -ENOTSUP;
 unsigned k=x->shape->dims[x->shape->rank-1],cols=w->shape->dims[1];
 size_t output_count;
 if(k!=w->shape->dims[0]||!k||k>32768u||!mul(x->count/k,cols,&output_count)||y->count!=output_count)return -EINVAL;
 for(unsigned i=0;i+1<x->shape->rank;i++)if(x->shape->dims[i]!=y->shape->dims[i])return -EINVAL;
 if(y->shape->dims[y->shape->rank-1]!=cols||w->format!=259||w->columns!=k||w->rows!=cols)return -EINVAL;
 size_t rows=x->count/k;int az=*(uint8_t *)zx->data,bz=*(int8_t *)zw->data,ret=0;
#ifdef GLV_ASR_ACCEL_ENABLED
 struct row_context context={g,w,0,false};struct glv_asr_integer_rows reader={integer_row,&context};
 ret=glv_asr_accel_integer(g->memory,x->data,y->data,(unsigned)rows,k,cols,(unsigned)az,bz,&reader);
 if(ret!=1)return ret;
#endif
 int8_t *row=graph_alloc(g,k);if(!row)return -ENOMEM;ret=0;
 for(unsigned o=0;o<cols&&!ret;o++){
#ifdef GLV_ASR_ACCEL_ENABLED
  select_block(g,w);
#endif
  ret=seek_read(g,w->offset+(uint32_t)((size_t)o*k),row,k);if(ret)break;
  for(size_t r=0;r<rows;r++){
   int32_t sum=0;const uint8_t *a=(uint8_t *)x->data+r*k;
   for(unsigned j=0;j<k;j++)sum+=((int)a[j]-az)*((int)row[j]-bz);
   ((int32_t *)y->data)[r*cols+o]=sum;
   if((ret=glv_cancel(g->memory)))break;
  }
 }
 glv_free(row);return ret;
}
static int evaluate_inner(struct glv_asr_graph *g,struct node *n)
{
 const struct parameters *parameters=&g->parameters[n->parameters];
 int ret;struct tensor *x=&g->t[n->in[0]],*y=&g->t[n->out[0]];
 for(unsigned i=0;i<n->ni;i++){
   struct tensor *t=&g->t[n->in[i]];
   bool row=(i==1&&(n->op==CONV||((n->op==MATMUL||n->op==GEMM)&&t->format)))||
     (i==0&&n->op==GATHER&&parameters->p[0]==0&&(t->flags&CONST)&&(t->format||t->type==U8||t->type==I8))||
     (i==1&&n->op==MATMUL_INTEGER);
   if(!row&&(ret=materialize(g,t)))return ret;
 }
 for(unsigned i=0;i<n->no;i++)if((ret=make_output(g,&g->t[n->out[i]])))return ret;
 if(n->op==DYNAMIC_QUANTIZE)return dynamic_quantize(g,n);
 if(n->op==MATMUL_INTEGER)return integer_matrix(g,n);
 if(n->op==DEQUANTIZE)return dequantize(g,n);
 if(n->op<=CUMSUM)return move(g,n);
 if(n->op>=ADD&&n->op<=POW){
   struct tensor *z=&g->t[n->in[1]];
   if(x->type!=F32)return integer_binary(g,n);
   if(y->type!=F32||z->type!=F32)return -EINVAL;
   return glv_asr_binary(g->memory,(enum glv_asr_binary)(n->op-ADD),x->data,x->count,x->shape,z->data,z->count,z->shape,y->data,y->count);
 }
 if(n->op>=NEG&&n->op<=RECIPROCAL){
   if(x->type!=F32||!same(x,y))return -EINVAL;
   return glv_asr_unary(g->memory,(enum glv_asr_unary)(n->op-NEG),x->data,y->data,x->count);
 }
 if(n->op==SOFTMAX){if(x->type!=F32||!same(x,y))return -EINVAL;
   return glv_asr_softmax(g->memory,x->data,x->count,x->shape,(unsigned)parameters->p[0],y->data,y->count);}
 if(n->op==REDUCE_MEAN||n->op==REDUCE_SUM){if(x->type!=F32||y->type!=F32)return -EINVAL;
   return glv_asr_reduce(g->memory,x->data,x->count,x->shape,(unsigned)parameters->p[0],n->op==REDUCE_MEAN,y->data,y->count);}
 if(n->op==MATMUL||n->op==GEMM)return matrix(g,n);
 if(n->op==CONV){
   if(x->type!=F32||y->type!=F32||n->ni<2)return -EINVAL;
   struct tensor *w=&g->t[n->in[1]],*b=n->ni==3?&g->t[n->in[2]]:NULL;
   if(w->type!=F32||(b&&b->type!=F32))return -EINVAL;
   struct glv_asr_conv c;unsigned fields[15];
   _Static_assert(sizeof(c)==sizeof(fields),"Conv ABI");
   for(unsigned i=0;i<15;i++){if(parameters->p[i]<0||(parameters->p[i]==0&&i<11))return -EINVAL;fields[i]=(unsigned)parameters->p[i];}
   memcpy(&c,fields,sizeof(c));
   struct row_context context={g,w,0,false};struct glv_asr_rows callback={.read=rows,.opaque=&context
#ifdef GLV_ASR_ACCEL_ENABLED
 ,.prepare=((w->flags&CONST)&&(w->format&255u))?prepared_rows:NULL
#endif
 };
   return glv_asr_conv(g->memory,x->data,x->count,b?b->data:NULL,b?b->count:0,y->data,y->count,&c,&callback);
 }return -ENOTSUP;
}
static int evaluate(struct glv_asr_graph *g,struct node *n)
{
 struct tensor *x=&g->t[n->in[0]],*y=&g->t[n->out[0]];
 bool eligible=n->no==1&&x->data&&!x->borrowed&&!(x->flags&CONST)&&x->left==1&&x->type==F32&&y->type==F32&&x->bytes==y->bytes;
 eligible=eligible&&(n->op==COPY||(n->op>=NEG&&n->op<=RECIPROCAL)||(n->op>=ADD&&n->op<=POW));
 if(eligible){y->data=x->data;y->borrowed=true;}
 int ret=evaluate_inner(g,n);
 if(eligible&&!ret){x->data=NULL;y->borrowed=false;}
 return ret;
}
static void cleanup(struct glv_asr_graph *g)
{if(g&&g->t)for(unsigned i=0;i<g->nt;i++)release(&g->t[i]);}
void glv_asr_graph_close(struct glv_asr_graph *g)
{if(!g)return;cleanup(g);if(g->data)fclose(g->data);
#ifdef GLV_ASR_ACCEL_ENABLED
glv_free(g->block);
#endif
glv_free(g->hot);glv_free(g->cache);glv_free(g->io);glv_free(g->row);glv_free(g->n);glv_free(g->parameters);glv_free(g->shapes);glv_free(g->t);glv_free(g);}
unsigned glv_asr_graph_inputs(const struct glv_asr_graph *g){return g?g->ni:0;}
unsigned glv_asr_graph_outputs(const struct glv_asr_graph *g){return g?g->no:0;}
unsigned glv_asr_graph_last_node(const struct glv_asr_graph *g){return g?g->last:0;}
void glv_asr_graph_io_statistics(const struct glv_asr_graph *g,struct glv_asr_io_stats *stats)
{if(stats){if(g)*stats=g->io_stats;else memset(stats,0,sizeof(*stats));}}
size_t glv_asr_graph_cached_bytes(const struct glv_asr_graph *g){return g&&g->cache?g->data_bytes:0;}
int glv_asr_graph_cache(struct glv_asr_graph *g,size_t limit)
{
 if(!g)return -EINVAL;if(g->cache)return 0;if(g->data_bytes>limit)return -E2BIG;
 int ret=glv_cancel(g->memory);if(ret)return ret;
 unsigned char *cache=glv_alloc(g->memory,g->data_bytes);if(!cache)return -ENOMEM;
 for(size_t at=0;at<g->data_bytes;){
   size_t bytes=g->data_bytes-at;if(bytes>16384)bytes=16384;
   ret=seek_read(g,(uint32_t)at,cache+at,bytes);if(ret)break;at+=bytes;
 }
 unsigned char digest[32];
 if(!ret)ret=glv_sha_buffer(cache,g->data_bytes,digest,g->memory);
 if(!ret&&memcmp(digest,g->expected_digest,32))ret=-EBADMSG;
 if(ret){glv_free(cache);return ret;}g->cache=cache;return 0;
}
int glv_asr_graph_hot_prefix(struct glv_asr_graph *g,size_t maximum)
{
 if(!g||maximum>8u*1024u*1024u)return -EINVAL;
 if(g->cache||g->hot||g->memory->asr_reference||g->memory->asr_no_blocks)return 0;
 /* Same verified open asset as the row reader. Immutable, bounded packed
  * bytes; keep this prefix across matrices/chunks rather than evicting it
  * on each matrix switch. No changes to tensor decoding or model values. */
 unsigned char *data=NULL;size_t n=maximum;if(n>g->data_bytes)n=g->data_bytes;
 /* Admit the streaming block before the prefix so a larger hot cache never
  * steals that buffer's budget. Neither operation starts a worker or does
  * asynchronous I/O. Mandatory graph allocations can evict either cache;
  * the independent2MiB system reserve remains enforced by glv_alloc. */
#ifdef GLV_ASR_ACCEL_ENABLED
 select_block(g,NULL);
#endif
 const size_t headroom=1024u*1024u;
 if(g->memory->used>g->memory->limit||g->memory->limit-g->memory->used<=headroom)return 0;
 size_t available=g->memory->limit-g->memory->used-headroom;
 if(n>available)n=available;n&=~(size_t)65535u;
 while(n&&glv_alloc_charge(n)>available)n=n>=65536u?n-65536u:0;
 for(;n>=128u*1024u;n/=2){
  if(g->memory->used>g->memory->limit||glv_alloc_charge(n)+headroom>g->memory->limit-g->memory->used)continue;
  data=glv_alloc(g->memory,n);if(data)break;
 }
 if(!data)return 0;
 int ret=0;for(size_t at=0;at<n;){size_t count=n-at;if(count>65536u)count=65536u;
  ret=seek_read(g,(uint32_t)at,data+at,count);if(ret)break;at+=count;}
 if(ret){glv_free(data);return ret;}
 g->hot=data;g->hot_bytes=n;g->memory->profile.asr_hot_peak_bytes+=n;return 0;
}
int glv_asr_graph_make_resident(struct glv_asr_graph *g)
{
 if(!g)return -EINVAL;
 int ret=glv_asr_graph_cache(g,SIZE_MAX);if(ret)return ret;
 /* cache() verifies RAM against the plan digest. No inference-time SD I/O. */
 if(g->data){int closed=fclose(g->data);g->data=NULL;if(closed)return -EIO;}
 glv_free(g->io);g->io=NULL;
#ifdef GLV_ASR_ACCEL_ENABLED
 glv_free(g->block);g->block=NULL;g->block_capacity=g->block_bytes=0;
 g->block_tensor=NULL;g->block_tried=true;
#endif
 return glv_cancel(g->memory);
}
int glv_asr_graph_io_info(const struct glv_asr_graph *g,bool output,unsigned index,
                         struct glv_asr_shape *shape,unsigned *type,size_t *bytes)
{
 if(!g||!shape||!type||!bytes||index>=(output?g->no:g->ni))return -EINVAL;
 const struct tensor *t=&g->t[output?g->outputs[index]:g->inputs[index]];
 *shape=*t->shape;*type=t->type;*bytes=t->bytes;return 0;
}
int glv_asr_graph_open(struct glv_asr_graph **out,const char *prefix,struct glv_memory *memory)
{
 if(!out||!prefix||!memory)return -EINVAL;*out=NULL;
 char path[256];if(strlen(prefix)>sizeof(path)-6)return -ENAMETOOLONG;
 snprintf(path,sizeof(path),"%s.plan",prefix);FILE *plan=NULL;size_t plan_size=0;
 int opened=glv_open_asset(path,&plan,&plan_size);if(opened)return opened;
 struct glv_asr_graph *g=glv_alloc(memory,sizeof(*g));int ret=0;unsigned char header[HEADER_BYTES];
 if(!g){fclose(plan);return -ENOMEM;}memset(g,0,sizeof(*g));g->memory=memory;
#ifdef GLV_ASR_ACCEL_ENABLED
 g->block_limit=128u*1024u;g->block_headroom=1024u*1024u;
#endif
 if((ret=exact(plan,header,sizeof(header))))goto done;
 unsigned version=u32(header+4);
 if(memcmp(header,"GLA1",4)||(version!=1&&version!=2)||u32(header+8)!=TENSOR_BYTES||u32(header+12)!=NODE_BYTES){ret=-EBADMSG;goto done;}
 g->nt=u32(header+16);g->nn=u32(header+20);g->ni=u32(header+24);g->no=u32(header+28);
 uint32_t data_bytes=u32(header+32);
 g->data_bytes=data_bytes;memcpy(g->expected_digest,header+36,32);
 if(!g->nt||g->nt>MAX_TENSORS||!g->nn||g->nn>MAX_NODES||!g->ni||g->ni>MAX_IO||!g->no||g->no>MAX_IO||data_bytes>INT32_MAX){ret=-EFBIG;goto done;}
 if(plan_size!=HEADER_BYTES+(size_t)g->nt*TENSOR_BYTES+(size_t)g->nn*NODE_BYTES+(g->ni+g->no)*4u){ret=-EBADMSG;goto done;}
 g->t=glv_alloc(memory,g->nt*sizeof(*g->t));if(!g->t){ret=-ENOMEM;goto done;}memset(g->t,0,g->nt*sizeof(*g->t));
 g->n=glv_alloc(memory,g->nn*sizeof(*g->n));if(!g->n){ret=-ENOMEM;goto done;}
 for(unsigned i=0;i<g->nt;i++){
   unsigned char buf[TENSOR_BYTES];if((ret=exact(plan,buf,sizeof(buf))))goto done;
   struct glv_asr_shape shape={.rank=u32(buf+4)};
   struct tensor *t=&g->t[i];t->type=u32(buf);t->shape=&shape;t->flags=u32(buf+8);
   if(!width(t->type)||shape.rank>8||t->flags&~7u||(t->flags&CONST&&t->flags&INPUT)){ret=-EBADMSG;goto done;}
   t->count=1;for(unsigned d=0;d<8;d++){shape.dims[d]=u32(buf+12+d*4);
     if(d<shape.rank&&(!shape.dims[d]||!mul(t->count,shape.dims[d],&t->count))){ret=-EOVERFLOW;goto done;}}
   if(!mul(t->count,width(t->type),&t->bytes)||t->bytes>GLV_MEMORY_LIMIT*8u){ret=-EFBIG;goto done;}
   t->offset=u32(buf+44);t->stored=u32(buf+48);t->format=u32(buf+52);t->rows=u32(buf+56);t->columns=u32(buf+60);t->padded=u32(buf+64);
   if(t->flags&CONST){
     if(t->offset>data_bytes||t->stored>data_bytes-t->offset){ret=-EBADMSG;goto done;}
     unsigned fmt=t->format&255u;
     if(t->format&~259u||fmt>(version==2?3u:2u)||(!fmt&&(t->stored!=t->bytes||(t->format&256u)))){ret=-EBADMSG;goto done;}
     if(fmt==3&&(t->type!=I8||t->format!=259||t->stored!=t->bytes||t->padded)){ret=-EBADMSG;goto done;}
     size_t logical;
     if(!t->rows||!t->columns||!mul(t->rows,t->columns,&logical)||logical!=t->count||
        ((t->format&256u)&&(t->shape->rank!=2||t->rows!=t->shape->dims[1]||t->columns!=t->shape->dims[0]))){ret=-EBADMSG;goto done;}
     if(fmt&&fmt!=3){size_t packed=0,rowbytes;
       if(t->type!=F32||!t->rows||!t->columns||t->padded<t->columns||t->padded%32||
          !mul(t->rows,t->columns,&logical)||logical!=t->count||!mul(t->padded/32,fmt==1?20u:36u,&rowbytes)||
          !mul(t->rows,rowbytes,&packed)||packed!=t->stored){ret=-EBADMSG;goto done;}
       if(rowbytes>g->row_bytes)g->row_bytes=rowbytes;
     }
   }else if(t->offset||t->stored||t->format){ret=-EBADMSG;goto done;}
   if((ret=intern_shape(g,&shape,&t->shape_id)))goto done;
 }
 for(unsigned i=0;i<g->nt;i++)g->t[i].shape=g->shapes+g->t[i].shape_id;
 /* uses doubles as producer state during validation, then is rebuilt. */
 for(unsigned i=0;i<g->nt;i++)if(g->t[i].flags&(CONST|INPUT))g->t[i].uses=1;
 for(unsigned i=0;i<g->nn;i++){
   unsigned char b[NODE_BYTES];if((ret=exact(plan,b,sizeof(b))))goto done;
   unsigned max_op=version==2?DEQUANTIZE:GEMM;
   struct node *n=&g->n[i];if(u32(b)>max_op||u32(b+4)>8||u32(b+8)>4){ret=-EBADMSG;goto done;}n->op=u32(b);n->ni=u32(b+4);n->no=u32(b+8);
   if(!n->ni||n->ni>8||!n->no||n->no>4||n->op<COPY||n->op>max_op){ret=-EBADMSG;goto done;}
   unsigned arity=(n->op==GATHER||n->op==GATHER_ELEMENTS||(n->op>=ADD&&n->op<=POW)||n->op==MATMUL)?2:1;
   if(n->op==MATMUL_INTEGER)arity=4;if(n->op==DEQUANTIZE)arity=3;
   if((n->op==CONV||n->op==GEMM)?(n->ni<2||n->ni>3):
      (n->op!=CONCAT&&n->ni!=arity)){ret=-EBADMSG;goto done;}
   if(n->op==DYNAMIC_QUANTIZE?n->no!=3:(n->op!=SPLIT&&n->no!=1)){ret=-EBADMSG;goto done;}
   for(unsigned j=0;j<8;j++){if(j<n->ni&&u32(b+12+j*4)>=g->nt){ret=-EBADMSG;goto done;}n->in[j]=u32(b+12+j*4);if(j<n->ni&&(n->in[j]>=g->nt||!g->t[n->in[j]].uses)){ret=-EBADMSG;goto done;}}
   for(unsigned j=0;j<4;j++){if(j<n->no&&u32(b+44+j*4)>=g->nt){ret=-EBADMSG;goto done;}n->out[j]=u32(b+44+j*4);if(j<n->no){if(n->out[j]>=g->nt||g->t[n->out[j]].uses){ret=-EBADMSG;goto done;}g->t[n->out[j]].uses=1;}}
   if((ret=intern_parameters(g,b,&n->parameters)))goto done;
 }
 for(unsigned i=0;i<g->ni+g->no;i++){
   unsigned char b[4];if((ret=exact(plan,b,4)))goto done;unsigned id=u32(b);
   unsigned flag=i<g->ni?INPUT:OUTPUT;if(id>=g->nt||!(g->t[id].flags&flag)||!g->t[id].uses){ret=-EBADMSG;goto done;}
   uint32_t *list=i<g->ni?g->inputs:g->outputs;unsigned at=i<g->ni?i:i-g->ni;
   for(unsigned j=0;j<at;j++)if(list[j]==id){ret=-EBADMSG;goto done;}list[at]=id;
 }
 if(fgetc(plan)!=EOF||ferror(plan)){ret=-EBADMSG;goto done;}
 for(unsigned i=0;i<g->nt;i++)g->t[i].uses=0;
 for(unsigned i=0;i<g->nn;i++)for(unsigned j=0;j<g->n[i].ni;j++)g->t[g->n[i].in[j]].uses++;
 for(unsigned i=0;i<g->no;i++)g->t[g->outputs[i]].uses++;
 snprintf(path,sizeof(path),"%s.data",prefix);size_t actual_size=0;
 ret=glv_open_asset(path,&g->data,&actual_size);if(ret)goto done;
 if(actual_size!=data_bytes){ret=-EBADMSG;goto done;}
 g->io=glv_alloc(memory,16384);if(!g->io){ret=-ENOMEM;goto done;}
 if(setvbuf(g->data,(char *)g->io,_IOFBF,16384)){ret=-EIO;goto done;}
 unsigned char hash[32];ret=glv_sha_file(g->data,data_bytes,hash,memory);if(ret)goto done;
 if(memcmp(hash,header+36,32)){ret=-EBADMSG;goto done;}
 g->cursor=data_bytes;
 g->row=glv_alloc(memory,g->row_bytes?g->row_bytes:1);if(!g->row){ret=-ENOMEM;goto done;}
done:
 fclose(plan);if(ret){glv_asr_graph_close(g);return ret;}*out=g;return 0;
}
static int run_nodes(struct glv_asr_graph *g)
{int ret=0;
 for(unsigned i=0;i<g->nn;i++){
   g->last=i;if((ret=glv_cancel(g->memory)))return ret;
   uint64_t op_start=glv_now_us();ret=evaluate(g,&g->n[i]);
   if(g->n[i].op<28)g->memory->profile.asr_op_us[g->n[i].op]+=glv_now_us()-op_start;
   if(ret)return ret;
   struct node *n=&g->n[i];
#ifdef GLV_ASR_GRAPH_TRACE
   extern void glv_asr_trace(unsigned,unsigned,const void *,size_t);
   for(unsigned j=0;j<n->no;j++)glv_asr_trace(i,j,g->t[n->out[j]].data,g->t[n->out[j]].bytes);
#endif
   for(unsigned j=0;j<n->ni;j++){struct tensor *t=&g->t[n->in[j]];if(!--t->left)release(t);}
   for(unsigned j=0;j<n->no;j++){struct tensor *t=&g->t[n->out[j]];if(!t->left)release(t);}
 }
return 0;
}
int glv_asr_graph_run(struct glv_asr_graph *g,const struct glv_asr_io *in,unsigned ni,const struct glv_asr_io *out,unsigned no)
{
 if(!g||!in||!out||ni!=g->ni||no!=g->no)return -EINVAL;
 int ret=glv_cancel(g->memory);if(ret)return ret;cleanup(g);g->last=0;
 for(unsigned i=0;i<ni+no;i++){
   const struct glv_asr_io *v=i<ni?&in[i]:&out[i-ni];struct tensor *t=&g->t[i<ni?g->inputs[i]:g->outputs[i-ni]];
   if(!v->data||v->bytes<t->bytes)return -EINVAL;
   for(unsigned j=0;j<i;j++){
     const struct glv_asr_io *w=j<ni?&in[j]:&out[j-ni];
     if((i>=ni||j>=ni)&&aliases(v->data,t->bytes,w->data,w->bytes))return -EINVAL;
   }
 }
 for(unsigned i=0;i<g->nt;i++)g->t[i].left=g->t[i].uses;
 for(unsigned i=0;i<ni;i++){struct tensor *t=&g->t[g->inputs[i]];t->data=in[i].data;t->borrowed=true;}
 ret=run_nodes(g);if(ret)goto done;
 for(unsigned i=0;i<no;i++){
   struct tensor *t=&g->t[g->outputs[i]];if((ret=glv_cancel(g->memory))||(ret=materialize(g,t)))goto done;
   memcpy(out[i].data,t->data,t->bytes);
 }
done:cleanup(g);return ret;
}

/* Transfer state buffers at their last use. Input 0 is the caller-owned
 * rolling feature window; other inputs are consumed even on graph failure.
 * Success transfers fresh outputs to caller; failure leaves outputs empty. */
int glv_asr_graph_exchange(struct glv_asr_graph *g,struct glv_asr_io *in,unsigned ni,struct glv_asr_io *out,unsigned no)
{
 if(!g||!in||!out||ni!=g->ni||no!=g->no)return -EINVAL;
 for(unsigned i=0;i<ni;i++){
  struct tensor *t=&g->t[g->inputs[i]];if(!in[i].data||in[i].bytes<t->bytes)return -EINVAL;
  for(unsigned j=0;j<i;j++)if(aliases(in[i].data,t->bytes,in[j].data,in[j].bytes))return -EINVAL;
 }
 for(unsigned i=0;i<no;i++)if(out[i].data||out[i].bytes<g->t[g->outputs[i]].bytes)return -EINVAL;
 int ret=glv_cancel(g->memory);if(ret)return ret;cleanup(g);g->last=0;
 for(unsigned i=0;i<g->nt;i++)g->t[i].left=g->t[i].uses;
 for(unsigned i=0;i<ni;i++){struct tensor *t=&g->t[g->inputs[i]];t->data=in[i].data;t->borrowed=i==0;if(i)in[i].data=NULL;}
 void *pending[MAX_IO]={0};bool copies[MAX_IO]={0};
 ret=run_nodes(g);if(ret)goto done;
 for(unsigned i=0;i<no;i++){
  struct tensor *t=&g->t[g->outputs[i]];
  if((ret=glv_cancel(g->memory))||(ret=materialize(g,t)))goto done;
  if(t->borrowed||t->flags&CONST){copies[i]=true;pending[i]=graph_alloc(g,t->bytes?t->bytes:1);if(!pending[i]){ret=-ENOMEM;goto done;}memcpy(pending[i],t->data,t->bytes);}
  else pending[i]=t->data;
 }
 for(unsigned i=0;i<no;i++){out[i].data=pending[i];if(!copies[i])g->t[g->outputs[i]].data=NULL;pending[i]=NULL;}
done:
 for(unsigned i=0;i<no;i++)if(copies[i])glv_free(pending[i]);
 cleanup(g);return ret;
}
