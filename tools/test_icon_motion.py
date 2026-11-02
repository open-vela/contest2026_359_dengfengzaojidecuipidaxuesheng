#!/usr/bin/env python3
"""Build/run the real LVGL icon motion regression using a prepared host cache."""
import argparse
from pathlib import Path
import subprocess
import os
from render_refined_home import png565

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--apps',type=Path,required=True)
    p.add_argument('--cache',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();root=Path(__file__).resolve().parents[1]
    a.output.mkdir(parents=True,exist_ok=True)
    desktop=root/'app/espdl-quickapp/overlay/apps/system/desktop'
    command=['gcc','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
             '-fno-omit-frame-pointer','-pthread','-DLV_CONF_INCLUDE_SIMPLE',
             '-I'+str(a.cache),'-I'+str(a.apps/'graphics/lvgl'),'-I'+str(desktop),
             str(root/'app/espdl-quickapp/tests/render_icon_motion.c'),
             *map(str,a.cache.glob('lvgl-*.o')),'-lm','-o',str(a.output/'test-icon-motion')]
    subprocess.run(command,check=True)
    env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1';env['UBSAN_OPTIONS']='halt_on_error=1:print_stacktrace=1'
    subprocess.run([str(a.output/'test-icon-motion'),str(a.output)],check=True,env=env)
    for path in a.output.glob('*.rgb565'):png565(path)
    # Animation frames come from the actual LVGL renderer and timer handler.
    try:
        from PIL import Image
    except ImportError:
        print('Native frames verified; Pillow unavailable for optional GIF export')
        return
    for prefix, filename in [('frame', 'motion-preview.gif'), ('semantic', 'semantic-preview.gif'),
                             *[(f'family-{page}',f'family-{page}.gif') for page in range(4)]]:
        frames=[Image.open(path).convert('RGB') for path in sorted(a.output.glob(prefix+'-*.png'))]
        if frames:
            frames[0].save(a.output/filename,save_all=True,append_images=frames[1:],
                           duration=80 if prefix.startswith('family-') else 40,optimize=True,
                           **({'loop':0} if prefix.startswith('family-') else {}))

if __name__=='__main__':main()
