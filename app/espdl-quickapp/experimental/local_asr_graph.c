/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr_graph.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* GLA1 is deliberately fixed width/little endian. Do not fread C structures.
 * Exporter owns ONNX specialization; this reader distrusts all counts,
 * offsets, producer order, shapes and indices. ABI 1 is experimental. */
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
#define I32 6u
#define I64 7u
#define BOOL 9u
enum {COPY=1,TRANSPOSE,GATHER,GATHER_ELEMENTS,SLICE,CONCAT,SPLIT,EXPAND,CAST,
      CUMSUM,ADD,SUB,MUL,DIV,POW,NEG,RELU,SIGMOID,TANH,RECIPROCAL,SOFTMAX,
      REDUCE_MEAN,REDUCE_SUM,MATMUL,CONV,GEMM};
struct tensor {
  struct glv_asr_shape shape;
  uint32_t type,flags,offset,stored,format,rows,columns,padded;
  size_t count,bytes;
  void *data;
  unsigned uses,left;
  bool borrowed;
};
struct node {uint32_t op,ni,no,in[8],out[4];int32_t p[32];float a,b;};
struct glv_asr_graph {
  struct glv_memory *memory;
  FILE *data;
  struct tensor *t;
  struct node *n;
  unsigned nt,nn,ni,no,last;
  uint32_t inputs[MAX_IO],outputs[MAX_IO];
  unsigned char *row;
  unsigned char *io,*cache;
  size_t row_bytes,cursor,data_bytes;
  struct glv_asr_io_stats io_stats;
};
static bool mul(size_t a,size_t b,size_t *r)
{if(b&&a>SIZE_MAX/b)return false;*r=a*b;return true;}
static size_t width(unsigned type)
{return type==F32||type==I32?4:type==I64?8:type==BOOL?1:0;}
static int exact(FILE *f,void *data,size_t n)
{return glv_read_exact(f,data,n);}
static uint32_t u32(const unsigned char *p){return glv_u32(p);}
static float f32(const unsigned char *p)
{uint32_t v=u32(p);float f;memcpy(&f,&v,4);return f;}
static bool same(const struct tensor *a,const struct tensor *b)
{return a->type==b->type&&a->shape.rank==b->shape.rank&&
 !memcmp(a->shape.dims,b->shape.dims,a->shape.rank*sizeof(uint32_t));}
static bool aliases(const void *a,size_t na,const void *b,size_t nb)
{
 uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
 if(na>UINTPTR_MAX-x||nb>UINTPTR_MAX-y)return true;
 return na&&nb&&x<y+nb&&y<x+na;
}
static int seek_read(struct glv_asr_graph *g,uint32_t off,void *p,size_t bytes)
{
 int r=glv_cancel(g->memory);if(r)return r;
 if(off>g->data_bytes||bytes>g->data_bytes-off)return -EBADMSG;
 if(g->cache){memcpy(p,g->cache+off,bytes);return 0;}
 if(off>INT32_MAX)return -EIO;
 /* NuttX fseeko flushes the read buffer even when the next row is exactly
  * contiguous. Keep the FILE cursor and its owned read-ahead buffer intact
  * on sequential reads; never change tensor order or arithmetic. */
 if(g->cursor!=off){
   if(fseek(g->data,(long)off,SEEK_SET))return -EIO;
   g->io_stats.seeks++;g->cursor=off;
 }
 g->io_stats.reads++;r=exact(g->data,p,bytes);
 if(r)g->cursor=SIZE_MAX;else{g->cursor=off+bytes;g->io_stats.bytes+=bytes;}
 return r;
}
/* Decode one stored output row. Format 1/2 is group32 symmetric Q4/Q8,
 * scales FP32 then packed values within each row (same as export tool). */
static int read_row(struct glv_asr_graph *g,struct tensor *t,unsigned row,float *dest,unsigned count)
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
struct row_context {struct glv_asr_graph *g;struct tensor *t;size_t batch;bool transpose;};
static int rows(void *opaque,unsigned row,float *dest,unsigned count)
{
 struct row_context *r=opaque;struct tensor *t=r->t;
 if((t->flags&CONST)&&(t->format||!t->data))return read_row(r->g,t,row,dest,count);
 if(t->type!=F32||!t->data)return -EINVAL;
 const float *src=(const float *)t->data+r->batch;
 if(r->transpose){unsigned n=t->shape.dims[t->shape.rank-1];
   if(row>=n||count!=t->shape.dims[t->shape.rank-2])return -EINVAL;
   for(unsigned k=0;k<count;k++)dest[k]=src[(size_t)k*n+row];
 }else {
   if(!t->shape.rank||row>=t->shape.dims[0]||count!=t->count/t->shape.dims[0]||r->batch)return -EINVAL;
   memcpy(dest,src+(size_t)row*count,(size_t)count*4);
 }
 return glv_cancel(r->g->memory);
}
static void release(struct tensor *t)
{if(!t->borrowed)glv_free(t->data);t->data=NULL;t->borrowed=false;}
static int materialize(struct glv_asr_graph *g,struct tensor *t)
{
 if(t->data)return 0;
 if(!(t->flags&CONST))return -EINVAL;
 t->data=glv_alloc(g->memory,t->bytes?t->bytes:1);if(!t->data)return -ENOMEM;
 int ret=0;
 if(!t->format)ret=seek_read(g,t->offset,t->data,t->bytes);
 else {
   /* Matrix-RHS transposition is format bit 8; reconstruct only for a
    * non-matrix consumer. Standard MatMul/Conv/Gather never needs this. */
   unsigned fmt=t->format;t->format&=255u;
   float *scratch=glv_alloc(g->memory,(size_t)t->columns*4);
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
 if(t->data)return -EINVAL;
 t->data=glv_alloc(g->memory,t->bytes?t->bytes:1);
 return t->data?0:-ENOMEM;
}
static int axis_value(int a,unsigned rank,unsigned *out)
{if(a<0)a+=(int)rank;if(a<0||(unsigned)a>=rank)return -EINVAL;*out=(unsigned)a;return 0;}
static size_t stride(const struct tensor *t,unsigned axis)
{size_t r=1;for(unsigned i=axis+1;i<t->shape.rank;i++)r*=t->shape.dims[i];return r;}
static int checked_index(int64_t index,uint32_t dim,size_t *out)
{if(index<0)index+=dim;if(index<0||(uint64_t)index>=dim)return -ERANGE;*out=(size_t)index;return 0;}
static int move(struct glv_asr_graph *g,struct node *n)
{
 struct tensor *x=&g->t[n->in[0]],*y=&g->t[n->out[0]];
 size_t size=width(x->type);int ret;
 if(n->op!=CAST&&x->type!=y->type)return -EINVAL;
 if(n->op==COPY){if(x->bytes!=y->bytes)return -EINVAL;memcpy(y->data,x->data,y->bytes);return glv_cancel(g->memory);}
 if(n->op==CONCAT||n->op==SPLIT){
   unsigned axis;if((ret=axis_value(n->p[0],x->shape.rank,&axis)))return ret;
   if(y->shape.rank!=x->shape.rank)return -EINVAL;
   if(n->op==CONCAT)for(unsigned d=0;d<x->shape.rank;d++)if(d!=axis&&y->shape.dims[d]!=x->shape.dims[d])return -EINVAL;
   size_t inner=stride(x,axis),outer=x->count/(inner*x->shape.dims[axis]),sum=0;
   unsigned total=n->op==CONCAT?n->ni:n->no;
   for(unsigned i=0;i<total;i++){
     struct tensor *v=&g->t[n->op==CONCAT?n->in[i]:n->out[i]];
     if(v->type!=x->type||v->shape.rank!=x->shape.rank)return -EINVAL;
     for(unsigned d=0;d<x->shape.rank;d++)if(d!=axis&&v->shape.dims[d]!=x->shape.dims[d])return -EINVAL;
     sum+=v->shape.dims[axis];
   }
   if(sum!=(n->op==CONCAT?y:x)->shape.dims[axis])return -EINVAL;
   size_t at=0;
   for(unsigned i=0;i<total;i++){
     struct tensor *v=&g->t[n->op==CONCAT?n->in[i]:n->out[i]];
     size_t block=(size_t)v->shape.dims[axis]*inner;
     for(size_t j=0;j<outer;j++){
       if((ret=glv_cancel(g->memory)))return ret;
       if(n->op==CONCAT)memcpy((char *)y->data+(j*sum*inner+at)*size,(char *)v->data+j*block*size,block*size);
       else memcpy((char *)v->data+j*block*size,(char *)x->data+(j*sum*inner+at)*size,block*size);
     }at+=block;
   }return 0;
 }
 if(n->op==GATHER||n->op==GATHER_ELEMENTS){
   struct tensor *indices=&g->t[n->in[1]];unsigned axis;
   if(indices->type!=I64||(ret=axis_value(n->p[0],x->shape.rank,&axis)))return -EINVAL;
   size_t inner=stride(x,axis),dim=x->shape.dims[axis];
   if(n->op==GATHER){
     size_t outer=x->count/(inner*dim);
     size_t count;if(!mul(outer,indices->count,&count)||!mul(count,inner,&count))return -EOVERFLOW;
     if(y->count!=count)return -EINVAL;
     if(!x->data){
       if(axis||x->type!=F32||inner!=x->columns||x->shape.dims[0]!=x->rows)return -EINVAL;
       for(size_t i=0;i<indices->count;i++){
         size_t index;if((ret=checked_index(((int64_t *)indices->data)[i],(uint32_t)dim,&index)))return ret;
         if((ret=read_row(g,x,(unsigned)index,(float *)y->data+i*inner,(unsigned)inner)))return ret;
       }return 0;
     }
     for(size_t o=0;o<outer;o++)for(size_t i=0;i<indices->count;i++){
       if((ret=glv_cancel(g->memory)))return ret;
       size_t index;if((ret=checked_index(((int64_t *)indices->data)[i],(uint32_t)dim,&index)))return ret;
       memcpy((char *)y->data+(o*indices->count+i)*inner*size,(char *)x->data+(o*dim+index)*inner*size,inner*size);
     }return 0;
   }
   if(indices->shape.rank!=x->shape.rank||indices->shape.rank!=y->shape.rank||indices->count!=y->count)return -EINVAL;
   for(unsigned d=0;d<y->shape.rank;d++)if(indices->shape.dims[d]!=y->shape.dims[d])return -EINVAL;
   for(unsigned d=0;d<x->shape.rank;d++)if(d!=axis&&indices->shape.dims[d]>x->shape.dims[d])return -EINVAL;
   for(size_t i=0;i<y->count;i++){
     if(!(i&1023)&&(ret=glv_cancel(g->memory)))return ret;
     size_t q=i,at=0,s=1,index;
     if((ret=checked_index(((int64_t *)indices->data)[i],(uint32_t)dim,&index)))return ret;
     for(unsigned r=x->shape.rank;r>0;r--){unsigned d=r-1;size_t c=q%y->shape.dims[d];q/=y->shape.dims[d];at+=(d==axis?index:c)*s;s*=x->shape.dims[d];}
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
     else return -ENOTSUP;
   }return 0;
 }
 if(n->op==CUMSUM){
   unsigned axis;if((ret=axis_value(n->p[0],x->shape.rank,&axis)))return ret;
   if(!same(x,y)||n->p[1]||n->p[2])return -ENOTSUP;
   size_t inner=stride(x,axis),dim=x->shape.dims[axis],outer=x->count/(inner*dim);
   for(size_t o=0;o<outer;o++)for(size_t in=0;in<inner;in++){
     if((ret=glv_cancel(g->memory)))return ret;
     if(x->type==F32){float sum=0;for(size_t j=0;j<dim;j++){size_t at=(o*dim+j)*inner+in;sum+=((float *)x->data)[at];((float *)y->data)[at]=sum;}}
     else if(x->type==I64){int64_t sum=0;for(size_t j=0;j<dim;j++){size_t at=(o*dim+j)*inner+in;int64_t v=((int64_t *)x->data)[at];if((v>0&&sum>INT64_MAX-v)||(v<0&&sum<INT64_MIN-v))return -EOVERFLOW;sum+=v;((int64_t *)y->data)[at]=sum;}}
     else return -ENOTSUP;
   }return 0;
 }
 if(n->op!=TRANSPOSE&&n->op!=SLICE&&n->op!=EXPAND)return -ENOTSUP;
 if(n->op!=EXPAND&&x->shape.rank!=y->shape.rank)return -EINVAL;
 if(n->op==EXPAND&&x->shape.rank>y->shape.rank)return -EINVAL;
 unsigned seen=0;
 for(unsigned i=0;i<y->shape.rank;i++){
   if(n->op==TRANSPOSE){int d=n->p[i];if(d<0||(unsigned)d>=x->shape.rank||(seen&(1u<<d))||y->shape.dims[i]!=x->shape.dims[d])return -EINVAL;seen|=1u<<d;}
   else if(n->op==SLICE){int64_t start=n->p[i],step=n->p[8+i],end=start+(int64_t)(y->shape.dims[i]?y->shape.dims[i]-1:0)*step;
     if(!step||start<0||end<0||start>=x->shape.dims[i]||end>=x->shape.dims[i])return -EINVAL;}
   else if(i+x->shape.rank>=y->shape.rank){unsigned dim=x->shape.dims[i+x->shape.rank-y->shape.rank];if(dim!=1&&dim!=y->shape.dims[i])return -EINVAL;}
 }
 for(size_t i=0;i<y->count;i++){
   if(!(i&1023)&&(ret=glv_cancel(g->memory)))return ret;
   size_t q=i,index=0;
   for(unsigned r=y->shape.rank;r>0;r--){unsigned d=r-1;size_t c=q%y->shape.dims[d];q/=y->shape.dims[d];
     if(n->op==TRANSPOSE)index+=c*stride(x,(unsigned)n->p[d]);
     else if(n->op==SLICE)index+=(size_t)((int64_t)n->p[d]+(int64_t)c*n->p[8+d])*stride(x,d);
     else if(d+x->shape.rank>=y->shape.rank){unsigned k=d+x->shape.rank-y->shape.rank;index+=(x->shape.dims[k]==1?0:c)*stride(x,k);}
   }memcpy((char *)y->data+i*size,(char *)x->data+index*size,size);
 }return 0;
}
static size_t broadcast_index(const struct tensor *a,const struct tensor *out,size_t at)
{
 size_t index=0,s=1;
 for(unsigned r=out->shape.rank;r>0;r--){unsigned d=r-1;size_t c=at%out->shape.dims[d];at/=out->shape.dims[d];
   if(d+a->shape.rank>=out->shape.rank){unsigned k=d+a->shape.rank-out->shape.rank;index+=(a->shape.dims[k]==1?0:c)*s;s*=a->shape.dims[k];}
 }return index;
}
static int integer_binary(struct glv_asr_graph *g,struct node *n)
{
 struct tensor *x=&g->t[n->in[0]],*z=&g->t[n->in[1]],*y=&g->t[n->out[0]];
 if(x->type!=I64||z->type!=I64||y->type!=I64||x->shape.rank>y->shape.rank||z->shape.rank>y->shape.rank)return -ENOTSUP;
 for(unsigned i=0;i<y->shape.rank;i++)for(unsigned j=0;j<2;j++){
   struct tensor *v=j?z:x;if(i+v->shape.rank>=y->shape.rank){unsigned dim=v->shape.dims[i+v->shape.rank-y->shape.rank];if(dim!=1&&dim!=y->shape.dims[i])return -EINVAL;}
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
 struct tensor *x=&g->t[n->in[0]],*w=&g->t[n->in[1]],*y=&g->t[n->out[0]];
 if(x->type!=F32||w->type!=F32||y->type!=F32||x->shape.rank<2||w->shape.rank<2)return -EINVAL;
 unsigned k=x->shape.dims[x->shape.rank-1],m=x->shape.dims[x->shape.rank-2];
 struct row_context context={g,w,0,true};struct glv_asr_rows callback={rows,&context};int ret;
 if(n->op==GEMM){
   if(n->p[0]||x->shape.rank!=2||w->shape.rank!=2)return -ENOTSUP;
   unsigned out=w->shape.dims[n->p[1]?0:1];context.transpose=!n->p[1];
   if(k!=w->shape.dims[n->p[1]?1:0]||y->count!=(size_t)m*out)return -EINVAL;
   struct tensor *b=n->ni==3?&g->t[n->in[2]]:NULL;
   if(b&&b->type!=F32)return -EINVAL;
   return glv_asr_gemm(g->memory,x->data,x->count,y->data,y->count,m,k,out,&callback,n->a,n->b,b?b->data:NULL,b?b->count:0,b?&b->shape:NULL);
 }
 unsigned out=w->shape.dims[w->shape.rank-1];
 if(k!=w->shape.dims[w->shape.rank-2]||y->shape.rank<2||y->shape.dims[y->shape.rank-2]!=m||y->shape.dims[y->shape.rank-1]!=out)return -EINVAL;
 unsigned rank=x->shape.rank>w->shape.rank?x->shape.rank:w->shape.rank;
 if(y->shape.rank!=rank)return -EINVAL;
 size_t batches=y->count/((size_t)m*out);
 for(unsigned d=0;d<rank-2;d++){
   unsigned a=d+x->shape.rank>=rank?x->shape.dims[d+x->shape.rank-rank]:1;
   unsigned b=d+w->shape.rank>=rank?w->shape.dims[d+w->shape.rank-rank]:1;
   if((a!=b&&a!=1&&b!=1)||y->shape.dims[d]!=(a==1?b:a))return -EINVAL;
 }
 /* A single 2-D RHS is shared by every leading input dimension. Fold these
  * dimensions into M so each SD weight row is read once for the whole node,
  * retaining the original K-order FP32 sum for every output element. */
 if(w->shape.rank==2){
   if(w->format&&!(w->format&256u))return -ENOTSUP;
   size_t total=x->count/k;if(total>UINT_MAX)return -EOVERFLOW;
   return glv_asr_matmul(g->memory,x->data,x->count,y->data,y->count,(unsigned)total,k,out,&callback);
 }
 for(size_t batch=0;batch<batches;batch++){
   size_t q=batch,ax=0,aw=0,sx=(size_t)m*k,sw=(size_t)k*out;
   for(unsigned r=rank-2;r>0;r--){unsigned d=r-1;size_t c=q%y->shape.dims[d];q/=y->shape.dims[d];
     if(d+x->shape.rank>=rank){unsigned dim=x->shape.dims[d+x->shape.rank-rank];ax+=(dim==1?0:c)*sx;sx*=dim;}
     if(d+w->shape.rank>=rank){unsigned dim=w->shape.dims[d+w->shape.rank-rank];aw+=(dim==1?0:c)*sw;sw*=dim;}
   }
   if(w->format&&(w->shape.rank!=2||!(w->format&256u)))return -ENOTSUP;
   context.batch=aw;
   ret=glv_asr_matmul(g->memory,(float *)x->data+ax,(size_t)m*k,(float *)y->data+batch*m*out,(size_t)m*out,m,k,out,&callback);if(ret)return ret;
 }return 0;
}
static int evaluate(struct glv_asr_graph *g,struct node *n)
{
 int ret;struct tensor *x=&g->t[n->in[0]],*y=&g->t[n->out[0]];
 for(unsigned i=0;i<n->ni;i++){
   struct tensor *t=&g->t[n->in[i]];
   bool row=(i==1&&(n->op==CONV||((n->op==MATMUL||n->op==GEMM)&&t->format)))||
     (i==0&&n->op==GATHER&&n->p[0]==0&&t->format);
   if(!row&&(ret=materialize(g,t)))return ret;
 }
 for(unsigned i=0;i<n->no;i++)if((ret=make_output(g,&g->t[n->out[i]])))return ret;
 if(n->op<=CUMSUM)return move(g,n);
 if(n->op>=ADD&&n->op<=POW){
   struct tensor *z=&g->t[n->in[1]];
   if(x->type!=F32)return integer_binary(g,n);
   if(y->type!=F32||z->type!=F32)return -EINVAL;
   return glv_asr_binary(g->memory,(enum glv_asr_binary)(n->op-ADD),x->data,x->count,&x->shape,z->data,z->count,&z->shape,y->data,y->count);
 }
 if(n->op>=NEG&&n->op<=RECIPROCAL){
   if(x->type!=F32||!same(x,y))return -EINVAL;
   return glv_asr_unary(g->memory,(enum glv_asr_unary)(n->op-NEG),x->data,y->data,x->count);
 }
 if(n->op==SOFTMAX){if(x->type!=F32||!same(x,y))return -EINVAL;
   return glv_asr_softmax(g->memory,x->data,x->count,&x->shape,(unsigned)n->p[0],y->data,y->count);}
 if(n->op==REDUCE_MEAN||n->op==REDUCE_SUM){if(x->type!=F32||y->type!=F32)return -EINVAL;
   return glv_asr_reduce(g->memory,x->data,x->count,&x->shape,(unsigned)n->p[0],n->op==REDUCE_MEAN,y->data,y->count);}
 if(n->op==MATMUL||n->op==GEMM)return matrix(g,n);
 if(n->op==CONV){
   if(x->type!=F32||y->type!=F32||n->ni<2)return -EINVAL;
   struct tensor *w=&g->t[n->in[1]],*b=n->ni==3?&g->t[n->in[2]]:NULL;
   if(w->type!=F32||(b&&b->type!=F32))return -EINVAL;
   struct glv_asr_conv c;unsigned fields[15];
   _Static_assert(sizeof(c)==sizeof(fields),"Conv ABI");
   for(unsigned i=0;i<15;i++){if(n->p[i]<0||(n->p[i]==0&&i<11))return -EINVAL;fields[i]=(unsigned)n->p[i];}
   memcpy(&c,fields,sizeof(c));
   struct row_context context={g,w,0,false};struct glv_asr_rows callback={rows,&context};
   return glv_asr_conv(g->memory,x->data,x->count,b?b->data:NULL,b?b->count:0,y->data,y->count,&c,&callback);
 }return -ENOTSUP;
}
static void cleanup(struct glv_asr_graph *g)
{if(g&&g->t)for(unsigned i=0;i<g->nt;i++)release(&g->t[i]);}
void glv_asr_graph_close(struct glv_asr_graph *g)
{if(!g)return;cleanup(g);if(g->data)fclose(g->data);glv_free(g->cache);glv_free(g->io);glv_free(g->row);glv_free(g->n);glv_free(g->t);glv_free(g);}
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
 if(ret){glv_free(cache);return ret;}g->cache=cache;return 0;
}
int glv_asr_graph_io_info(const struct glv_asr_graph *g,bool output,unsigned index,
                         struct glv_asr_shape *shape,unsigned *type,size_t *bytes)
{
 if(!g||!shape||!type||!bytes||index>=(output?g->no:g->ni))return -EINVAL;
 const struct tensor *t=&g->t[output?g->outputs[index]:g->inputs[index]];
 *shape=t->shape;*type=t->type;*bytes=t->bytes;return 0;
}
int glv_asr_graph_open(struct glv_asr_graph **out,const char *prefix,struct glv_memory *memory)
{
 if(!out||!prefix||!memory)return -EINVAL;*out=NULL;
 char path[256];if(strlen(prefix)>sizeof(path)-6)return -ENAMETOOLONG;
 snprintf(path,sizeof(path),"%s.plan",prefix);FILE *plan=NULL;size_t plan_size=0;
 int opened=glv_open_asset(path,&plan,&plan_size);if(opened)return opened;
 struct glv_asr_graph *g=glv_alloc(memory,sizeof(*g));int ret=0;unsigned char header[HEADER_BYTES];
 if(!g){fclose(plan);return -ENOMEM;}memset(g,0,sizeof(*g));g->memory=memory;
 if((ret=exact(plan,header,sizeof(header))))goto done;
 if(memcmp(header,"GLA1",4)||u32(header+4)!=1||u32(header+8)!=TENSOR_BYTES||u32(header+12)!=NODE_BYTES){ret=-EBADMSG;goto done;}
 g->nt=u32(header+16);g->nn=u32(header+20);g->ni=u32(header+24);g->no=u32(header+28);
 uint32_t data_bytes=u32(header+32);
 g->data_bytes=data_bytes;
 if(!g->nt||g->nt>MAX_TENSORS||!g->nn||g->nn>MAX_NODES||!g->ni||g->ni>MAX_IO||!g->no||g->no>MAX_IO||data_bytes>INT32_MAX){ret=-EFBIG;goto done;}
 if(plan_size!=HEADER_BYTES+(size_t)g->nt*TENSOR_BYTES+(size_t)g->nn*NODE_BYTES+(g->ni+g->no)*4u){ret=-EBADMSG;goto done;}
 g->t=glv_alloc(memory,g->nt*sizeof(*g->t));if(!g->t){ret=-ENOMEM;goto done;}memset(g->t,0,g->nt*sizeof(*g->t));
 g->n=glv_alloc(memory,g->nn*sizeof(*g->n));if(!g->n){ret=-ENOMEM;goto done;}
 for(unsigned i=0;i<g->nt;i++){
   unsigned char buf[TENSOR_BYTES];if((ret=exact(plan,buf,sizeof(buf))))goto done;
   struct tensor *t=&g->t[i];t->type=u32(buf);t->shape.rank=u32(buf+4);t->flags=u32(buf+8);
   if(!width(t->type)||t->shape.rank>8||t->flags&~7u||(t->flags&CONST&&t->flags&INPUT)){ret=-EBADMSG;goto done;}
   t->count=1;for(unsigned d=0;d<8;d++){t->shape.dims[d]=u32(buf+12+d*4);
     if(d<t->shape.rank&&(!t->shape.dims[d]||!mul(t->count,t->shape.dims[d],&t->count))){ret=-EOVERFLOW;goto done;}}
   if(!mul(t->count,width(t->type),&t->bytes)||t->bytes>GLV_MEMORY_LIMIT*8u){ret=-EFBIG;goto done;}
   t->offset=u32(buf+44);t->stored=u32(buf+48);t->format=u32(buf+52);t->rows=u32(buf+56);t->columns=u32(buf+60);t->padded=u32(buf+64);
   if(t->flags&CONST){
     if(t->offset>data_bytes||t->stored>data_bytes-t->offset){ret=-EBADMSG;goto done;}
     unsigned fmt=t->format&255u;
     if(t->format&~259u||fmt>2||(!fmt&&(t->stored!=t->bytes||(t->format&256u)))){ret=-EBADMSG;goto done;}
     size_t logical;
     if(!t->rows||!t->columns||!mul(t->rows,t->columns,&logical)||logical!=t->count||
        ((t->format&256u)&&(t->shape.rank!=2||t->rows!=t->shape.dims[1]||t->columns!=t->shape.dims[0]))){ret=-EBADMSG;goto done;}
     if(fmt){size_t packed=0,rowbytes;
       if(t->type!=F32||!t->rows||!t->columns||t->padded<t->columns||t->padded%32||
          !mul(t->rows,t->columns,&logical)||logical!=t->count||!mul(t->padded/32,fmt==1?20u:36u,&rowbytes)||
          !mul(t->rows,rowbytes,&packed)||packed!=t->stored){ret=-EBADMSG;goto done;}
       if(rowbytes>g->row_bytes)g->row_bytes=rowbytes;
     }
   }else if(t->offset||t->stored||t->format){ret=-EBADMSG;goto done;}
 }
 /* uses doubles as producer state during validation, then is rebuilt. */
 for(unsigned i=0;i<g->nt;i++)if(g->t[i].flags&(CONST|INPUT))g->t[i].uses=1;
 for(unsigned i=0;i<g->nn;i++){
   unsigned char b[NODE_BYTES];if((ret=exact(plan,b,sizeof(b))))goto done;
   struct node *n=&g->n[i];n->op=u32(b);n->ni=u32(b+4);n->no=u32(b+8);
   if(!n->ni||n->ni>8||!n->no||n->no>4||n->op<COPY||n->op>GEMM){ret=-EBADMSG;goto done;}
   unsigned arity=(n->op==GATHER||n->op==GATHER_ELEMENTS||(n->op>=ADD&&n->op<=POW)||n->op==MATMUL)?2:1;
   if((n->op==CONV||n->op==GEMM)?(n->ni<2||n->ni>3):
      (n->op!=CONCAT&&n->ni!=arity)){ret=-EBADMSG;goto done;}
   if(n->op!=SPLIT&&n->no!=1){ret=-EBADMSG;goto done;}
   for(unsigned j=0;j<8;j++){n->in[j]=u32(b+12+j*4);if(j<n->ni&&(n->in[j]>=g->nt||!g->t[n->in[j]].uses)){ret=-EBADMSG;goto done;}}
   for(unsigned j=0;j<4;j++){n->out[j]=u32(b+44+j*4);if(j<n->no){if(n->out[j]>=g->nt||g->t[n->out[j]].uses){ret=-EBADMSG;goto done;}g->t[n->out[j]].uses=1;}}
   for(unsigned j=0;j<32;j++)n->p[j]=(int32_t)u32(b+60+j*4);n->a=f32(b+188);n->b=0;
   /* alpha/beta use p30/p31 raw float bits; tail reserved for format growth. */
   memcpy(&n->a,&n->p[30],4);memcpy(&n->b,&n->p[31],4);
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
 for(unsigned i=0;i<g->nn;i++){
   g->last=i;if((ret=glv_cancel(g->memory))||(ret=evaluate(g,&g->n[i])))goto done;
   struct node *n=&g->n[i];
   for(unsigned j=0;j<n->ni;j++){struct tensor *t=&g->t[n->in[j]];if(!--t->left)release(t);}
   for(unsigned j=0;j<n->no;j++){struct tensor *t=&g->t[n->out[j]];if(!t->left)release(t);}
 }
 for(unsigned i=0;i<no;i++){
   struct tensor *t=&g->t[g->outputs[i]];if((ret=glv_cancel(g->memory))||(ret=materialize(g,t)))goto done;
   memcpy(out[i].data,t->data,t->bytes);
 }
done:cleanup(g);return ret;
}
