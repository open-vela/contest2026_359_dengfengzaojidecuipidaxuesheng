#!/usr/bin/env python3
"""Exercise the production LVGL page-delete callback with inert event objects."""
import argparse,hashlib,json,subprocess,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from render_all_ui import function
def main():
    p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    repo=Path(__file__).resolve().parents[2];d=repo/'app/espdl-quickapp/overlay/apps/system/desktop';source=d/'glass_local_voice_ui.inc'
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    code='''#include "glass_local_voice.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
typedef struct {void *target;} lv_event_t;
struct local_voice_ui {void *card,*timer;uint32_t request;};
static struct local_voice_ui *g_local_ui;
static struct glv_snapshot fake;
static unsigned releases,stops,timers;
static void *lv_event_get_target(lv_event_t *e){return e->target;}
static void lv_timer_delete(void *t){assert(t);timers++;}
void glass_local_voice_get(struct glv_snapshot *s,char *p,size_t n){(void)p;(void)n;*s=fake;}
void glass_local_voice_release(void){releases++;}
void glass_local_voice_stop_from(uint32_t id,enum glv_cancel_source source){assert(id==fake.request&&source==GLV_CANCEL_VIEW_CLOSE);stops++;}
'''+function(source.read_text(),'local_ui_deleted')+'''
int main(void){int card,other,timer;lv_event_t good={&card},bad={&other};
 for(unsigned capture=0;capture<3;capture++){
  g_local_ui=calloc(1,sizeof(*g_local_ui));assert(g_local_ui);
  g_local_ui->card=&card;g_local_ui->timer=&timer;g_local_ui->request=1;
  fake=(struct glv_snapshot){.busy=true,.capture_pending=capture!=0,.request=capture==2?2:1};
  unsigned before=releases;local_ui_deleted(&bad);assert(g_local_ui&&releases==before);
  local_ui_deleted(&good);assert(!g_local_ui&&releases==before+1);
  local_ui_deleted(&good);assert(releases==before+1);
 }
 assert(releases==3&&stops==1&&timers==3);return 0;}
'''
    (out/'view.c').write_text(code);exe=out/'view'
    subprocess.run(['gcc','-O2','-g','-fsanitize=address,undefined','-fno-pie','-no-pie','-I'+str(d),str(out/'view.c'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
    (out/'VERIFICATION.json').write_text(json.dumps(dict(passed=True,scope='Actual page deletion callback; foreign events and stale captures do not cancel another owner; every actual close queues model retirement',source_sha256={str(source.relative_to(repo)):hashlib.sha256(source.read_bytes()).hexdigest()}),indent=2)+'\n')
    print('LOCAL_VIEW_SESSION_RETIREMENT_OK')
if __name__=='__main__':main()
