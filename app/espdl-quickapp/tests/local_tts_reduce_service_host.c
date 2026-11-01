/* SPDX-License-Identifier: Apache-2.0 */
#define main inherited_service_main
#include "local_voice_service_host.c"
#undef main
int main(void)
{
 assert(!glass_local_voice_start());struct glv_snapshot s=wait_idle();uint32_t request=0;
 assert(!s.tts_int8&&!s.tts_int8_reduce&&!s.profile.sha_bytes);
 assert(!glass_local_voice_tts_int8_reduce(true));
 assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&!s.profile.tts_int8_reduce_calls&&!opens);
 assert(!glass_local_voice_tts_int8(true));
 assert(!glass_local_voice_tts_benchmark("你好。",&request));
 assert(glass_local_voice_tts_int8_reduce(false)==-EBUSY);s=wait_idle();
 assert(!s.error&&s.profile.tts_int8_calls==66&&s.profile.tts_int8_kept_calls==8);
 assert(s.profile.tts_int8_reduce_calls==66&&s.profile.tts_int8_fast_input_calls==66&&s.profile.tts_int8_reduce_selftests==1&&s.profile.tts_int8_input_conversions_saved>0&&s.profile.tts_int8_conversions_saved>0&&!opens&&!frames);
 assert(!glass_local_voice_send("你好。",GLV_READ,&request));s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&!s.profile.tts_int8_reduce_calls&&!s.profile.tts_int8_reduce_selftests&&opens==closes&&opens==1);
 assert(!glass_local_voice_tts_benchmark("你好。",&request));
 bool synth=false;for(unsigned i=0;i<20000;i++){glass_local_voice_get(&s,NULL,0);if(s.request==request&&s.busy&&s.phase==GLV_SYNTHESIZING){synth=true;break;}usleep(1000);}
 assert(synth);glass_local_voice_stop(request);s=wait_idle();assert(s.error==-ECANCELED&&opens==closes&&opens==1);
 budget=5376*1024;assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();
 assert(!s.error&&s.memory_peak<=budget&&s.profile.tts_int8_reduce_calls==66&&s.profile.tts_int8_fast_input_calls==66&&s.profile.tts_int8_reduce_selftests==1&&opens==1);
 assert(!glass_local_voice_tts_int8_reduce(false));s=wait_idle();assert(!s.tts_int8_reduce);
 puts("FULL_ROW_WORKER_DEFAULT_OFF_JOB9_ONLY_NORMAL_READ_BASELINE_BUSY_CANCEL_LOW_MEMORY_NO_PLAY_OK");return 0;
}
