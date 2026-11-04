#include "local_asr_accel.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <float.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct fixture {struct glv_asr_packed_rows view;unsigned reads;atomic_uint calls;unsigned cancel_after;};
static int read_row(void *p,unsigned r,float *y,unsigned k){(void)r;(void)y;(void)k;((struct fixture *)p)->reads++;return -EIO;}
static int prepare(void *p,unsigned r,float *y,int16_t *c,float *s,float *l,unsigned k){(void)c;(void)s;(void)l;return read_row(p,r,y,k);}
static int baseline_prepare(void *p,unsigned row,float *dest,int16_t *codes,float *scales,float *l1,unsigned k)
{
 struct fixture *f=p;const struct glv_asr_packed_rows *v=&f->view;unsigned groups=v->padded/32u,gb=v->format==1?20:36;
 for(unsigned g=0;g<groups;g++){
  const unsigned char *b=v->data+row*v->stride+g*gb;memcpy(scales+g,b,4);l1[g]=0;
  for(unsigned i=0;i<32;i++){
   unsigned col=g*32u+i;int code=0;
   if(col<k){if(v->format==1){unsigned nib=(b[4+i/2]>>(4u*(i&1u)))&15u;code=nib<8?(int)nib:(int)nib-16;}else code=(int8_t)b[4+i];}
   codes[col]=(int16_t)code;if(col<k){dest[col]=(float)code*scales[g];l1[g]+=fabsf(dest[col]);}
  }
 }
 return 1;
}
static int resident(void *p,struct glv_asr_packed_rows *v){*v=((struct fixture *)p)->view;return 1;}
static bool cancelled(void *p){struct fixture *f=p;unsigned n=atomic_fetch_add(&f->calls,1)+1;return f->cancel_after&&n>=f->cancel_after;}
static unsigned seed=121;
static unsigned rng(void){seed=seed*1664525u+1013904223u;return seed;}
static void run(unsigned fmt,unsigned rows,unsigned k,unsigned n,bool serial,bool cancel,bool corrupt)
{
 unsigned padded=(k+31u)&~31u,groups=padded/32u,gb=fmt==1?20:36;
 size_t stride=(size_t)groups*gb;
 unsigned char *raw=calloc(n,stride);float *w=calloc((size_t)n*k,4),*x=calloc((size_t)rows*k,4),*y=calloc((size_t)rows*n,4);
 assert(raw&&w&&x&&y);
 for(unsigned o=0;o<n;o++)for(unsigned g=0;g<groups;g++){
  unsigned char *b=raw+o*stride+g*gb;float scale=(float)(1+rng()%31u)/97.f;memcpy(b,&scale,4);
  for(unsigned i=0;i<32;i++){
   int code=(int)(rng()%(fmt==1?16:256))-(fmt==1?8:128);unsigned col=g*32u+i;
   if(fmt==1)b[4+i/2]|=(unsigned)(code&15)<<(4*(i&1u));else b[4+i]=(unsigned char)code;
   if(col<k)w[(size_t)o*k+col]=(float)code*scale;
  }
 }
 for(unsigned i=0;i<rows*k;i++)x[i]=(int)(rng()%40001u)/10007.f-2.f;
 if(corrupt){float bad=NAN;memcpy(raw,&bad,4);}
 struct fixture f={.view={raw,fmt,n,k,padded,stride},.cancel_after=cancel?17:0};atomic_init(&f.calls,0);
 struct glv_memory m={.limit=2u*1024u*1024u,.cancelled=cancelled,.opaque=&f,.asr_serial=serial};
 struct glv_asr_rows reader={.read=read_row,.opaque=&f,.prepare=prepare,.resident=resident};
 int ret=glv_asr_matmul(&m,x,(size_t)rows*k,y,(size_t)rows*n,rows,k,n,&reader);
 if(cancel)assert(ret==-ECANCELED);else if(corrupt)assert(ret==-EBADMSG);else{
  assert(!ret);
  assert(!f.reads);
  float *previous=calloc((size_t)rows*n,4);assert(previous);
  struct glv_asr_rows baseline={.read=read_row,.opaque=&f,.prepare=baseline_prepare};
  assert(!glv_asr_matmul(&m,x,(size_t)rows*k,previous,(size_t)rows*n,rows,k,n,&baseline));
  assert(!memcmp(y,previous,(size_t)rows*n*4));free(previous);
  for(unsigned r=0;r<rows;r++)for(unsigned o=0;o<n;o++){
   float ref=0,absolute=0;for(unsigned j=0;j<k;j++){float term=x[(size_t)r*k+j]*w[(size_t)o*k+j];ref+=term;absolute+=fabsf(term);}
   /* Scalar and existing group32 kernels have different FP32 reduction
    * order. Above requires bit identity to the previous accelerated path;
    * here also bound deviation by FP32 summation error, without fast-math. */
   assert(fabsf(y[(size_t)r*n+o]-ref)<=2e-6f+2e-5f*fabsf(ref)+2.f*k*FLT_EPSILON*absolute);
  }
 }
 assert(!f.reads);glv_asr_parallel_close(&m);assert(!m.used);
 free(raw);free(w);free(x);free(y);
 printf("RESIDENT_OP fmt=%u rows=%u k=%u n=%u serial=%u cancel=%u corrupt=%u result=%d clean=1\n",fmt,rows,k,n,serial,cancel,corrupt,ret);
}
int main(void)
{
 for(unsigned fmt=1;fmt<=2;fmt++){
  const unsigned ks[]={32,33,65,319,320,2432};
  for(unsigned i=0;i<6;i++)for(unsigned serial=0;serial<2;serial++)run(fmt,i==5?8:2,ks[i],i==5?160:19,serial,false,false);
  run(fmt,16,320,320,false,true,false);run(fmt,16,320,320,false,false,true);
 }
 puts("RESIDENT_OPS_OK cases=28");
}
