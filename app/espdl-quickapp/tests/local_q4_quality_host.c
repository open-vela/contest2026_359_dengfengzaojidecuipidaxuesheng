/* SPDX-License-Identifier: Apache-2.0 */
/* Real model/LLM, optional quantized-activation matrix replacement only. */
#include "glass_local_llm.h"
#include "local_q4_pie.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static unsigned bits, calls;
int __real_glv_matvec(struct glv_model *,const char *,const float *,float *,unsigned,unsigned);
int __real_glv_matbatch(struct glv_model *,const char *,const float *,float *,unsigned,unsigned,unsigned);
int __wrap_glv_matvec(struct glv_model *m,const char *n,const float *x,float *y,unsigned r,unsigned c)
{
  if(bits&&glv_tensor(m,n)->type==1){calls++;return glv_q4_matbatch(m,n,x,y,r,c,1,bits);}
  return __real_glv_matvec(m,n,x,y,r,c);
}
int __wrap_glv_matbatch(struct glv_model *m,const char *n,const float *x,float *y,unsigned r,unsigned c,unsigned b)
{
  if(bits&&glv_tensor(m,n)->type==1){calls++;return glv_q4_matbatch(m,n,x,y,r,c,b,bits);}
  return __real_glv_matbatch(m,n,x,y,r,c,b);
}
int main(int argc,char **argv)
{
  assert(argc==7);
  bits=(unsigned)atoi(argv[1]);assert(bits==0||bits==8||bits==16);
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.q4_bits=bits};struct glv_llm *llm=NULL;
  assert(!glv_llm_open(argv[2],&memory,&llm));
  char prompt[8192]={0};FILE *f=fopen(argv[3],"rb");assert(f);
  size_t n=fread(prompt,1,sizeof(prompt)-1,f);assert(n&&feof(f));fclose(f);
  unsigned count=(unsigned)atoi(argv[6]);
  float logits[8192];char reply[4096];int ret;
  clock_t began=clock();
  if(!strcmp(argv[4],"prefill")){
    int ids[512];int tokens=glv_llm_encode(llm,prompt,ids,512);assert(tokens>0);
    ret=glv_llm_prefill(llm,ids,(unsigned)tokens,logits);
    assert(!ret);f=fopen(argv[5],"wb");assert(f);
    assert(fwrite(logits,sizeof(float),8192,f)==8192);
  }else if(!strcmp(argv[4],"context")){
    int ids[512];assert(count&&count<=512);
    for(unsigned i=0;i<count;i++)ids[i]=(i*7919u+31u)%8192u;
    ret=glv_llm_prefill(llm,ids,count,logits);assert(!ret);
    f=fopen(argv[5],"wb");assert(f);assert(fwrite(logits,sizeof(float),8192,f)==8192);
  }else{
    assert(!strcmp(argv[4],"generate"));
    ret=glv_llm_generate(llm,prompt,count,reply,sizeof(reply),NULL,NULL);assert(ret>=0);
    f=fopen(argv[5],"wb");assert(f);assert(fwrite(reply,1,ret,f)==(size_t)ret);
  }
  fclose(f);glv_llm_close(llm);assert(!memory.used);assert(!bits||calls);
  printf("{\"bits\":%u,\"matrix_calls\":%u,\"cpu_ms\":%.3f,\"peak_bytes\":%zu,\"remaining_bytes\":%zu}\n",
    bits,calls,1000.0*(clock()-began)/CLOCKS_PER_SEC,memory.peak,memory.used);
  return 0;
}
