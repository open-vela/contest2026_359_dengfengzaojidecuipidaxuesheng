/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_model.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static unsigned reads,seeks,fail_read,fail_seek,polls,stop_at;
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *p,size_t n,size_t size,FILE *f)
{reads++;if(fail_read==reads){errno=EIO;return 0;}return __real_fread(p,n,size,f);}
int __real_fseek(FILE *,long,int);
int __wrap_fseek(FILE *f,long at,int whence)
{seeks++;if(fail_seek==seeks){errno=EIO;return -1;}return __real_fseek(f,at,whence);}
static bool cancel(void *unused){(void)unused;return stop_at&&++polls>=stop_at;}
static void fresh(struct glv_model *m)
{m->cursor=SIZE_MAX;reads=seeks=polls=fail_read=fail_seek=stop_at=0;}
int main(void)
{
  const unsigned counts[]={1,2,255,256,257,512,513,2288,4092,4093};
  for(unsigned z=0;z<10;z++)for(unsigned bulk=0;bulk<2;bulk++){
    unsigned n=counts[z],stride=n+4;unsigned char *data=malloc(2*stride),*raw=malloc(stride);
    float *ref=malloc(n*4),*decoded=malloc(n*4);assert(data&&raw&&ref&&decoded);
    for(unsigned row=0;row<2;row++){
      float scale=.0013f*(row+1);memcpy(data+row*stride,&scale,4);
      for(unsigned i=0;i<n;i++)data[row*stride+4+i]=(unsigned char)(i*73+row*19);
    }
    struct glv_memory mem={.limit=GLV_MEMORY_LIMIT,.cancelled=cancel};
    struct glv_tensor t={.name="q8",.shape={2,n},.ndim=2,.type=2,.stride=stride,.bytes=2*stride};
    FILE *f=tmpfile();assert(f);assert(fwrite(data,1,2*stride,f)==2*stride);fflush(f);
    struct glv_model m={.memory=&mem,.file=f,.bytes=2*stride,.bulk_q8=bulk,.tensors=&t,.count=1};
    fresh(&m);assert(!glv_tensor_row(&m,&t,1,ref,n));unsigned normal_reads=reads,normal_seeks=seeks;
    fresh(&m);assert(!glv_tensor_q8_packed(&m,&t,1,raw,stride,n));assert(reads==normal_reads&&seeks==normal_seeks);
    assert(!memcmp(raw,data+stride,stride));float scale;memcpy(&scale,raw,4);
    for(unsigned i=0;i<n;i++)decoded[i]=scale*(int8_t)raw[4+i];assert(!memcmp(decoded,ref,n*4));
    for(unsigned at=1;at<=normal_reads;at++){
      fresh(&m);fail_read=at;assert(glv_tensor_row(&m,&t,1,ref,n)==-EIO&&reads==at&&m.cursor==SIZE_MAX);
      fresh(&m);fail_read=at;assert(glv_tensor_q8_packed(&m,&t,1,raw,stride,n)==-EIO&&reads==at&&m.cursor==SIZE_MAX);
    }
    fresh(&m);fail_seek=1;assert(glv_tensor_q8_packed(&m,&t,1,raw,stride,n)==-EIO&&seeks==1&&!reads);
    fresh(&m);stop_at=1;assert(glv_tensor_q8_packed(&m,&t,1,raw,stride,n)==-ECANCELED&&!reads);
    fresh(&m);assert(glv_tensor_q8_packed(&m,&t,2,raw,stride,n)==-EINVAL&&!reads);
    assert(glv_tensor_q8_packed(&m,&t,0,raw,stride-1,n)==-EINVAL&&!reads);
    assert(glv_tensor_q8_packed(&m,&t,0,raw,stride,n-1)==-EINVAL&&!reads);
    t.type=1;assert(glv_tensor_q8_packed(&m,&t,0,raw,stride,n)==-ENOTSUP&&!reads);t.type=2;
    if(bulk&&n>=2&&n<=4092){
      fresh(&m);assert(!glv_model_cache_tensor(&m,"q8"));assert(!glv_tensor_row(&m,&t,0,ref,n));
      size_t cursor=m.cursor;reads=seeks=0;
      assert(!glv_tensor_q8_packed(&m,&t,0,raw,stride,n)&&!reads&&!seeks&&m.cursor==cursor);
      assert(!memcmp(raw,data,stride));stop_at=1;polls=0;
      assert(glv_tensor_q8_packed(&m,&t,0,raw,stride,n)==-ECANCELED&&!reads);stop_at=0;
      glv_model_cache_clear(&m);assert(!mem.used);
    }
    fclose(f);m.file=NULL;m.data=data;
    const float bad[]={0,-0.f,-1,INFINITY,NAN};
    for(unsigned i=0;i<5;i++){
      memcpy(data,&bad[i],4);fresh(&m);
      assert(glv_tensor_row(&m,&t,0,ref,n)==-EBADMSG);
      assert(glv_tensor_q8_packed(&m,&t,0,raw,stride,n)==-EBADMSG);
    }
    free(data);free(raw);free(ref);free(decoded);
  }
  puts("PACKED_Q8_EXACT_IO_COUNTS_READ_SEEK_FAULT_CANCEL_CACHE_INVALID_SCALE_NO_REPLAY_OK");return 0;
}
