"""Opt-in LVGL image/copy bridge and non-halting stage timers, prepared tree only."""
import hashlib
from pathlib import Path
import re
from render_all_ui import function

MARKER='/* GLASS VIDEO BLIT BRIDGE */'
HEADER='''/* GLASS VIDEO BLIT BRIDGE */
#include <stdint.h>
uint64_t glass_video_profile_now(void);
void glass_video_profile_add(unsigned,uint64_t);
int glass_video_blit_image(void *,const void *,const void *,const void *,const void *);
int glass_video_blit_copy(void *,const void *,const void *,const void *);
int glass_video_blit_fill(void *,const void *,const void *);
'''

def apply(apps,enabled=True):
    root=Path(apps)/'graphics/lvgl/lvgl/src'
    targets=[('draw/sw/lv_draw_sw_img.c','img_draw_core',0),
             ('draw/sw/blend/lv_draw_sw_blend.c','lv_draw_sw_blend',1),
             ('draw/lv_draw_buf.c','lv_draw_buf_copy',2),
             ('draw/sw/lv_draw_sw_fill.c','lv_draw_sw_fill',3),
             ('draw/sw/lv_draw_sw_transform.c','lv_draw_sw_transform',4),
             ('core/lv_refr.c','refr_sync_areas',5),
             ('drivers/nuttx/lv_nuttx_fbdev.c','flush_cb',6),
             ('draw/sw/lv_draw_sw_box_shadow.c','lv_draw_sw_box_shadow',7)]
    receipts={}
    for name,func,kind in targets:
        path=root/name
        backup=path.with_name(path.name+'.glass_video_blit.original')
        source=path.read_text()
        if enabled and MARKER in source and 'int glass_video_blit_fill(' not in source:
            if not backup.exists():raise RuntimeError('Missing original LVGL source for bridge migration: '+name)
            path.write_bytes(backup.read_bytes());backup.unlink()
            source=path.read_text()
        if not enabled:
            if backup.exists():path.write_bytes(backup.read_bytes());backup.unlink()
            continue
        if MARKER not in source:
            if backup.exists():raise RuntimeError('Stale original LVGL bridge backup: '+name)
            body=function(source,func)
            signature=body[:body.index('{')].strip()
            args=signature[signature.index('(')+1:signature.rindex(')')]
            names=[]
            if args.strip() not in ('','void'):
                for arg in args.split(','):
                    match=re.search(r'(\w+)\s*(?:\[[^]]*\])?\s*$',arg)
                    if not match:raise RuntimeError('Unexpected LVGL argument: '+arg)
                    names.append(match[1])
            renamed=re.sub(r'\b'+func+r'\s*\(',func+'_glass_inner(',body,count=1)
            if func=='img_draw_core':
                renamed=renamed.replace('{','{\n    if(glass_video_blit_image(draw_unit, draw_dsc, decoder_dsc, img_coords, clipped_img_area)) return;',1)
            if func=='lv_draw_buf_copy':
                renamed=renamed.replace('{','{\n    if(glass_video_blit_copy(dest, dest_area, src, src_area)) return;',1)
            if func=='lv_draw_sw_fill':
                renamed=renamed.replace('{','{\n    if(glass_video_blit_fill(draw_unit, dsc, coords)) return;',1)
            wrapper=signature+'''\n{
    uint64_t glass_started = glass_video_profile_now();
    '''+func+'_glass_inner('+', '.join(names)+''');
    glass_video_profile_add('''+str(kind)+''', glass_started);
}'''
            backup.write_bytes(path.read_bytes())
            source=HEADER+'\n'+source.lstrip('\ufeff').replace(body,renamed+'\n\n'+wrapper,1)
            path.write_text(source)
        elif '\n\ufeff/**' in source:
            # An upstream UTF-8 BOM belongs at byte zero, never after headers.
            source=source.replace('\n\ufeff/**','\n/**',1)
            path.write_text(source)
        assert source.count(func+'_glass_inner(')==2,name
        receipts[name]=hashlib.sha256(path.read_bytes()).hexdigest()
    return receipts
