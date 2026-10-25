#!/usr/bin/env python3
"""Offline review renderer: actual desktop page sources, hardware mocks.

Requires the prepared app dependencies and the native LVGL host cache produced
by render_refined_home.py. Never uses a device or network service.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path
from render_refined_home import png565


def function(text,name):
    match=re.search(r'(?m)^(?:static )?[^\n;{}]*\b'+re.escape(name)+r'\([^;]*?\)\s*\{',text)
    if not match:raise ValueError(name)
    i=match.end();depth=1;quote=None;comment=None
    while depth:
        c=text[i];n=text[i:i+2]
        if comment=='line':
            if c=='\n':comment=None
        elif comment=='block':
            if n=='*/':comment=None;i+=1
        elif quote:
            if c=='\\':i+=1
            elif c==quote:quote=None
        elif n=='//':comment='line';i+=1
        elif n=='/*':comment='block';i+=1
        elif c in '\"\'':quote=c
        elif c=='{':depth+=1
        elif c=='}':depth-=1
        i+=1
    return text[match.start():i]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apps',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    p.add_argument('--cache',type=Path,required=True)
    p.add_argument('--desktop-source',type=Path,help='Optional immutable desktop source snapshot, including its runtime header')
    p.add_argument('--prepare-only',action='store_true')
    p.add_argument('--harness-source',type=Path,help='Optional focused native page harness')
    p.add_argument('--png-prefix',default='',help='Convert only the matching native captures; all pages still run under sanitizers')
    p.add_argument('--grok-assets',type=Path,help='Predecoded SD banks used by the actual native player')
    p.add_argument('--music-font',type=Path,help='Pinned SD font file used by the native streaming-font review')
    p.add_argument('--lyric-fonts',type=Path,help='Verified optional lyric font pack directory')
    a=p.parse_args();repo=Path(__file__).resolve().parents[1];apps=a.apps.resolve();out=a.output.resolve();cache=a.cache.resolve()
    out.mkdir(parents=True,exist_ok=True);work=out/'source';work.mkdir(exist_ok=True)
    desktop=a.desktop_source.resolve() if a.desktop_source else repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    for path in (apps/'system/desktop').glob('*.h'):shutil.copy2(path,work/path.name)
    for path in desktop.iterdir():
        if path.suffix in ('.h','.inc'):shutil.copy2(path,work/path.name)
    for folder in ['c6probe','hass']:
        for path in (apps/f'system/{folder}').glob('*.h'):shutil.copy2(path,work/path.name)
    for name in ['desktop_worker.h','desktop_backend.h']:
        shutil.copy2(repo/'tools/c6'/name,work/name)
    for path in (desktop.parent/'hass').glob('*.h'):shutil.copy2(path,work/path.name)
    (work/'nuttx').mkdir(exist_ok=True);(work/'netutils').mkdir(exist_ok=True)
    (work/'nuttx/config.h').write_text('''#pragma once
#define CONFIG_SYSTEM_DESKTOP 1
#define CONFIG_SYSTEM_C6_DESKTOP 1
#define CONFIG_SYSTEM_GLASS_BLE 1
#define CONFIG_SYSTEM_HASS 1
#define CONFIG_SYSTEM_BILIBILI_LIVE 1
#define CONFIG_SYSTEM_DESKTOP_QPK_DIR "/offline-fixtures/qpk"
#define CONFIG_LIBC_LOCALTIME 1
''')
    (work/'nuttx/sched.h').write_text('#pragma once\n')
    (work/'netutils/netlib.h').write_text('#pragma once\n#include <netinet/in.h>\nint netlib_get_ipv4addr(const char*,struct in_addr*);\n')
    # Do not compile boot networking, workers or device CLI entry points.
    main=(desktop/'desktop_main.c').read_text().split('#include "glass_ui_probe.inc"')[0]
    main=main.replace('#include "../bilibili_live/', '#include "')
    (work/'production_desktop.inc').write_text(main)
    ui=(desktop/'glass_ui.inc').read_text().replace('#include "glass_netboot.inc"','#include <arpa/inet.h>\n#include <netutils/netlib.h>\n#define C6NET_IFNAME "wlan0"\nstatic void glass_net_lease(bool wait) {(void)wait;}\nstatic bool review_wifi_lease_ready=true;\nstatic bool glass_net_wifi_ready(uint64_t generation) {(void)generation;return review_wifi_lease_ready;}\nstatic struct glass_status_network review_net;\nstatic void glass_status_network_get(struct glass_status_network *out) {*out=review_net;}')
    ui=ui.replace(function(ui,'desktop_ui_create'),'static void desktop_ui_create(void) {}')
    (work/'glass_ui.inc').write_text(ui)
    # The media constructors are unchanged; workers are replaced by inert
    # callbacks. Their production layout and LVGL objects are retained.
    media=['''#define WIDTH 640
#define HEIGHT 360
#define W 640
#define H 360
static lv_obj_t *g_page,*g_room,*g_platform,*g_status,*g_metrics,*g_scale_switch,*g_viewport,*g_image;
static const lv_font_t *g_font;
static bool g_scale;
static lv_timer_t *g_timer;
static struct {bool joined;} *g_session;
static void reap(void){} static void release(void){}
static void deleted(lv_event_t *e){(void)e;}
static void play_clicked(lv_event_t *e){(void)e;} static void stop_clicked(lv_event_t *e){(void)e;}
static void live_scale_changed(lv_event_t *e){(void)e;} static void tick(lv_timer_t *t){(void)t;}
void bili_live_stop(void){}
''']
    live=(repo/'app/bilibili_live/bili_live.c').read_text()
    media += [function(live,'button'),function(live,'bili_live_show')]
    local=(repo/'app/bilibili_live/local_video_ui.c').read_text()
    media.append('''#define button local_button
static lv_obj_t *page,*image,*label,*list,*path_label,*pause_button,*viewport,*metrics;
static const lv_font_t *font;static lv_timer_t *timer;static unsigned volume=40;static bool album_playback_view;
static char directory[512]="/sdcard";
static void refresh(lv_event_t *e){(void)e;}static void up(lv_event_t *e){(void)e;}
static void pause_clicked(lv_event_t *e){(void)e;}static void fullscreen_clicked(lv_event_t *e){(void)e;}
static void volume_changed(lv_event_t *e){(void)e;}static void update(lv_timer_t *t){(void)t;}
void local_video_stop(void){}
void local_video_play_path(const char *path){(void)path;}
''')
    media += [function(local,'button'),function(local,'local_video_show'),function(local,'video_view_button'),function(local,'local_video_view'),'#undef button']
    (work/'media_pages.inc').write_text('\n'.join(media))
    runtime=(desktop/'qpk_runtime.c').read_text()
    (work/'production_qpk_ha_navigation.inc').write_text(function(runtime,'qpk_runtime_open_ha_device'))
    binding_names=['qpk_arg_int','qpk_arg_color','qpk_arg_string','qpk_widget_deleted','qpk_add_widget','qpk_widget_current',
      'qpk_event_release','qpk_event_deleted','js_ui_text','qpk_number_style','js_ui_number','js_ui_set_text',
      'qpk_touch_call','qpk_event_touched','qpk_event_swiped',
      'js_ui_camera_reference','js_ui_camera_reference_zoom','js_ui_camera_reference_controls','js_ui_set_hidden','js_ui_background','js_ui_get_size','js_ui_is_visible','js_ui_set_color','js_ui_set_style','js_ui_measure_text','js_ui_panel',
      'js_ui_set_pos','js_ui_set_size','js_ui_present','js_ui_range','js_ui_set_opacity','js_ui_show','js_ui_hide','js_ui_remove','js_ui_rect',
      'js_ui_arc','js_ui_arc_set','js_ui_line','js_ui_line_set','js_ui_button','qpk_input_finish',
      'qpk_input_cancel','qpk_input_submit','qpk_input_button','js_prompt_input','qpk_message_arg','js_prompt_toast','js_prompt_dialog']
    visibility='js_ui_is_visible(' in runtime
    if not visibility:binding_names.remove('js_ui_is_visible')
    (work/'production_qpk_bindings.inc').write_text(('#include "glass_ui_visibility.h"\n' if visibility else '')+'#include "camera_reference_ui.h"\nstatic bool qpk_viewport_object(lv_obj_t *);\nstatic void qpk_viewport_origin(lv_obj_t *,int *,int *);\nstatic void qpk_viewport_cleanup(lv_obj_t *,void *);\n'+'\n\n'.join(function(runtime,n) for n in binding_names)+'\n#include "qpk_polygon.inc"\n#include "qpk_viewport.inc"\n')
    registration={'text':('js_ui_text',6),'number':('js_ui_number',6),'setText':('js_ui_set_text',2),
      'cameraReference':('js_ui_camera_reference',0),'cameraReferenceZoom':('js_ui_camera_reference_zoom',2),'cameraReferenceControls':('js_ui_camera_reference_controls',11),'setHidden':('js_ui_set_hidden',2),'background':('js_ui_background',1),'getSize':('js_ui_get_size',0),
      'setColor':('js_ui_set_color',2),'setStyle':('js_ui_set_style',2),'measureText':('js_ui_measure_text',2),'panel':('js_ui_panel',7),
      'present':('js_ui_present',2),'range':('js_ui_range',5),'rangeLinked':('js_ui_range',6),'button':('js_ui_button',7),'setPos':('js_ui_set_pos',3),'setSize':('js_ui_set_size',3),
      'setOpacity':('js_ui_set_opacity',2),'show':('js_ui_show',1),'hide':('js_ui_hide',1),'remove':('js_ui_remove',1),
      'rect':('js_ui_rect',5),'arc':('js_ui_arc',9),'arcSet':('js_ui_arc_set',2),'line':('js_ui_line',4),'lineSet':('js_ui_line_set',2),
      'polygon':('js_ui_polygon',5),'polygonSet':('js_ui_polygon_set',6),
      'polygonBuffer':('js_ui_polygon_buffer',5),'polygonSetBuffer':('js_ui_polygon_set_buffer',6),'front':('js_ui_front',1),
      'grokSprite':('js_ui_grok_sprite',6),'grokControl':('js_ui_grok_control',3)}
    registration.update({'viewport':('js_ui_viewport',5),'attach':('js_ui_attach',2),'scrollInfo':('js_ui_scroll_info',1),'scrollTo':('js_ui_scroll_to',3),'scrollExtent':('js_ui_scroll_extent',2),'scrollLock':('js_ui_scroll_lock',2)})
    if visibility:registration['isVisible']=('js_ui_is_visible',0)
    registration['colorWheel']=('js_ui_color_wheel',3)
    (work/'production_qpk_registration.inc').write_text('\n'.join(f'JS_SetPropertyStr(c,ui,"{name}",JS_NewCFunction(c,{fn},"{name}",{count}));' for name,(fn,count) in registration.items()))
    qrdir=apps/'graphics/lvgl/lvgl/src/libs/qrcode'
    qr=(qrdir/'qrcodegen.c').read_text().replace('#if LV_USE_QRCODE','#if 1').replace('"../../misc/lv_assert.h"','<lvgl/src/misc/lv_assert.h>')
    (work/'qrcodegen.c').write_text(qr)
    (work/'qrcodegen.h').write_text((qrdir/'qrcodegen.h').read_text().replace('"../../../lvgl.h"','<lvgl/lvgl.h>').replace('#if LV_USE_QRCODE','#if 1'))
    # A service header's exact prototypes are used for inert host definitions.
    headers=['qpk_runtime.h','qpk_storage.h','glass_qpk_builder.h','glass_portal.h',
      'hass_portal.h','hass_service.h','glass_music.h','glass_lock_photo.h','glass_chat.h','glass_ime.h',
      'glass_weather.h','glass_settings_service.h','glass_file_service.h','glass_system_monitor.h','glass_voice.h','glass_pinyin.h',
      'glass_ble.h','glass_ble_central.h','glass_status.h','glass_speech.h','qpk_espdl.h','desktop_worker.h','pet_engine.h','pet_lvgl.h','qpk_mjpeg.h']
    skip={'glass_ime_attach','glass_ime_commit','glass_ime_commit_tree','glass_ime_start_voice','glass_ime_set_target',
          'desktop_motion_button','qpk_mjpeg_create','qpk_mjpeg_destroy','qpk_mjpeg_probe','qpk_mjpeg_preview_limits',
          'qpk_mjpeg_frame_size','qpk_mjpeg_decode','qpk_mjpeg_decode_region','qpk_mjpeg_decode_view','qpk_mjpeg_decode_preview','qpk_mjpeg_decode_still','qpk_mjpeg_decode_still_fit'}
    mocks=['#include <stdlib.h>\n#include <string.h>\n#include <stdio.h>\n#include <errno.h>\n#include <netinet/in.h>\n#include "glass_icons.h"']
    mocks += [f'#include "{name}"' for name in headers]
    prototypes={}
    for header in headers:
        text=(work/header).read_text()
        text=re.sub(r'/\*.*?\*/','',text,flags=re.S)
        for m in re.finditer(r'(?m)^([\w\s*]+?)\s+(\w+)\s*\(([^;{}]*?)\)\s*;',text):
            ret,name,args=m.groups();ret=ret.strip()
            if 'typedef' in ret or name in skip or name.startswith('qpk_runtime_'):continue
            # Do not swallow preceding preprocessor lines into a return type.
            ret=ret.split('\n')[-1].strip()
            if ret.startswith('extern'):ret=ret[6:]
            if not ret:continue
            prototypes[name]=(ret,args)
    custom=repo/'app/espdl-quickapp/tests/all_ui_services.c'
    custom_text=custom.read_text() if custom.exists() else ''
    for name,(ret,args) in prototypes.items():
        if re.search(r'\b'+name+r'\s*\(',custom_text):continue
        body='' if ret=='void' else 'return NULL;' if '*' in ret else 'return 0;'
        mocks.append(f'{ret} {name}({args}) {{{body}}}')
    mocks.append(custom_text)
    (work/'service_mocks.c').write_text('\n'.join(mocks))
    if a.prepare_only:return
    flags=['-std=gnu11','-O1','-g','-ffunction-sections','-fdata-sections','-pthread',
      '-fsanitize=address,undefined','-fno-omit-frame-pointer',
      '-I'+str(work),'-I'+str(desktop),'-I'+str(apps/'system/desktop'),
      '-I'+str(repo/'app/bilibili_live'),'-I'+str(apps/'graphics/lvgl'),
      '-I'+str(apps/'netutils/cjson/cJSON'),'-I'+str(cache),'-DLV_CONF_INCLUDE_SIMPLE']
    objects=sorted(cache.glob('lvgl-*.o'),key=lambda x:int(x.stem.split('-')[-1]))
    qjs=apps/'interpreters/quickjs/quickjs'
    qjsflags=['-std=gnu11','-O1','-g','-fwrapv','-D_GNU_SOURCE','-DCONFIG_BIGNUM',
      '-DCONFIG_VERSION="'+(qjs/'VERSION').read_text().strip()+'"','-I'+str(qjs),'-pthread',
      '-fsanitize=address,undefined','-fno-omit-frame-pointer']
    for name in ['quickjs.c','libregexp.c','libunicode.c','cutils.c','libbf.c']:
        obj=out/(name+'.o')
        if not obj.exists():subprocess.run(['gcc',*qjsflags,'-c',str(qjs/name),'-o',str(obj)],check=True)
        objects.append(obj)
    prelude=work/'review_prelude.js'
    prelude.write_text((repo/'app/espdl-quickapp/tests/all_ui_fixture.js').read_text()+'\n'+(repo/'app/espdl-quickapp/tests/mijia_card_fixture.js').read_text())
    flags += ['-I'+str(qjs),'-DCONFIG_BIGNUM','-DREVIEW_PRELUDE="'+str(prelude)+'"']
    if a.harness_source:flags+=['-DGROK_HOST_TEST']
    if a.music_font:flags+=['-DGLASS_SD_FONT_PATH="'+str(a.music_font.resolve())+'"']
    if a.lyric_fonts:flags+=['-DGLASS_LYRIC_FONT_ROOT="'+str(a.lyric_fonts.resolve())+'"']
    if a.grok_assets:flags+=['-DGROK_ASSET_ROOT="'+str(a.grok_assets)+'"']
    sources=[a.harness_source or repo/'app/espdl-quickapp/tests/render_all_ui.c',repo/'app/espdl-quickapp/tests/qpk_review_bridge.c',work/'service_mocks.c',desktop/'glass_icons.c',desktop/'glass_ime.c',
      apps/'netutils/cjson/cJSON/cJSON.c',work/'qrcodegen.c']
    sources += [desktop/name for name in ['camera_resource.c','recorder_resource.c','espdl_resource.c','ouo_resource.c','dafeiyu_resource.c','homeassistant_resource.c']]
    sources += [desktop/name for name in ['glass_grok.c','glass_grok_state.c','glass_code_font.c']]
    sources += [desktop/'glass_sd_font.c']
    sources += [desktop/name for name in ['qpk_mjpeg.c','qpk_tjpgd.c']]
    sources += [repo/'app/bilibili_live/local_album_ui.c',repo/'app/bilibili_live/album_formats.c']
    command=['gcc',*flags,*map(str,sources),*map(str,objects),'-Wl,--gc-sections','-lm','-o',str(out/'render-all-ui')]
    with (out/'compile.log').open('w') as log:
        result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:
        print('\n'.join((out/'compile.log').read_text().splitlines()[:110]));raise SystemExit(result.returncode)
    subprocess.run([str(out/'render-all-ui'),str(apps/'system/desktop/AlibabaPuHuiTi-3-55-Regular.otf'),str(out)],check=True)
    captures=[out/Path(row['file']).with_suffix('.rgb565') for row in json.loads((out/'screens.json').read_text()) if row['id'].startswith(a.png_prefix)]
    with ProcessPoolExecutor(max_workers=4) as pool:list(pool.map(png565,captures))
    tracked=sources+[repo/'tools/render_all_ui.py',repo/'app/espdl-quickapp/tests/all_ui_fixture.js',desktop/'desktop_main.c',desktop/'qpk_runtime.c']+list(desktop.glob('*.inc'))+[desktop/'glass_file_types.h',desktop/'glass_pager.h',desktop/'glass_speech_level.h',desktop/'glass_grok_gaze.h',desktop/'glass_chat_markup.h',desktop/'glass_chat.h']
    records=[]
    tracked += [desktop/name for name in ['glass_app_theme.h','glass_app_controls.h','qpk_app_theme.h']]
    tracked += [repo/'app/bilibili_live'/name for name in ['bili_live.c','local_video_ui.c']]
    if (desktop/'glass_ui_visibility.h').exists():tracked.append(desktop/'glass_ui_visibility.h')
    for path in sorted(set(tracked)):
        try:name=str(path.relative_to(repo))
        except ValueError:name='prepared-apps/'+str(path.relative_to(apps)) if path.is_relative_to(apps) else path.name
        records.append({'source':name,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
    (out/'render-sources.json').write_text(json.dumps(records,ensure_ascii=False,indent=2))
    print('ALL_UI_RENDER_OK',out)


if __name__=='__main__':main()
