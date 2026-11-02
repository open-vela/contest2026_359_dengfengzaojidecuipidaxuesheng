/* SPDX-License-Identifier: Apache-2.0 */
#define main inherited_service_main
#include "local_voice_service_host.c"
#undef main
int main(void)
{
 assert(!glass_local_voice_start());struct glv_snapshot s=wait_idle();uint32_t request=0;
 assert(s.tts_quant_fast&&!s.profile.sha_bytes&&!s.profile.tts_quant_fast_selftests);
 assert(!glass_local_voice_send("你好。",GLV_READ,&request));s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&s.profile.tts_quant_fast_calls==74&&s.profile.tts_quant_fast_selftests==1&&s.profile.tts_quant_fast_input_saved>0&&opens==closes&&opens==1);
 assert(!glass_local_voice_tts_quant_fast(false));
 assert(!glass_local_voice_send("你好。",GLV_READ,&request));s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&!s.profile.tts_quant_fast_calls&&!s.profile.tts_quant_fast_selftests&&opens==closes&&opens==2);
 assert(!glass_local_voice_tts_quant_fast(true));
 assert(!glass_local_voice_tts_benchmark("你好。",&request));assert(glass_local_voice_tts_quant_fast(false)==-EBUSY);s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&s.profile.tts_quant_fast_calls==74&&s.profile.tts_quant_fast_selftests==1&&opens==2);
 assert(!glass_local_voice_tts_benchmark("你好。",&request));
 bool synth=false;for(unsigned i=0;i<20000;i++){glass_local_voice_get(&s,NULL,0);if(s.request==request&&s.busy&&s.phase==GLV_SYNTHESIZING){synth=true;break;}usleep(1000);}
 assert(synth);glass_local_voice_stop(request);s=wait_idle();assert(s.error==-ECANCELED&&opens==closes&&opens==2);
 budget=5376*1024;assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();
 assert(!s.error&&s.memory_peak<=budget&&s.profile.tts_quant_fast_calls==74&&s.profile.tts_quant_fast_selftests==1&&opens==2);
 puts("STABLE_QUANT_DEFAULT_ON_NO_PRELOAD_READ_EXACT_BUSY_CANCEL_LOW_MEMORY_NO_PLAY_BENCH_OK");return 0;
}
