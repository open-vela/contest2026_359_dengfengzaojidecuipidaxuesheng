/* SPDX-License-Identifier: Apache-2.0 */
#include "local_asr_graph.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cancellation {unsigned calls,stop;};
static bool cancelled(void *opaque)
{struct cancellation *c=opaque;return c->stop&&++c->calls>=c->stop;}
int main(int argc,char **argv)
{
 assert(argc==2||argc==3);
 struct cancellation cancel={0,0};struct glv_memory mem={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.opaque=&cancel};
 struct glv_asr_graph *g=NULL;int ret=glv_asr_graph_open(&g,argv[1],&mem);
 if(argc==3){assert(ret<0&&!g&&!mem.used);puts("ASR_GRAPH_REJECTED_CLEANLY");return 0;}
 assert(!ret&&g);size_t metadata=mem.used;
 unsigned ni=glv_asr_graph_inputs(g),no=glv_asr_graph_outputs(g);assert(ni&&no&&ni<=64&&no<=64);
 struct glv_asr_io in[64],out[64];memset(in,0,sizeof(in));memset(out,0,sizeof(out));
 for(unsigned i=0;i<ni+no;i++){
   struct glv_asr_io *v=i<ni?&in[i]:&out[i-ni];struct glv_asr_shape s;unsigned type;
   assert(!glv_asr_graph_io_info(g,i>=ni,i<ni?i:i-ni,&s,&type,&v->bytes));
   v->data=glv_alloc(&mem,v->bytes);assert(v->data);
 }
 size_t idle=mem.used;size_t size=in[0].bytes;in[0].bytes--;
 assert(glv_asr_graph_run(g,in,ni,out,no)==-EINVAL&&mem.used==idle);in[0].bytes=size;
 void *saved=out[0].data;out[0].data=in[0].data;
 assert(glv_asr_graph_run(g,in,ni,out,no)==-EINVAL&&mem.used==idle);out[0].data=saved;
 size_t limit=mem.limit;mem.limit=mem.used;
 assert(glv_asr_graph_run(g,in,ni,out,no)==-ENOMEM&&mem.used==idle);mem.limit=limit;
 const unsigned stops[]={1,100,700,5000};
 for(unsigned i=0;i<sizeof(stops)/sizeof(stops[0]);i++){
   cancel.calls=0;cancel.stop=stops[i];ret=glv_asr_graph_run(g,in,ni,out,no);
   assert((ret==-ECANCELED||ret==0)&&mem.used==idle);
   if(i==0)assert(ret==-ECANCELED);
 }
 cancel.stop=0;assert(!glv_asr_graph_run(g,in,ni,out,no)&&mem.used==idle);
 if(ni==1&&in[0].bytes==16){
   ((int64_t *)in[0].data)[0]=INT64_MAX;
   assert(glv_asr_graph_run(g,in,ni,out,no)==-ERANGE&&mem.used==idle);
   ((int64_t *)in[0].data)[0]=-1;assert(!glv_asr_graph_run(g,in,ni,out,no));
 }
 cancel.calls=0;cancel.stop=1;struct glv_asr_graph *second=(void *)1;
 assert(glv_asr_graph_open(&second,argv[1],&mem)==-ECANCELED&&!second&&mem.used==idle);cancel.stop=0;
 if(ni==2&&no==1&&out[0].bytes==5537u*4){
   assert(!glv_asr_graph_open(&second,argv[1],&mem));size_t before=mem.used;
   assert(glv_asr_graph_cache(second,1)==-E2BIG&&!glv_asr_graph_cached_bytes(second)&&mem.used==before);
   mem.limit=before;assert(glv_asr_graph_cache(second,2u*1024u*1024u)==-ENOMEM&&mem.used==before);mem.limit=limit;
   cancel.calls=0;cancel.stop=30;
   assert(glv_asr_graph_cache(second,2u*1024u*1024u)==-ECANCELED&&!glv_asr_graph_cached_bytes(second)&&mem.used==before);cancel.stop=0;
   assert(!glv_asr_graph_cache(second,2u*1024u*1024u)&&glv_asr_graph_cached_bytes(second)==2015476);
   size_t cached=mem.used;assert(!glv_asr_graph_cache(second,2u*1024u*1024u)&&mem.used==cached);
   assert(!glv_asr_graph_run(second,in,ni,out,no)&&mem.used==cached);
   glv_asr_graph_close(second);assert(mem.used==idle);
 }
 /* Simulated card/data failure after successful load. Existing descriptor is
  * retained; truncation happens in an isolated fixture, never on real SD. */
 char path[512];assert(snprintf(path,sizeof(path),"%s.data",argv[1])<(int)sizeof(path));
 assert(!truncate(path,0));ret=glv_asr_graph_run(g,in,ni,out,no);
 assert(ret<0&&mem.used==idle);
 for(unsigned i=0;i<ni;i++)glv_free(in[i].data);for(unsigned i=0;i<no;i++)glv_free(out[i].data);
 assert(mem.used==metadata);glv_asr_graph_close(g);assert(!mem.used);
 /* Open with no memory and immediate cancellation must release all metadata. */
 mem.limit=1;g=(void *)1;assert(glv_asr_graph_open(&g,argv[1],&mem)==-ENOMEM&&!g&&!mem.used);
 mem.limit=limit;cancel.calls=0;cancel.stop=1;g=(void *)1;
 ret=glv_asr_graph_open(&g,argv[1],&mem);assert(ret<0&&!g&&!mem.used);
 printf("ASR_GRAPH_LIFECYCLE_OK peak=%zu\n",mem.peak);return 0;
}
