/* SPDX-License-Identifier: Apache-2.0 */
/* Compare both decoders with real stdio and precise short-read injection. */
#include "glass_local_model.h"
#include <assert.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static unsigned reads,checks,cases;static bool stop,short_read,seek_error;
static bool cancelled(void *unused){(void)unused;checks++;return stop;}
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *p,size_t s,size_t n,FILE *f)
{reads++;if(short_read){short_read=false;return __real_fread(p,s,n-1,f);}return __real_fread(p,s,n,f);}
int __real_fseek(FILE *,long,int);
int __wrap_fseek(FILE *f,long offset,int whence)
{if(seek_error){seek_error=false;return -1;}return __real_fseek(f,offset,whence);}
static void one(unsigned count,unsigned type,bool resident,float scale)
{
 struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled};
 struct glv_tensor tensor={.name="row",.ndim=2,.shape={3,count},.type=type,.offset=16};
 size_t row_bytes=type==2?count+4u:type==1?count/32u*20u:count*4u;
 tensor.stride=type?row_bytes:0;tensor.bytes=row_bytes*3;
 size_t bytes=16+tensor.bytes;unsigned char *data=calloc(1,bytes);assert(data);
 for(unsigned row=0;row<3;row++){
  unsigned char *p=data+16+row*row_bytes;
  if(type==2){memcpy(p,&scale,4);for(unsigned i=0;i<count;i++)p[4+i]=(unsigned char)(i*37u+row*61u);}
  else if(type==1){for(unsigned g=0;g<count/32;g++){memcpy(p+g*20,&scale,4);for(unsigned j=4;j<20;j++)p[g*20+j]=(unsigned char)(j+g+row);}}
  else {for(unsigned i=0;i<count;i++){float v=(float)((int)(i%131)-65)*scale;memcpy(p+4*i,&v,4);}}
 }
 FILE *file=tmpfile();assert(file&&fwrite(data,1,bytes,file)==bytes&&!fflush(file));
 struct glv_model model={.memory=&memory,.bytes=bytes,.file=file,.cursor=SIZE_MAX,.data=resident?data:NULL};
 size_t cap=(size_t)count*4;unsigned char *a=malloc(cap+32),*b=malloc(cap+32);assert(a&&b);
 for(unsigned row=0;row<3;row++){
  memset(a,0xa7,cap+32);memset(b,0xa7,cap+32);
  model.bulk_q8=false;unsigned before=reads;
  int legacy=glv_tensor_row(&model,&tensor,row,(float *)(a+16),count);unsigned old_reads=reads-before;
  model.bulk_q8=true;before=reads;
  int bulk=glv_tensor_row(&model,&tensor,row,(float *)(b+16),count);unsigned new_reads=reads-before;
  assert(legacy==bulk);if(!legacy)assert(!memcmp(a+16,b+16,cap));
  for(unsigned i=0;i<16;i++){assert(a[i]==0xa7&&b[i]==0xa7);assert(a[cap+16+i]==0xa7&&b[cap+16+i]==0xa7);}
  if(type==2&&count>=2&&count<=4092&&!resident){assert(new_reads==1);if(!legacy)assert(old_reads>=2);}
  stop=true;before=reads;assert(glv_tensor_row(&model,&tensor,row,(float *)(b+16),count)==-ECANCELED&&reads==before);stop=false;
  cases++;
 }
 if(!resident){
  for(unsigned mode=0;mode<2;mode++){
   model.bulk_q8=mode;model.cursor=SIZE_MAX;short_read=true;
   assert(glv_tensor_row(&model,&tensor,0,(float *)(b+16),count)==-EIO&&model.cursor==SIZE_MAX);
   seek_error=true;unsigned before=reads;
   assert(glv_tensor_row(&model,&tensor,0,(float *)(b+16),count)==-EIO&&reads==before);
  }
 }
 assert(glv_tensor_row(&model,&tensor,3,(float *)(b+16),count)==-EINVAL);
 fclose(file);free(a);free(b);free(data);assert(!memory.used);
}
int main(void)
{
 const unsigned sizes[]={1,2,3,4,7,31,32,33,255,256,257,511,1023,1024,2288,4092,4093,8192};
 const float scales[]={1.f,.00003125f,1.234567f,FLT_MIN,FLT_MAX,-1.f,0.f,INFINITY,NAN};
 for(unsigned n=0;n<sizeof(sizes)/sizeof(sizes[0]);n++)for(unsigned s=0;s<sizeof(scales)/sizeof(scales[0]);s++)for(unsigned resident=0;resident<2;resident++)one(sizes[n],2,resident,scales[s]);
 for(unsigned type=0;type<2;type++)for(unsigned resident=0;resident<2;resident++)one(64,type,resident,.125f);
 printf("TTS_ROWS_EXACT_CANARY_IO_CANCEL_OK cases=%u cancel_checks=%u\n",cases,checks);return 0;
}
