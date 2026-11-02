/* SPDX-License-Identifier: Apache-2.0 */
#define main inherited_service_main
#define glv_platform_audio_write inherited_audio_write
#include "local_voice_service_host.c"
#undef main
#undef glv_platform_audio_write
static unsigned first_request_audio,first_request_time,max_done_on_write;
static bool fail_write,lose_card;
int glv_platform_audio_write(void *audio,const int16_t *pcm,unsigned n)
{
 struct glv_snapshot s;glass_local_voice_get(&s,NULL,0);
 assert(s.profile.tts_planned_clauses==2||s.profile.tts_planned_clauses==1);
 if(s.profile.tts_planned_clauses==2){
   if(!first_request_audio){assert(s.profile.tts_clauses==1);first_request_audio=s.request;}
   if(s.request==first_request_audio){
     if(s.first_audio_ms){if(!first_request_time)first_request_time=s.first_audio_ms;else assert(first_request_time==s.first_audio_ms);}
     if(s.profile.tts_clauses>max_done_on_write)max_done_on_write=s.profile.tts_clauses;
   }
 }
 if(fail_write){fail_write=false;return -EIO;}
 int ret=inherited_audio_write(audio,pcm,n);
 if(lose_card){lose_card=false;atomic_store(&card,false);}
 return ret;
}
int main(void)
{
 assert(!glass_local_voice_start());struct glv_snapshot s=wait_idle();uint32_t request=0;
 assert(s.tts_short&&!s.profile.sha_bytes);
 const char *text="检查已经完成，结果一切正常。";
 assert(!glass_local_voice_send(text,GLV_READ,&request));
 assert(glass_local_voice_tts_short(false)==-EBUSY);s=wait_idle();
 assert(!s.error&&s.profile.tts_planned_clauses==2&&s.profile.tts_clauses==2&&s.profile.tts_first_clause_ms>0&&s.first_audio_ms>0&&first_request_audio==request&&max_done_on_write==2&&s.first_audio_ms==first_request_time&&s.profile.tts_peak_buffered_samples<s.profile.tts_samples&&opens==2&&closes==2);
 uint32_t split_wave=s.profile.tts_wave_fnv;unsigned samples=s.profile.tts_samples;
 assert(!glass_local_voice_tts_benchmark(text,&request));s=wait_idle();
 assert(!s.error&&!s.first_audio_ms&&s.profile.tts_clauses==2&&s.profile.tts_wave_fnv==split_wave&&s.profile.tts_samples==samples&&opens==2);
 stop_on_audio=true;assert(!glass_local_voice_send(text,GLV_READ,&request));s=wait_idle();
 assert(s.error==-ECANCELED&&s.profile.tts_clauses==1&&opens==3&&closes==3);
 fail_write=true;assert(!glass_local_voice_send(text,GLV_READ,&request));s=wait_idle();
 assert(s.error==-EIO&&s.profile.tts_clauses==1&&opens==4&&closes==4);
 lose_card=true;assert(!glass_local_voice_send(text,GLV_READ,&request));s=wait_idle();
 assert(s.error==-ENODEV&&s.profile.tts_clauses==1&&opens==5&&closes==5);atomic_store(&card,true);
 assert(!glass_local_voice_tts_short(false));assert(!glass_local_voice_tts_benchmark(text,&request));s=wait_idle();
 assert(!s.error&&s.profile.tts_clauses==1&&s.profile.tts_planned_clauses==1&&!s.first_audio_ms&&s.profile.tts_peak_buffered_samples==s.profile.tts_samples&&opens==5);
 assert(!glass_local_voice_tts_short(true));assert(!glass_local_voice_tts_benchmark("你好。",&request));s=wait_idle();
 assert(!s.error&&s.profile.tts_wave_fnv==2168036331u&&s.profile.tts_clauses==1&&opens==closes);
 puts("SHORT_CLAUSE_DEFAULT_NO_PRELOAD_FIRST_PLAY_BEFORE_NEXT_SYNTH_NO_PLAY_HASH_CANCEL_CARD_AUDIO_FAILURE_LEGACY_EXACT_OK");
 return 0;
}
