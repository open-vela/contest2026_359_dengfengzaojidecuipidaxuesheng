/* SPDX-License-Identifier: Apache-2.0 */
#define main inherited_service_main
#include "local_voice_service_host.c"
#undef main
#include "local_tts_int8_policy.h"
int main(void)
{
 assert(!glass_local_voice_start());struct glv_snapshot s=wait_idle();assert(!s.tts_int8&&!s.profile.sha_bytes);
 assert(!glass_local_voice_tts_int8(true));uint32_t request=0;
 assert(!glass_local_voice_tts_benchmark("你好。",&request));
 bool synth=false;for(unsigned i=0;i<20000;i++){glass_local_voice_get(&s,NULL,0);if(s.request==request&&s.busy&&s.phase==GLV_SYNTHESIZING){synth=true;break;}usleep(1000);}
 assert(synth);glass_local_voice_stop(request);s=wait_idle();assert(s.error==-ECANCELED&&!opens&&!frames);
 char path[512],saved[512];snprintf(path,sizeof(path),"%s/policy.bin",GLV_TTS_INT8_POLICY_ROOT);snprintf(saved,sizeof(saved),"%s/policy.saved",GLV_TTS_INT8_POLICY_ROOT);
 assert(!rename(path,saved));assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();assert(s.error==-ENOENT&&!opens&&!frames);assert(!rename(saved,path));
 FILE *f=fopen(path,"r+b");assert(f);assert(!fseek(f,80,SEEK_SET));int old=fgetc(f);assert(old>=0&&!fseek(f,80,SEEK_SET));assert(fputc(old^1,f)>=0&&!fclose(f));
 assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();assert(s.error==-EBADMSG&&!opens&&!frames);
 assert(!glass_local_voice_send("你好。",GLV_READ,&request));s=wait_idle();assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&opens==closes&&opens==1);
 f=fopen(path,"r+b");assert(f);assert(!fseek(f,80,SEEK_SET));assert(fputc(old,f)>=0&&!fclose(f));
 budget=5376*1024;assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();assert(!s.error&&s.memory_peak<=budget&&s.profile.tts_int8_calls==66&&s.profile.tts_int8_kept_calls==8&&opens==1&&opens==closes);
 puts("POLICY_WORKER_POST_LOAD_CANCEL_MISSING_CORRUPT_NORMAL_READ_ISOLATED_LOW_BUDGET_NO_AUDIO_CLEANUP_OK");return 0;
}
