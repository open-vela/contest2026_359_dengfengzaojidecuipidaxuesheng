/* SPDX-License-Identifier: Apache-2.0 */
/* Exact row/position ordering, tails and error writes for scalar matbatch. */
#include "glass_local_model.h"
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <string.h>

#define ROWS 3u
#define COLS 37u
static float weights[ROWS*COLS],input[64*COLS],actual[64*ROWS],expected[64*ROWS];

static int oracle(unsigned batch)
{
  for(unsigned row=0;row<ROWS;row++){
    for(unsigned c=0;c<COLS;c++)if(!glv_finite(weights[row*COLS+c]))return -EBADMSG;
    for(unsigned b=0;b<batch;b++){
      float sum=0;
      for(unsigned c=0;c<COLS;c++)sum+=weights[row*COLS+c]*input[b*COLS+c];
      expected[b*ROWS+row]=sum;
      if(!glv_finite(sum))return -ERANGE;
    }
  }
  return 0;
}

int main(void)
{
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT};
  struct glv_tensor tensor={.name="test",.ndim=2,.shape={ROWS,COLS},.bytes=sizeof(weights),.stride=COLS*sizeof(float)};
  struct glv_model model={.memory=&memory,.tensors=&tensor,.count=1,.bytes=sizeof(weights),.data=(unsigned char *)weights};
  unsigned cases=0;
  for(unsigned batch=1;batch<=64;batch++)for(unsigned fault=0;fault<5;fault++){
    for(unsigned i=0;i<ROWS*COLS;i++)weights[i]=(float)((int)(i*17%39)-19)/8;
    for(unsigned i=0;i<64*COLS;i++)input[i]=ldexpf((float)((int)(i*7919%37)-18),(int)(i%13)-6);
    /* Preserve cancellation-sensitive ascending accumulation, signed zero,
     * non-multiple-of-four tails and the first invalid position's output. */
    if(fault==1)input[(batch-1)*COLS+7]=NAN;
    if(fault==2){weights[0]=2;input[(batch/2)*COLS]=3e38f;}
    if(fault==3)weights[COLS+9]=INFINITY;
    if(fault==4)for(unsigned i=0;i<batch*COLS;i++)input[i]=-0.f;
    for(unsigned i=0;i<64*ROWS;i++)actual[i]=expected[i]=-1234.5f;
    int want=oracle(batch);
    int got=glv_matbatch(&model,"test",input,actual,ROWS,COLS,batch);
    assert(got==want&&memory.used==0);
    assert(!memcmp(actual,expected,sizeof(actual)));
    cases++;
  }
  assert(glv_matbatch(&model,"test",input,actual,ROWS,COLS,0)==-EINVAL);
  assert(glv_matbatch(&model,"test",input,actual,ROWS,COLS,65)==-EINVAL);
  assert(memory.used==0);
  printf("MATBATCH_ORDER_OK cases=%u\n",cases);
  return 0;
}
