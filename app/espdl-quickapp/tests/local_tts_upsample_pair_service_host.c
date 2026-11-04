/* SPDX-License-Identifier: Apache-2.0 */
#define main inherited_service_main
#include "local_voice_service_host.c"
#undef main
int main(void)
{
 assert(!glass_local_voice_start());struct glv_snapshot s=wait_idle();uint32_t request=0;
 assert(!s.tts_upsample_pair&&!s.profile.sha_bytes&&!s.profile.tts_upsample_pair_calls);
 assert(!glass_local_voice_tts_upsample_pair(true));assert(glass_local_voice_tts_fma(true)==-EINVAL);
 assert(!glass_local_voice_send("你好。",GLV_READ,&request));assert(glass_local_voice_tts_upsample_pair(false)==-EBUSY);s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&s.profile.tts_upsample_pair_calls==4&&!s.profile.tts_upsample_pair_fallbacks&&opens==closes&&opens==1);
 uint64_t sum=0;for(unsigned i=0;i<4;i++)sum+=s.profile.tts_upsample_stage_us[i];assert(sum==s.profile.tts_upsample_us);
 size_t pair_peak=s.memory_peak;
 assert(!glass_local_voice_tts_upsample_pair(false));assert(!glass_local_voice_send("你好。",GLV_READ,&request));s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&!s.profile.tts_upsample_pair_calls&&s.memory_peak==pair_peak&&opens==closes&&opens==2);
 assert(!glass_local_voice_tts_fma(true));assert(glass_local_voice_tts_upsample_pair(true)==-EINVAL);assert(!glass_local_voice_tts_fma(false));assert(!glass_local_voice_tts_upsample_pair(true));
 assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();
 assert(!s.error&&!s.first_audio_ms&&s.profile.tts_wave_fnv==2168036331u&&s.profile.tts_upsample_pair_calls==4&&opens==2);
 assert(!glass_local_voice_tts_benchmark("你好。",&request));
 bool synth=false;for(unsigned i=0;i<20000;i++){glass_local_voice_get(&s,NULL,0);if(s.request==request&&s.busy&&s.phase==GLV_SYNTHESIZING){synth=true;break;}usleep(1000);}
 assert(synth);glass_local_voice_stop(request);s=wait_idle();assert(s.error==-ECANCELED&&opens==closes&&opens==2);
 budget=5376*1024;assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();
 assert(!s.error&&s.memory_peak<=budget&&s.profile.tts_upsample_pair_calls==4&&opens==2);
 puts("UPSAMPLE_PAIR_WORKER_DEFAULT_OFF_NO_PRELOAD_READ_EXACT_FMA_EXCLUSIVE_SAME_PEAK_BUSY_CANCEL_LOW_MEMORY_NO_PLAY_OK");
 return 0;
}
