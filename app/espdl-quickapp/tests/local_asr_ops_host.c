/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr_ops.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
static unsigned checks,cancel_at;
static int read_error;
static bool cancelled(void *unused){(void)unused;return cancel_at&&++checks>=cancel_at;}
static int row(void *unused,unsigned o,float *data,unsigned n)
{
  (void)unused;(void)o;if(read_error)return read_error;
  assert(n<=4);for(unsigned i=0;i<n;i++)data[i]=(float)i+1;return 0;
}
int main(void)
{
  struct glv_memory memory={.limit=1024*1024,.cancelled=cancelled};struct glv_asr_rows rows={row,NULL};
  float x[]={1,2,3,4,5,6,7,8,9},out[9]={0},bias[]={1};
  assert(!glv_asr_matmul(&memory,x,9,out,9,1,3,2,&rows)&&out[0]==14&&out[1]==14&&!memory.used);
  assert(glv_asr_matmul(&memory,x,2,out,9,1,3,2,&rows)==-EINVAL);
  assert(glv_asr_matmul(&memory,x,9,x,9,1,3,2,&rows)==-EINVAL);
  struct glv_asr_conv conv={.channels=1,.height=3,.width=3,.outputs=1,.kernel_h=2,.kernel_w=2,.groups=1,.stride_h=1,.stride_w=1,.dilation_h=1,.dilation_w=1};
  assert(!glv_asr_conv(&memory,x,9,bias,1,out,9,&conv,&rows)&&out[0]==38&&out[3]==78&&!memory.used);
  assert(glv_asr_conv(&memory,x,9,bias,0,out,9,&conv,&rows)==-EINVAL);
  assert(glv_asr_conv(&memory,x,9,NULL,0,out,3,&conv,&rows)==-EINVAL);
  read_error=-EIO;
  assert(glv_asr_matmul(&memory,x,9,out,9,1,3,2,&rows)==-EIO&&!memory.used);
  assert(glv_asr_conv(&memory,x,9,NULL,0,out,9,&conv,&rows)==-EIO&&!memory.used);read_error=0;
  cancel_at=2;checks=0;
  assert(glv_asr_matmul(&memory,x,9,out,9,1,3,2,&rows)==-ECANCELED&&!memory.used);checks=0;
  assert(glv_asr_conv(&memory,x,9,NULL,0,out,9,&conv,&rows)==-ECANCELED&&!memory.used);cancel_at=0;
  memory.limit=1;
  assert(glv_asr_matmul(&memory,x,9,out,9,1,3,2,&rows)==-ENOMEM&&!memory.used);
  assert(glv_asr_conv(&memory,x,9,NULL,0,out,9,&conv,&rows)==-ENOMEM&&!memory.used);
  conv.height=UINT_MAX;
  assert(glv_asr_conv(&memory,x,9,NULL,0,out,9,&conv,&rows)==-EOVERFLOW&&!memory.used);
  memory.limit=1024*1024;
  struct glv_asr_shape sx={2,{2,3}},scalar={0,{0}},bad={9,{0}},shorter={1,{2}};
  float two=2;
  assert(!glv_asr_gemm(&memory,x,9,out,9,1,3,2,&rows,.5f,2,&two,1,&scalar)&&out[0]==11&&out[1]==11);
  assert(!glv_asr_unary(&memory,GLV_ASR_NEG,x,out,9)&&out[0]==-1&&out[8]==-9);
  assert(!glv_asr_binary(&memory,GLV_ASR_MUL,x,9,&sx,&two,1,&scalar,out,9)&&out[0]==2&&out[5]==12);
  assert(glv_asr_binary(&memory,GLV_ASR_ADD,x,9,&sx,x,9,&shorter,out,9)==-EINVAL);
  assert(glv_asr_binary(&memory,GLV_ASR_ADD,x,9,&bad,&two,1,&scalar,out,9)==-EINVAL);
  assert(!glv_asr_reduce(&memory,x,9,&sx,2,true,out,9)&&out[0]==2&&out[1]==5);
  assert(glv_asr_reduce(&memory,x,9,&sx,4,false,out,9)==-EINVAL);
  assert(!glv_asr_softmax(&memory,x,9,&sx,1,out,9)&&out[2]>out[1]&&out[1]>out[0]);
  assert(glv_asr_softmax(&memory,x,9,&sx,2,out,9)==-EINVAL);
  cancel_at=1;checks=0;assert(glv_asr_unary(&memory,GLV_ASR_NEG,x,out,9)==-ECANCELED);cancel_at=0;
  puts("ASR_OPS_LIFECYCLE_OK bounds/alias/cancel/SD-error/OOM/overflow/free");return 0;
}
