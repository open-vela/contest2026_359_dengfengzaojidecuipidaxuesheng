from pathlib import Path
import subprocess,os
from sys import path
import argparse
p=argparse.ArgumentParser();p.add_argument('--apps',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);r=Path(__file__).resolve().parents[1];path.insert(0,str(r/'tools'))
from render_all_ui import function
d=r/'app/espdl-quickapp/overlay/apps/system/desktop';cjson=a.apps.resolve()/'netutils/cjson/cJSON'
pre=r'''#include "qpk_hardware.h"
#include "qpk_storage.h"
#include "qpk_audio_store.h"
#include "glass_local_voice.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define GLASS_LOCAL_VOICE
#define GLV_ASR_STREAMING
#define GLV_ASR_MICROPHONE
#define QPK_RECORDER_TEXT_ROOT CACHE_ROOT
enum qpk_volume {QPK_VOLUME_INTERNAL,QPK_VOLUME_SD};
static bool card=true,cancel_next=false;
static unsigned calls,stops;
static uint32_t request;
static int result_error;
static struct qpk_hw_progress *current_progress;
int qpk_volume_ready(enum qpk_volume volume,bool create){(void)create;return volume==QPK_VOLUME_SD&&!card?-ENODEV:0;}
const char *qpk_volume_data_root(enum qpk_volume volume){(void)volume;return WAV_ROOT;}
int qpk_audio_path(const char *root,const char *package,const char *clip,char path[QPK_STORAGE_PATH_MAX],bool create){(void)create;assert(!strcmp(package,"com.openvela.recorder"));snprintf(path,QPK_STORAGE_PATH_MAX,"%s/%s.wav",root,clip);return 0;}
int glass_local_voice_transcribe_file(const char *p,uint32_t *id){assert(strstr(p,WAV_ROOT)==p);calls++;*id=++request;result_error=0;if(cancel_next)atomic_store(&current_progress->cancel,true);return 0;}
void glass_local_voice_stop_from(uint32_t id,enum glv_cancel_source source){(void)source;assert(id==request);stops++;result_error=-ECANCELED;}
void glass_local_voice_get(struct glv_snapshot *s,char *reply,size_t cap){(void)reply;(void)cap;memset(s,0,sizeof(*s));s->request=request;s->error=result_error;s->asr_decoded_ms=1000;}
int glass_local_voice_file_result(uint32_t id,struct glv_file_transcript *r){assert(id==request);memset(r,0,sizeof(*r));r->duration_ms=1000;strcpy(r->text,"你好");r->count=2;r->marks[0]=(struct glv_file_mark){100,3};r->marks[1]=(struct glv_file_mark){600,6};return 0;}
'''
source=(d/'qpk_hw_audio.c').read_text();body='\n'.join(function(source,n) for n in ['write_all','start_file'])+'\n#include "qpk_recorder_transcript.inc"\n'
post=r'''
static void write_bytes(const char *path,const char *data){FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,strlen(data),f)==strlen(data));fclose(f);}
int main(void){
 mkdir(WAV_ROOT,0700);char source[256];snprintf(source,sizeof(source),"%s/rec_1.wav",WAV_ROOT);write_bytes(source,"original PCM fixture");
 struct qpk_hw_progress progress={0};current_progress=&progress;
 cJSON *a=cJSON_Parse("{\"clip\":\"rec_1\"}"),*out=cJSON_CreateObject();assert(a&&out);
 assert(!recorder_text_execute("transcript",a,out,&progress));assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out,"available")));cJSON_Delete(out);out=cJSON_CreateObject();
 assert(!recorder_text_execute("transcribe",a,out,&progress));assert(calls==1&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"available")));assert(!strcmp(cJSON_GetObjectItemCaseSensitive(out,"text")->valuestring,"你好"));cJSON_Delete(out);out=cJSON_CreateObject();
 assert(!recorder_text_execute("transcribe",a,out,&progress));assert(calls==1&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"cached")));cJSON_Delete(out);out=cJSON_CreateObject();
 write_bytes(source,"changed PCM fixture");assert(!recorder_text_execute("transcript",a,out,&progress));assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(out,"available")));cJSON_Delete(out);out=cJSON_CreateObject();
 cancel_next=true;assert(recorder_text_execute("transcribe",a,out,&progress)==-ECANCELED);assert(stops==1);cancel_next=false;atomic_store(&progress.cancel,false);
 write_bytes(source,"original PCM fixture");assert(!recorder_text_execute("transcript",a,out,&progress));assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(out,"available")));cJSON_Delete(out);out=cJSON_CreateObject();
 unsigned before=calls;card=false;assert(recorder_text_execute("transcribe",a,out,&progress)==-ENODEV&&calls==before);card=true;
 char hash[65],sidecar[256],ignored[256];assert(!recorder_text_hash(source,hash,&progress));assert(!recorder_text_paths(a,ignored,sidecar));
 write_bytes(sidecar,"[[[[[[[[[[[[0]]]]]]]]]]]]");cJSON *bad=NULL;assert(recorder_text_load(sidecar,hash,&bad)==-EBADMSG&&!bad);
 cJSON *invalid=cJSON_Parse("{\"clip\":\"../bad\"}");assert(recorder_text_execute("transcribe",invalid,out,&progress)==-EINVAL);cJSON_Delete(invalid);
 assert(!recorder_text_remove(a)&&!access(source,F_OK));cJSON_Delete(a);cJSON_Delete(out);
 puts("RECORDER_CAPTION_CACHE_OK: real SHA binding, cache reuse, changed audio invalidation, cancel preserves old text, no SD fallback, parse depth, path validation and removal");return 0;
}
'''
(out/'caption-cache-host.c').write_text(pre+body+post)
cmd=['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(d),'-I'+str(cjson),'-DCACHE_ROOT="'+str(out/'caption-cache')+'"','-DWAV_ROOT="'+str(out/'caption-wave')+'"',str(out/'caption-cache-host.c'),str(d/'glass_local_model.c'),str(d/'glass_local_sha.c'),str(cjson/'cJSON.c'),'-lm','-pthread','-o',str(out/'caption-cache-host')]
subprocess.run(cmd,check=True)
subprocess.run([str(out/'caption-cache-host')],check=True,timeout=30,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'})
