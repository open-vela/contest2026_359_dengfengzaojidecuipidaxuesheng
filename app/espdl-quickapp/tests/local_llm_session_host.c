/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_llm.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static void clean(char *out,size_t capacity,const char *text)
{
  const unsigned char *p=(const unsigned char *)text,*end=p+strlen(text);size_t n=0;
  while(p<end){const unsigned char *before=p;uint32_t cp;
    if(glv_utf8_read(&p,end,&cp)){p=before+1;cp=0xfffd;}
    unsigned char data[4];unsigned bytes=glv_utf8_write(cp,data);assert(n+bytes<capacity);
    memcpy(out+n,data,bytes);n+=bytes;
  }out[n]=0;
}
int main(int argc,char **argv)
{
  assert(argc==4);bool warm=!strcmp(argv[2],"warm");FILE *out=fopen(argv[3],"wb");assert(out);
  struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.reuse_kv=warm};struct glv_llm *llm=NULL;
  char history[8192]={0},prompt[10000],reply[4096],safe[4096];
  const char *inputs[]={"你好","1加1等于几？","为什么要节约用水？","a\n你好😀。"};
  for(unsigned i=0;i<4;i++){
    if(!llm)assert(!glv_llm_open(argv[1],&m,&llm));
    m.peak=m.used;memset(&m.profile,0,sizeof(m.profile));
    snprintf(prompt,sizeof(prompt),"%s<|im_start|>user\n%s<|im_end|>\n<|im_start|>assistant\n",history,inputs[i]);
    int count=glv_llm_generate(llm,prompt,8,reply,sizeof(reply),NULL,NULL);assert(count>=0);
    clean(safe,sizeof(safe),reply);uint32_t bytes=(uint32_t)strlen(safe);
    assert(fwrite(&bytes,4,1,out)==1&&fwrite(safe,1,bytes,out)==bytes);
    printf("SESSION_VECTOR turn=%u reused=%u prefill=%u sha=%llu weight=%llu used=%zu peak=%zu\n",i,m.profile.kv_reused_tokens,
      m.profile.prefill_tokens,(unsigned long long)m.profile.sha_bytes,(unsigned long long)m.profile.weight_bytes,m.used,m.peak);
    if(warm&&i)assert(m.profile.kv_reused_tokens>0);
    snprintf(history,sizeof(history),"%s%s<|im_end|>\n",prompt,safe);
    if(!warm){glv_llm_close(llm);llm=NULL;assert(!m.used);}
  }
  /* A changed beginning rewinds the prefix and old future KV stays invisible. */
  if(!llm)assert(!glv_llm_open(argv[1],&m,&llm));
  int n=glv_llm_generate(llm,"<|im_start|>user\n测试<|im_end|>\n<|im_start|>assistant\n",8,reply,sizeof(reply),NULL,NULL);
  assert(n>=0);clean(safe,sizeof(safe),reply);uint32_t bytes=(uint32_t)strlen(safe);
  assert(fwrite(&bytes,4,1,out)==1&&fwrite(safe,1,bytes,out)==bytes);
  glv_llm_close(llm);assert(!m.used);fclose(out);puts("LLM_SESSION_RECLAIMED");return 0;
}
