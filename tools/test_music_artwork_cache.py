#!/usr/bin/env python3
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[1];desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
with tempfile.TemporaryDirectory(prefix='music-artwork-format-') as tmp:
    out=Path(tmp)
    subprocess.run(['cc','-D_GNU_SOURCE','-std=gnu11','-O1','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-DMUSIC_CACHE_ROOT="'+str(out/'cache')+'"','-I'+str(desktop),
                    str(repo/'app/espdl-quickapp/tests/music_artwork_cache_host.c'),'-o',str(out/'test')],check=True)
    subprocess.run([str(out/'test')],check=True,timeout=15)
