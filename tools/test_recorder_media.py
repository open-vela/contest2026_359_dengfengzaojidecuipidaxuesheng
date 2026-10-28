#!/usr/bin/env python3
"""Exercise actual built-in recorder media routing with mocked devices/mounts."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--apps',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1]
source=repo/'app/espdl-quickapp/overlay/apps/system/desktop/qpk_hw_audio.c'
text=source.read_text()
production=function(text,'recorder_clip_name')+'\n'+function(text,'recorder_list')+'\n'+function(text,'qpk_hw_audio_execute_at')
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdatomic.h>
#include "cJSON.h"
#define QPK_STORAGE_KEY_MAX 64
enum qpk_volume {QPK_VOLUME_INTERNAL,QPK_VOLUME_SD};
struct qpk_hw_progress {atomic_bool cancel;};
static int sd_error,internal_error;
static unsigned sd_write_requests,flash_write_requests,backend_calls,list_calls;
static const char *qpk_volume_data_root(enum qpk_volume v){return v==QPK_VOLUME_SD?"/sd/qpk/.data":"/flash/qpk/.data";}
static int qpk_volume_ready(enum qpk_volume v,bool create)
{assert(!create);return v==QPK_VOLUME_SD?sd_error:internal_error;}
static int qpk_audio_list(const char *root,const char *package,cJSON *out,const atomic_bool *cancel)
{
  assert(!strcmp(package,"com.openvela.recorder"));list_calls++;
  if(atomic_load(cancel))return -ECANCELED;
  cJSON *clips=cJSON_AddArrayToObject(out,"clips"),*clip=cJSON_CreateObject();
  assert(clips&&clip&&cJSON_AddStringToObject(clip,"clip",!strcmp(root,qpk_volume_data_root(QPK_VOLUME_SD))?"rec_000007":"rec_000001"));
  assert(cJSON_AddBoolToObject(clip,"valid",true));cJSON_AddItemToArray(clips,clip);return 0;
}
static int audio_execute_storage(const char *root,const char *package,const char *op,const cJSON *args,cJSON *out,struct qpk_hw_progress *progress)
{
  (void)progress;backend_calls++;
  bool sd=!strcmp(root,qpk_volume_data_root(QPK_VOLUME_SD));
  bool write=!strcmp(op,"record")||!strcmp(op,"save")||!strcmp(op,"remove");
  if(write){if(sd)sd_write_requests++;else flash_write_requests++;}
  if(!strcmp(package,"com.openvela.recorder")&&write)assert(sd);
  if(sd&&sd_error)return sd_error;
  cJSON_AddStringToObject(out,"root",root);cJSON_AddStringToObject(out,"operation",op);
  const cJSON *clip=cJSON_GetObjectItemCaseSensitive(args,"clip");
  if(cJSON_IsString(clip))cJSON_AddStringToObject(out,"clip",clip->valuestring);
  return 0;
}
'''
post=r'''
static int execute(const char *package,const char *op,cJSON *args,cJSON *out,struct qpk_hw_progress *progress)
{return qpk_hw_audio_execute_at(qpk_volume_data_root(QPK_VOLUME_INTERNAL),package,op,args,out,progress);}
int main(void)
{
  struct qpk_hw_progress progress={0};cJSON *args=cJSON_CreateObject(),*out=cJSON_CreateObject();
  cJSON_AddStringToObject(args,"clip","rec_000008");
  assert(execute("com.openvela.recorder","list",args,out,&progress)==0);
  cJSON *clips=cJSON_GetObjectItemCaseSensitive(out,"clips");assert(cJSON_GetArraySize(clips)==2);
  cJSON *legacy=cJSON_GetArrayItem(clips,1);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(legacy,"clip")->valuestring,"flash:rec_000001"));
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(legacy,"readOnly")));
  cJSON_Delete(out);out=cJSON_CreateObject();
  assert(execute("com.openvela.recorder","record",args,out,&progress)==0);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(out,"root")->valuestring,qpk_volume_data_root(QPK_VOLUME_SD)));
  sd_error=-ENODEV;cJSON_Delete(out);out=cJSON_CreateObject();
  assert(execute("com.openvela.recorder","record",args,out,&progress)==-ENODEV);
  assert(flash_write_requests==0);
  assert(execute("com.openvela.recorder","list",args,out,&progress)==0);
  assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(out,"clips"))==1);
  sd_error=0;cJSON_Delete(out);out=cJSON_CreateObject();
  assert(recorder_clip_name(args,"flash:rec_000001")==0);
  unsigned n=backend_calls;
  assert(execute("com.openvela.recorder","remove",args,out,&progress)==-EROFS&&backend_calls==n);
  assert(execute("com.openvela.recorder","record",args,out,&progress)==-EROFS&&backend_calls==n);
  assert(execute("com.openvela.recorder","play",args,out,&progress)==0);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(out,"root")->valuestring,qpk_volume_data_root(QPK_VOLUME_INTERNAL)));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(out,"clip")->valuestring,"rec_000001"));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(args,"clip")->valuestring,"flash:rec_000001"));
  cJSON_Delete(out);out=cJSON_CreateObject();
  assert(execute("other.app","record",args,out,&progress)==0&&flash_write_requests==1);
  atomic_store(&progress.cancel,true);cJSON_Delete(out);out=cJSON_CreateObject();
  assert(execute("com.openvela.recorder","list",args,out,&progress)==-ECANCELED);
  atomic_store(&progress.cancel,false);sd_error=-EIO;cJSON_Delete(out);out=cJSON_CreateObject();
  n=list_calls;assert(execute("com.openvela.recorder","list",args,out,&progress)==-EIO&&list_calls==n);
  assert(qpk_hw_audio_execute_at(NULL,"com.openvela.recorder","list",args,out,&progress)==-EINVAL);
  cJSON_Delete(args);cJSON_Delete(out);
  puts("RECORDER_MEDIA_OK: real route/list functions, SD-only new writes, missing/broken SD no fallback, read-only legacy aliases, legacy playback, unchanged other apps and cancellation");
}
'''
(out/'test.c').write_text(pre+production+post)
cjson=a.apps.resolve()/'netutils/cjson/cJSON'
subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(cjson),str(out/'test.c'),str(cjson/'cJSON.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True)
(out/'RESULT.json').write_text(json.dumps(dict(passed=True,hardware_tested=False,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),scope='Actual recorder route/list; mocked audio and mount transport'),indent=2)+'\n')
