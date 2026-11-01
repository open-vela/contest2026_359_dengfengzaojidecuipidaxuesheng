/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_local_capture.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
struct fixture {unsigned opened,closed,reads,voice_blocks,finish_after,cancel_after,fail_after;int open_error,close_error;bool silent,oversize,backwards,cancel_now;uint64_t ms;};
static int input_open(void *opaque){struct fixture *f=opaque;f->opened++;f->ms=10000;return f->open_error;}
static int input_read(void *opaque,int16_t *out)
{
 struct fixture *f=opaque;assert(f->opened==1&&!f->closed);f->reads++;f->ms+=15;
 if(f->backwards&&f->reads==3)f->ms=9999;
 if(f->fail_after&&f->reads>=f->fail_after)return -EIO;
 for(unsigned i=0;i<GLV_CAPTURE_BLOCK;i++)out[i]=f->silent||f->reads>f->voice_blocks?0:(i&1)?-1234:1234;
 return f->oversize?GLV_CAPTURE_BLOCK+1:GLV_CAPTURE_BLOCK;
}
static int input_close(void *opaque){struct fixture *f=opaque;assert(f->opened==1&&!f->closed);f->closed++;return f->close_error;}
static uint64_t milliseconds(void *opaque){return ((struct fixture *)opaque)->ms;}
static bool cancelled(void *opaque){struct fixture *f=opaque;return f->cancel_now||(f->cancel_after&&f->reads>=f->cancel_after);}
static bool finished(void *opaque){struct fixture *f=opaque;return f->finish_after&&f->reads>=f->finish_after;}
static unsigned levels;
static void level(void *opaque,unsigned peak,unsigned elapsed){(void)opaque;assert(peak<=32768&&elapsed<=60000);levels++;}
static int collect(struct fixture *f,struct glv_capture_options *options,size_t limit,int16_t **pcm,size_t *count)
{
 struct glv_capture_source source={input_open,input_read,input_close,milliseconds,f};
 struct glv_capture_signals signals={finished,level,f};struct glv_memory m={0,0,limit,cancelled,f};
 int ret=glv_capture_collect(&m,&source,&signals,options,pcm,count);
 if(!ret){assert(f->closed==1&&*pcm&&*count>=1600);for(unsigned i=0;i<*count&&i<f->voice_blocks*GLV_CAPTURE_BLOCK;i++)assert((*pcm)[i]==((i&1)?-1234:1234));glv_free(*pcm);*pcm=NULL;}
 else assert(!*pcm&&!*count);
 assert(!m.used);return ret;
}
int main(void)
{
 struct glv_capture_options automatic={2000,1000,300,true},manual=automatic;manual.automatic_endpoint=false;
 struct fixture f={.voice_blocks=20};int16_t *pcm;size_t count;
 assert(!collect(&f,&automatic,GLV_MEMORY_LIMIT,&pcm,&count)&&count==9600&&f.reads==40&&f.closed==1&&levels>=40);
 f=(struct fixture){.voice_blocks=100,.finish_after=20};assert(!collect(&f,&manual,GLV_MEMORY_LIMIT,&pcm,&count)&&count==4800);
 f=(struct fixture){.silent=true};assert(collect(&f,&automatic,GLV_MEMORY_LIMIT,&pcm,&count)==-ENODATA&&f.closed==1);
 f=(struct fixture){.voice_blocks=1000};struct glv_capture_options short_limit={500,400,200,true};assert(collect(&f,&short_limit,GLV_MEMORY_LIMIT,&pcm,&count)==-E2BIG&&f.closed==1);
 f=(struct fixture){.voice_blocks=100,.cancel_after=3};assert(collect(&f,&manual,GLV_MEMORY_LIMIT,&pcm,&count)==-ECANCELED&&f.closed==1);
 f=(struct fixture){.open_error=-EBUSY};assert(collect(&f,&manual,GLV_MEMORY_LIMIT,&pcm,&count)==-EBUSY&&f.opened==1&&f.closed==1&&!f.reads);
 f=(struct fixture){.voice_blocks=20,.fail_after=3};assert(collect(&f,&automatic,GLV_MEMORY_LIMIT,&pcm,&count)==-EIO&&f.closed==1);
 f=(struct fixture){.oversize=true};assert(collect(&f,&automatic,GLV_MEMORY_LIMIT,&pcm,&count)==-EIO&&f.closed==1);
 f=(struct fixture){.voice_blocks=100,.backwards=true};assert(collect(&f,&manual,GLV_MEMORY_LIMIT,&pcm,&count)==-EIO&&f.closed==1);
 f=(struct fixture){.voice_blocks=100,.finish_after=20,.close_error=-EIO};assert(collect(&f,&manual,GLV_MEMORY_LIMIT,&pcm,&count)==-EIO&&f.closed==1);
 f=(struct fixture){.voice_blocks=20};assert(collect(&f,&automatic,1,&pcm,&count)==-ENOMEM&&!f.opened&&!f.closed);
 f=(struct fixture){.voice_blocks=20};assert(collect(&f,&automatic,65000,&pcm,&count)==-ENOMEM&&f.closed==1);
 f=(struct fixture){.cancel_now=true};assert(collect(&f,&automatic,GLV_MEMORY_LIMIT,&pcm,&count)==-ECANCELED&&!f.opened&&!f.closed);
 struct glv_memory m={0,0,GLV_MEMORY_LIMIT,NULL,NULL};struct glv_capture_source source={input_open,input_read,input_close,milliseconds,&f};
 assert(glv_capture_collect(&m,&source,NULL,&(struct glv_capture_options){60001,1000,300,true},&pcm,&count)==-EINVAL&&!m.used);
 puts("LOCAL_CAPTURE_COLLECT_OK exact PCM/manual/VAD/silence/limit/cancel/open/read/close/clock/OOM cleanup; no device or file IO");return 0;
}
