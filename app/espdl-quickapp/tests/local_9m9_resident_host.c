/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_llm.h"
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define WEIGHT_BYTES 10046336u
static bool deny_reads,tamper;
static size_t reads_after_load,weight_seen;
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *data,size_t size,size_t count,FILE *file)
{
  if(deny_reads){reads_after_load++;return 0;}
  size_t got=__real_fread(data,size,count,file);struct stat st;
  if(!fstat(fileno(file),&st)&&(size_t)st.st_size==WEIGHT_BYTES){
    if(tamper&&weight_seen>=WEIGHT_BYTES&&got&&size){((unsigned char *)data)[0]^=1;tamper=false;}
    weight_seen+=got*size;
  }return got;
}
static bool stopped(void *unused){(void)unused;return true;}
int main(int argc,char **argv)
{
  assert(argc==3);struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.reuse_kv=true};struct glv_llm *llm=NULL;
  assert(!glv_llm_open(argv[1],&memory,&llm));assert(glv_llm_resident_bytes(llm)==WEIGHT_BYTES);
  size_t idle=memory.used;float a[8192],b[8192];deny_reads=true;
  assert(!glv_llm_forward(llm,1,a));assert(glv_llm_kv_bytes(llm)==2u*1024u*1024u);
  size_t hot=memory.used;assert(glv_llm_trim_kv(llm)>=2u*1024u*1024u);assert(memory.used==idle);
  assert(glv_llm_resident_bytes(llm)==WEIGHT_BYTES&&!glv_llm_kv_bytes(llm));
  assert(!glv_llm_forward(llm,1,b));assert(!memcmp(a,b,sizeof(a)));assert(!reads_after_load);
  deny_reads=false;glv_llm_trim_kv(llm);
  FILE *f=fopen(argv[2],"rb");assert(f);char prompt[8193];size_t n=fread(prompt,1,8192,f);assert(!ferror(f));fclose(f);prompt[n]=0;
  deny_reads=true;char reply[4096];int tokens=glv_llm_generate_tool(llm,prompt,24,reply,sizeof(reply),NULL,NULL);assert(tokens>=0&&reply[0]);assert(!reads_after_load);
  printf("RESIDENT_9M9 weight_bytes=%u idle_bytes=%zu hot_bytes=%zu peak_bytes=%zu weight_reads_after_open=%zu tokens=%d reply=%s\n",WEIGHT_BYTES,idle,hot,memory.peak,reads_after_load,tokens,reply);
  deny_reads=false;glv_llm_close(llm);llm=NULL;assert(!memory.used);
  memory=(struct glv_memory){.limit=WEIGHT_BYTES-1024};assert(glv_llm_open(argv[1],&memory,&llm)==-ENOMEM);assert(!llm&&!memory.used);
  memory=(struct glv_memory){.limit=GLV_MEMORY_LIMIT,.cancelled=stopped};assert(glv_llm_open(argv[1],&memory,&llm)==-ECANCELED);assert(!llm&&!memory.used);
  memory=(struct glv_memory){.limit=GLV_MEMORY_LIMIT};weight_seen=0;tamper=true;
  assert(glv_llm_open(argv[1],&memory,&llm)==-EBADMSG);assert(!llm&&!memory.used&&!tamper);
  puts("RESIDENT_TRIM_RELOAD_EQUIVALENT_NO_SD_LOW_MEMORY_CANCEL_TAMPER_RECLAIM_OK");return 0;
}
