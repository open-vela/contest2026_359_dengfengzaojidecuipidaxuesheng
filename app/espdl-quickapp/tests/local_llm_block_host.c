/* Production packed reader: exact bytes, bounded reuse, cancellation/fault. */
#include "glass_local_model.c"
#include <assert.h>
static unsigned checks,stop_after;
static bool cancelled(void *p){(void)p;return stop_after&&++checks>=stop_after;}
int main(int argc,char **argv)
{
 assert(argc==2);struct glv_memory a={.limit=GLV_MEMORY_LIMIT},b={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled};
 struct glv_model full,part;assert(!glv_model_open(&full,argv[1],&a,true));assert(!glv_model_open(&part,argv[1],&b,false));
 part.bulk_q8=true;
 assert(glv_model_block_reserve(&part,257u*1024u)==-EINVAL);
 for(unsigned pass=0;pass<2;pass++){
  assert(!glv_model_block_reserve(&part,pass?24u*1024u:192u*1024u));
  for(unsigned i=0;i<part.count;i++){
   const struct glv_tensor *t=part.tensors+i;if(t->type!=2)continue;
   glv_model_block_select(&part,t->name);unsigned char row[580];
   for(unsigned r=0;r<t->shape[0];r++){
    assert(!glv_tensor_q8_packed(&part,t,r,row,sizeof(row),t->shape[1]));
    assert(!memcmp(row,full.data+t->offset+(size_t)r*t->stride,t->stride));
   }
  }
  glv_model_block_release(&part);assert(!part.block_data&&!part.block_bytes);
 }
 const char *name="model.layers.0.mlp.gate_proj.weight";const struct glv_tensor *t=glv_tensor(&part,name);assert(t);
 assert(!glv_model_block_reserve(&part,192u*1024u));assert(!glv_model_cache_tensor(&part,name));
 unsigned char row[580];glv_model_block_select(&part,name);
 for(unsigned r=0;r<t->shape[0];r++)assert(!glv_tensor_q8_packed(&part,t,r,row,sizeof(row),t->shape[1]));
 uint64_t before=b.profile.weight_read_calls;glv_model_block_select(&part,name);
 for(unsigned r=0;r<t->shape[0];r++)assert(!glv_tensor_q8_packed(&part,t,r,row,sizeof(row),t->shape[1]));
 assert(before==b.profile.weight_read_calls);glv_model_cache_clear(&part);
 /* Allocation failure does not change any read/compute policy. */
 glv_model_block_release(&part);size_t limit=b.limit;b.limit=b.used;
 assert(glv_model_block_reserve(&part,24u*1024u)==-ENOMEM&&!part.block_data);b.limit=limit;
 assert(!glv_tensor_q8_packed(&part,t,0,row,sizeof(row),t->shape[1]));
 assert(!memcmp(row,full.data+t->offset,t->stride));
 /* Cancel between the first and second 64 KiB chunks: no valid prefix. */
 assert(!glv_model_block_reserve(&part,192u*1024u));glv_model_block_select(&part,name);checks=0;stop_after=3;
 assert(glv_tensor_q8_packed(&part,t,0,row,sizeof(row),t->shape[1])==-ECANCELED);assert(!part.block_bytes);stop_after=0;
 glv_model_block_select(&part,name);assert(!glv_tensor_q8_packed(&part,t,0,row,sizeof(row),t->shape[1]));
 checks=0;stop_after=1;assert(glv_tensor_q8_packed(&part,t,1,row,sizeof(row),t->shape[1])==-ECANCELED);stop_after=0;
 /* A short file is terminal: one attempted read, no automatic replay. */
 fclose(part.file);part.file=tmpfile();assert(part.file);part.cursor=SIZE_MAX;glv_model_block_select(&part,name);before=b.profile.weight_read_calls;
 assert(glv_tensor_q8_packed(&part,t,0,row,sizeof(row),t->shape[1])==-EIO);
 assert(!part.block_bytes&&b.profile.weight_read_calls==before+1);
 printf("BLOCK_READER_OK loads=%llu hits=%llu peak=%zu all-packed-rows-exact/24K-and-192K/cache-first/allocation-fallback/cancel/fault-no-replay\n",(unsigned long long)b.profile.llm_block_loads,(unsigned long long)b.profile.llm_block_hits,b.profile.llm_block_peak_bytes);
 glv_model_close(&part);glv_model_close(&full);assert(!a.used&&!b.used);return 0;
}
