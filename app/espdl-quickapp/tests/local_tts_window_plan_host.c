/* SPDX-License-Identifier: Apache-2.0 */
/* Observe the real graph at its live-allocation witness, then reject budgets
 * immediately below that floor. No substitute allocator or shape-only graph. */
#include "glass_local_tts.c"
#include <assert.h>
static struct glv_memory *observed;
static size_t baseline,floor_bytes;
static bool critical,witness;
static bool cancelled(void *unused)
{
  (void)unused;
  if(!critical)return false;
  assert(observed->used-baseline==floor_bytes);
  witness=true;return true;
}
int __real_glv_tts_pie_conv(struct glv_model *,const struct glv_tensor *,const float *,unsigned,unsigned,float *,unsigned,unsigned,unsigned,unsigned,const float *,bool,bool);
int __wrap_glv_tts_pie_conv(struct glv_model *m,const struct glv_tensor *w,const float *x,unsigned c,unsigned t,float *out,unsigned oc,unsigned length,unsigned dilation,unsigned padding,const float *bias,bool leaky,bool add)
{
  critical=!strcmp(w->name,"vocoder.blocks.11.convs2.2.1.weight");
  int ret=__real_glv_tts_pie_conv(m,w,x,c,t,out,oc,length,dilation,padding,bias,leaky,add);
  critical=false;return ret;
}
int main(int argc,char **argv)
{
  assert(argc==2);const unsigned frames[]={1,16,52,60,72,104};
  for(unsigned test=0;test<sizeof(frames)/sizeof(frames[0]);test++){
    struct glv_memory memory={.limit=GLV_MEMORY_LIMIT,.cancelled=cancelled,.tts_serial_fp=true};
    struct glv_tts *tts=NULL;assert(!glv_tts_open(argv[1],&memory,&tts));
    baseline=memory.used;floor_bytes=window_minimum(tts,frames[test]);assert(floor_bytes);
    assert(window_minimum(tts,frames[test]+1)-floor_bytes==70520u);
    observed=&memory;witness=false;
    struct matrix mel=make(tts,80,frames[test]);assert(mel.v);
    struct matrix audio=vocoder(tts,mel);drop(&mel);drop(&audio);
    assert(tts->error==-ECANCELED&&witness&&memory.used==baseline);
    memory.limit=baseline+floor_bytes-1;tts->error=0;witness=false;
    mel=make(tts,80,frames[test]);assert(mel.v);audio=vocoder(tts,mel);drop(&mel);drop(&audio);
    assert(tts->error==-ENOMEM&&!witness&&memory.used==baseline);
    glv_tts_close(tts);assert(!memory.used);
    printf("WINDOW_FLOOR_WITNESS_OK frames=%u floor=%zu below_floor_rejected=1 remaining=0\n",frames[test],floor_bytes);fflush(stdout);
  }
  assert(glv_alloc_charge(0)==SIZE_MAX&&glv_alloc_charge(SIZE_MAX)==SIZE_MAX);
  return 0;
}
