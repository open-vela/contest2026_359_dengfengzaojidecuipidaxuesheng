#!/usr/bin/env python3
"""Render the production refined home and outline icon atlas with real LVGL.

Runs on Linux/WSL, never opens a serial device or connects external services.
Only the service snapshots and entry callbacks are mocked. Layout, font data,
assets, object helpers and cached update function come from production source.
"""
import argparse
import concurrent.futures
import os
from pathlib import Path
import re
import struct
import subprocess
import zlib


def png565(source):
    raw=source.read_bytes();rgb=bytearray()
    for y in range(600):
        rgb.append(0)
        for x in range(1024):
            i=(y*1024+x)*2;value=raw[i]|raw[i+1]<<8
            rgb.extend((((value>>11)&31)*255//31,((value>>5)&63)*255//63,(value&31)*255//31))
    def chunk(kind,data):
        return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',1024,600,8,2,0,0,0))
    source.with_suffix('.png').write_bytes(png+chunk(b'IDAT',zlib.compress(bytes(rgb)))+chunk(b'IEND',b''))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apps',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    repo=Path(__file__).resolve().parents[1];apps=args.apps.resolve();out=args.output.resolve()
    desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
    lvgl=apps/'graphics/lvgl/lvgl';out.mkdir(parents=True,exist_ok=True)
    source=(desktop/'glass_ui.inc').read_text()
    helpers=[]
    for start,end in [
        ('static lv_obj_t *glass_box(', 'static lv_obj_t *glass_number('),
        ('static struct glass_display_s', 'static bool g_preferences_save_failed;'),
        ('static void glass_update(lv_timer_t *timer)', 'static void glass_palette_select(')]:
        first=source.index(start);last=source.index(end,first);helpers.append(source[first:last])
    helpers.insert(2, '#include "glass_weather_scene.inc"')
    (out/'production_home_helpers.inc').write_text('\n'.join(helpers))
    # Full LV_SYMBOL coverage plus semantically named additions.
    generator=repo/'tools/generate_glass_icons.py'
    import importlib.util
    spec=importlib.util.spec_from_file_location('icons',generator);icons=importlib.util.module_from_spec(spec);spec.loader.exec_module(icons)
    atlas=[]
    for i,(name,code) in enumerate(icons.SYMBOLS.items()):
        x=24+(i%10)*100;y=24+(i//10)*70
        encoded=''.join(f'\\x{b:02x}' for b in chr(code).encode())
        atlas.append(f'home_icon(g_desktop.screen,"{encoded}",{x+32},{y},24,0x242430);')
        atlas.append(f'home_text(g_desktop.screen,"{name}",{x},{y+34},98,12,0x565360);')
    (out/'production_icon_atlas.inc').write_text('\n'.join(atlas))
    config=(lvgl/'lv_conf_template.h').read_text().replace('#if 0 /*Set it to "1" to enable content*/','#if 1 /*Host render*/')
    config=re.sub(r'#define LV_MEM_SIZE .*','#define LV_MEM_SIZE (16 * 1024 * 1024U)',config)
    for key in ['LV_USE_TINY_TTF','LV_FONT_MONTSERRAT_24','LV_FONT_MONTSERRAT_14']:
        config=re.sub(rf'#define {key} .*',f'#define {key} 1',config)
    # Thread-safe system malloc gives ASan visibility into real allocations.
    config=re.sub(r'#define LV_USE_STDLIB_MALLOC .*','#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB',config)
    conf=out/'lv_conf.h';unchanged=conf.exists() and conf.read_text()==config
    if not unchanged:conf.write_text(config)
    flags=['-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread',
           '-I'+str(desktop),'-I'+str(apps/'system/desktop'),'-I'+str(apps/'graphics/lvgl'),
           '-I'+str(out),'-I'+str(desktop.parent/'hass'),'-I'+str(apps/'netutils/cjson/cJSON'),'-I'+str(repo/'app/espdl-quickapp/tests'),'-DLV_CONF_INCLUDE_SIMPLE']
    environment=os.environ.copy();environment['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1';environment['ASAN_OPTIONS']='detect_leaks=1'
    def run(*cmd):subprocess.run(list(map(str,cmd)),check=True,env=environment)
    def compile(item):
        i,s=item;obj=out/f'lvgl-{i}.o'
        if unchanged and obj.exists() and obj.stat().st_mtime>s.stat().st_mtime:return obj
        run('gcc',*flags,'-c',s,'-o',obj);return obj
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        objects=list(pool.map(compile,enumerate(sorted((lvgl/'src').rglob('*.c')))))
    run('gcc',*flags,repo/'app/espdl-quickapp/tests/render_refined_home.c',desktop/'glass_i18n.c',desktop/'glass_icons.c',desktop/'glass_code_font.c',desktop/'glass_grok.c',desktop/'glass_grok_state.c',desktop/'qpk_mjpeg.c',desktop/'qpk_tjpgd.c',apps/'netutils/cjson/cJSON/cJSON.c',
        *objects,'-lm','-o',out/'render-refined-home')
    run(out/'render-refined-home',apps/'system/desktop/AlibabaPuHuiTi-3-55-Regular.otf',out)
    for path in out.glob('*.rgb565'):png565(path)
    print('Actual LVGL host rendering complete:',out)


if __name__=='__main__':main()
