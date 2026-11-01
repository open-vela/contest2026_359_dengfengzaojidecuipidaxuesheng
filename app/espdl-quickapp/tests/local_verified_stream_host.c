/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise actual SHA/manifest/model code, including retained FD ownership. */
#include "glass_local_assets.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static bool stop;
static bool cancelled(void *unused){(void)unused;return stop;}
static size_t reads;
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *p,size_t s,size_t n,FILE *f)
{size_t r=__real_fread(p,s,n,f);reads+=r*s;return r;}
int main(int argc,char **argv)
{
  assert(argc==3);const char *root=argv[1],*which=argv[2];
  struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled};
  memory.retain_verified=strcmp(which,"baseline")!=0;
  char failed[96],path[512],modelroot[512];
  snprintf(path,sizeof(path),"%s/edgeidiot/weights.bin",root);
  snprintf(modelroot,sizeof(modelroot),"%s/edgeidiot",root);
  if(!strcmp(which,"budget"))memory.limit=16000;
  if(!strcmp(which,"corrupt")){FILE *f=fopen(path,"r+b");assert(f);assert(fputc(42,f)!=EOF);fclose(f);}
  int ret=glv_assets_check(root,&memory,failed,sizeof(failed));
  if(!strcmp(which,"budget")||!strcmp(which,"corrupt")){
    assert(ret==(!strcmp(which,"budget")?-ENOMEM:-EBADMSG));
    assert(!memory.verified&&!memory.used);puts("REJECTED_AND_RECLAIMED");return 0;
  }
  assert(!ret);size_t sha_before=memory.profile.sha_bytes,reads_before=reads;
  if(!strcmp(which,"cancel"))stop=true;
  if(!strcmp(which,"truncate")){assert(!truncate(path,4));}
  if(!strcmp(which,"missing"))assert(!unlink(path));
  if(!strcmp(which,"replace")){
    char moved[512];snprintf(moved,sizeof(moved),"%s/edgeidiot/old.bin",root);assert(!rename(path,moved));
    FILE *in=fopen(moved,"rb"),*out=fopen(path,"wb");assert(in&&out);int c;while((c=fgetc(in))!=EOF)assert(fputc(c,out)!=EOF);fclose(in);fclose(out);
  }
  if(!strcmp(which,"index")){
    char index[512];snprintf(index,sizeof(index),"%s/model.bin",modelroot);FILE *f=fopen(index,"r+b");assert(f);assert(!fseek(f,16,SEEK_SET));int c=fgetc(f);assert(c!=EOF);assert(!fseek(f,16,SEEK_SET));assert(fputc(c^1,f)!=EOF);fclose(f);
  }
  struct glv_model model;ret=glv_model_open(&model,modelroot,&memory,false);
  if(!strcmp(which,"cancel")||!strcmp(which,"truncate")||!strcmp(which,"missing")||!strcmp(which,"replace")||!strcmp(which,"index")){
    assert(ret<0);if(stop)assert(ret==-ECANCELED);assert(!memory.profile.verified_reuses);
  }else{
    assert(!ret);assert(memory.profile.verified_reuses==(!strcmp(which,"baseline")?0:1));
    assert(memory.profile.sha_bytes-sha_before==(!strcmp(which,"baseline")?40:0));
    assert(reads-reads_before==164+(!strcmp(which,"baseline")?40:0));
    float input[32],out[2];for(unsigned i=0;i<32;i++)input[i]=1;
    assert(!glv_matvec(&model,"sample.weight",input,out,2,32));assert(out[0]==32&&out[1]==64);
    glv_model_close(&model);
    if(!strcmp(which,"reuse")){
      snprintf(modelroot,sizeof(modelroot),"%s/tts-v2",root);
      assert(!glv_model_open(&model,modelroot,&memory,true));assert(memory.profile.verified_reuses==2);
      assert(!glv_matvec(&model,"sample.weight",input,out,2,32));assert(out[0]==32&&out[1]==64);
      glv_model_close(&model);
    }
  }
  glv_verified_clear(&memory);assert(!memory.used&&!memory.verified);
  printf("VERIFIED_STREAM_OK case=%s sha_bytes=%llu reuse=%u read_bytes=%lu\n",which,
    (unsigned long long)memory.profile.sha_bytes,memory.profile.verified_reuses,(unsigned long)reads);
  return 0;
}
