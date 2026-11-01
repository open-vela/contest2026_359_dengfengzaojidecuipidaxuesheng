/* SPDX-License-Identifier: Apache-2.0 */
#include "local_tts_pie.c"
#include <assert.h>
#include <string.h>
static unsigned clocks,polls,stop_at;
uint64_t __real_glv_now_us(void);
uint64_t __wrap_glv_now_us(void){clocks++;return __real_glv_now_us();}
static bool cancel(void *unused){(void)unused;return ++polls==stop_at;}
int main(void)
{
  unsigned char data[26];
  for(unsigned o=0;o<2;o++){
    float scale=.01f;memcpy(data+o*13,&scale,4);
    for(unsigned k=0;k<9;k++)data[o*13+4+k]=(unsigned char)(int8_t)((int)k*17-100);
  }
  struct glv_memory m={.limit=GLV_MEMORY_LIMIT,.cancelled=cancel};
  struct glv_model model={.memory=&m,.data=data,.bytes=sizeof(data)};
  struct glv_tensor tensor={.shape={2,3,3},.ndim=3,.type=2,.stride=13};
  float in[3*129],full[2*129],out[2*129],bias[2]={.1f,-.2f};
  for(unsigned k=0;k<3*129;k++)in[k]=(float)((int)(k%31)-15)*.11f;
  for(unsigned single=0;single<2;single++)for(unsigned add=0;add<2;add++){
    unsigned baseline_clocks=0,baseline_polls=0,batches=0;
    for(unsigned coarse=0;coarse<2;coarse++){
      memset(&m.profile,0,sizeof(m.profile));m.tts_single_dot=single;m.tts_coarse_profile=coarse;
      for(unsigned k=0;k<2*129;k++)out[k]=.25f;
      clocks=polls=stop_at=0;
      assert(!glv_tts_pie_conv(&model,&tensor,in,3,129,out,2,129,1,1,bias,true,add));
      assert(!m.used);
      if(!coarse){memcpy(full,out,sizeof(out));baseline_clocks=clocks;baseline_polls=polls;batches=(129+7)/8;}
      else{
        assert(!memcmp(full,out,sizeof(out)));assert(polls==baseline_polls);
        assert(baseline_clocks-clocks==6*batches);
        assert(!m.profile.tts_pie_pack_us&&!m.profile.tts_pie_compute_us&&!m.profile.tts_pie_output_us);
      }
    }
    /* Cancel at every callback boundary in both modes, including final batch.
     * No callback is removed or deferred by the profiling switch. */
    for(unsigned at=1;at<=baseline_polls;at++)for(unsigned coarse=0;coarse<2;coarse++){
      memset(&m.profile,0,sizeof(m.profile));m.tts_coarse_profile=coarse;
      polls=0;stop_at=at;
      assert(glv_tts_pie_conv(&model,&tensor,in,3,129,out,2,129,1,1,bias,true,add)==-ECANCELED);
      assert(polls==at&&!m.used);
    }
  }
  puts("TTS_PROFILE_EXACT_SIX_CLOCKS_PER_BATCH_CANCEL_EVERY_BOUNDARY_CLEANUP_OK");return 0;
}
