"""Repair LVGL 9.2 event-vector relocation across callbacks, prepared tree only.

The entry count remains bounded to the initial count, so callbacks added by a
callback cannot extend this dispatch forever. Reacquire each descriptor from
the live array, as in LVGL 9.3, instead of keeping a reallocatable array pointer.
This does not backport 9.3's separate deferred-removal/ABI changes.
"""
import hashlib
from pathlib import Path
from render_all_ui import function

MARKER = '/* GLASS EVENT VECTOR RELOCATION FIX */'


def apply(apps):
    path = Path(apps)/'graphics/lvgl/lvgl/src/misc/lv_event.c'
    source = path.read_text()
    body = function(source, 'lv_event_send')
    if MARKER not in body:
        old = '    lv_event_dsc_t ** dsc = lv_array_front(list);\n'
        anchor = '    for(i = 0; i < size; i++) {\n'
        if body.count(old) != 1 or body.count(anchor) != 1:
            raise RuntimeError('Unrecognized LVGL event dispatcher; review before patching')
        fixed = body.replace(old, '').replace(anchor, anchor + '''        /* GLASS EVENT VECTOR RELOCATION FIX */
        /* Callbacks can grow/reallocate or shrink the event vector. Never
         * retain its data pointer across a callback. Object deletion is
         * checked below before touching the list again. */
        lv_event_dsc_t * dsc = lv_event_get_dsc(list, i);
        if(dsc == NULL) break;
''').replace('dsc[i]->', 'dsc->')
        source = source.replace(body, fixed, 1)
        path.write_text(source)
    else:
        if 'lv_array_front(list)' in body or 'dsc[i]' in body:
            raise RuntimeError('Incomplete LVGL event-vector repair')
    return {'misc/lv_event.c': hashlib.sha256(path.read_bytes()).hexdigest()}
