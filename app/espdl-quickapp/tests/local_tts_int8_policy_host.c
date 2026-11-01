/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_int8_policy.h"
#include "glass_local_assets.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
static unsigned reads,fail_read;
size_t __real_fread(void *,size_t,size_t,FILE *);
size_t __wrap_fread(void *p,size_t n,size_t c,FILE *f)
{if(fail_read&&++reads==fail_read)return 0;return __real_fread(p,n,c,f);}
static unsigned char original[8192];static size_t original_size;
static bool cancel(void *arg){(void)arg;return true;}
static void put32(unsigned char *p,unsigned n){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(n>>(8*i));}
static void publish(const unsigned char *data,size_t bytes,bool valid_digest)
{
 char path[512];snprintf(path,sizeof(path),"%s/policy.bin",GLV_TTS_INT8_POLICY_ROOT);FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,bytes,f)==bytes&&!fclose(f));
 unsigned char h[16]={0},entry[132]={0},digest[32];memcpy(h,"GLV1",4);put32(h+4,GLV_ABI);put32(h+8,1);strcpy((char *)entry,"policy.bin");put32(entry+96,bytes);
 struct glv_memory m={.limit=GLV_MEMORY_LIMIT};assert(!glv_sha_buffer(data,bytes,digest,&m));if(!valid_digest)digest[0]^=1;memcpy(entry+100,digest,32);
 snprintf(path,sizeof(path),"%s/manifest.bin",GLV_TTS_INT8_POLICY_ROOT);f=fopen(path,"wb");assert(f);assert(fwrite(h,1,16,f)==16&&fwrite(entry,1,132,f)==132&&!fclose(f));
}
int main(void)
{
 char path[512];snprintf(path,sizeof(path),"%s/policy.bin",GLV_TTS_INT8_POLICY_ROOT);FILE *f=fopen(path,"rb");assert(f);original_size=fread(original,1,sizeof(original),f);fclose(f);assert(original_size==700);
 struct glv_memory m={.limit=GLV_MEMORY_LIMIT};struct glv_tensor tensors[8]={0};for(unsigned i=0;i<8;i++){strcpy(tensors[i].name,(const char *)original+60+i*80);tensors[i].type=2;tensors[i].ndim=3;tensors[i].shape[0]=tensors[i].shape[1]=tensors[i].shape[2]=1;}
 struct glv_model model={.memory=&m,.tensors=tensors,.count=8};unsigned tests=0;
 for(unsigned kind=0;kind<22;kind++){
  unsigned char data[8192];memcpy(data,original,original_size);size_t bytes=original_size;bool valid=true;memcpy(m.tts_base_digest,original+16,32);m.limit=GLV_MEMORY_LIMIT;m.cancelled=NULL;
  if(kind==1)valid=false;
  if(kind==2)data[0]^=1;
  if(kind==3)put32(data+8,2);
  if(kind==4)put32(data+12,99);
  if(kind==5)data[16]^=1;
  if(kind==6)put32(data+48,3);
  if(kind==7)put32(data+56,1);
  if(kind==8)put32(data+52,0);
  if(kind==9)put32(data+52,75);
  if(kind==10){data[bytes++]=0;}
  if(kind==11)memset(data+60,'x',80);
  if(kind==12)memcpy(data+140,data+60,80);
  if(kind==13){memset(data+60,0,80);strcpy((char *)data+60,"vocoder.unknown.weight");}
  if(kind==14){memset(data+60,0,80);strcpy((char *)data+60,"vocoder.upsamples.0.1.weight");}
  if(kind==15)tensors[0].type=0;
  if(kind==16)tensors[0].ndim=2;
  if(kind==17)tensors[0].shape[0]=209;
  if(kind==18)tensors[0].shape[1]=0;
  if(kind==19)tensors[0].shape[2]=12;
  if(kind==20)m.limit=32;
  if(kind==21)m.cancelled=cancel;
  publish(data,bytes,valid);int ret=glv_tts_int8_policy_load(&model);
  if(!kind){
   assert(!ret&&m.tts_int8_policy&&m.used);for(unsigned i=0;i<8;i++)assert(glv_tts_int8_precision(&model,&tensors[i])==16);
   struct glv_tensor other={0};strcpy(other.name,"vocoder.blocks.0.convs1.0.1.weight");assert(glv_tts_int8_precision(&model,&other)==8);
   assert(glv_tts_int8_policy_load(&model)==-EBUSY);glv_tts_int8_policy_close(&m);
  }else assert(ret<0&&!m.tts_int8_policy);
  assert(!m.used);tensors[0].type=2;tensors[0].ndim=3;tensors[0].shape[0]=tensors[0].shape[1]=tensors[0].shape[2]=1;tests++;
 }
 publish(original,original_size,true);m.limit=GLV_MEMORY_LIMIT;m.cancelled=NULL;
 for(unsigned at=1;at<=3;at++){memcpy(m.tts_base_digest,original+16,32);reads=0;fail_read=at;assert(glv_tts_int8_policy_load(&model)<0&&!m.used&&!m.tts_int8_policy);fail_read=0;tests++;}
 printf("POLICY_VERIFIED_SNAPSHOT_SHA_BASE_ABI_SHAPE_COUNT_ALLOC_READ_CANCEL_CLEANUP_OK cases=%u\n",tests);return 0;
}
