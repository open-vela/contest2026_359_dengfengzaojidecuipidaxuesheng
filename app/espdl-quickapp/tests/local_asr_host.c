/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct cancel {unsigned calls,stop;};
static bool cancelled(void *opaque)
{struct cancel *c=opaque;return c->stop&&++c->calls>=c->stop;}
struct callback {struct glv_asr *asr;int seen;};
static int reject(void *opaque,unsigned token,unsigned frame)
{
 struct callback *c=opaque;assert(token>0&&token!=2);(void)frame;char out[8];
 assert(glv_asr_transcribe(c->asr,NULL,0,out,sizeof(out),NULL,NULL,NULL)==-EBUSY);
 c->seen++;return -123;
}
int main(int argc,char **argv)
{
 assert(argc==3||argc==4);struct cancel c={0,0};struct glv_memory mem={0,0,GLV_MEMORY_LIMIT,cancelled,&c};
 struct glv_asr *a=NULL;int ret=glv_asr_open(&a,argv[1],&mem);
 if(argc==4){assert(ret<0&&!a&&!mem.used);puts("ASR_ENGINE_BAD_ASSET_REJECTED");return 0;}
 assert(!ret&&a);FILE *file=fopen(argv[2],"rb");assert(file&&!fseek(file,0,SEEK_END));long bytes=ftell(file);assert(bytes>0&&bytes<2000000&&!(bytes&1));rewind(file);
 int16_t *pcm=glv_alloc(&mem,(size_t)bytes);char *text=glv_alloc(&mem,4096);assert(pcm&&text);
 assert(fread(pcm,1,(size_t)bytes,file)==(size_t)bytes);fclose(file);size_t idle=mem.used;
 assert(glv_asr_transcribe(a,pcm,16000u*60+1,text,4096,NULL,NULL,NULL)==-EINVAL&&mem.used==idle);
 size_t limit=mem.limit;mem.limit=idle;text[0]='X';
 assert(glv_asr_transcribe(a,pcm,(size_t)bytes/2,text,4096,NULL,NULL,NULL)==-ENOMEM&&mem.used==idle&&!text[0]);mem.limit=limit;
 const unsigned stops[]={1,300,3000,50000};
 for(unsigned i=0;i<sizeof(stops)/sizeof(stops[0]);i++){
   c.calls=0;c.stop=stops[i];text[0]='X';
   assert(glv_asr_transcribe(a,pcm,(size_t)bytes/2,text,4096,NULL,NULL,NULL)==-ECANCELED&&!text[0]&&mem.used==idle);
 }c.stop=0;
 struct callback callback={a,0};text[0]='X';
 assert(glv_asr_transcribe(a,pcm,(size_t)bytes/2,text,4096,reject,&callback,NULL)==-123&&callback.seen==1&&!text[0]&&mem.used==idle);
 struct glv_asr_stats stats;
 for(unsigned i=0;i<2;i++){
   assert(!glv_asr_transcribe(a,pcm,(size_t)bytes/2,text,4096,NULL,NULL,&stats));
   assert(text[0]&&stats.tokens&&stats.chunks&&mem.used==idle);
 }
 assert(glv_asr_transcribe(a,pcm,(size_t)bytes/2,text,1,NULL,NULL,NULL)==-ENOSPC&&!text[0]&&mem.used==idle);
 glv_free(pcm);glv_free(text);glv_asr_close(a);assert(!mem.used);
 const size_t limits[]={1,16384,131072,524288,1048576,2097152};
 for(unsigned i=0;i<sizeof(limits)/sizeof(limits[0]);i++){
   mem.limit=limits[i];a=(void *)1;ret=glv_asr_open(&a,argv[1],&mem);
   assert(ret==-ENOMEM&&!a&&!mem.used);
 }
 mem.limit=limit;c.calls=0;c.stop=1;a=(void *)1;
 assert(glv_asr_open(&a,argv[1],&mem)==-ECANCELED&&!a&&!mem.used);
 printf("ASR_ENGINE_LIFECYCLE_OK peak=%zu\n",mem.peak);return 0;
}
