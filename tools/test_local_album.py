#!/usr/bin/env python3
"""Exercise album SD browsing / JPEG workers with real decoder and ASan."""
import os
import struct
import zlib
from pathlib import Path
import subprocess
import tempfile

repo=Path(__file__).resolve().parents[1]
desktop=repo/'app/espdl-quickapp/overlay/apps/system/desktop'
with tempfile.TemporaryDirectory(prefix='local-album-') as tmp:
    root=Path(tmp);sd=root/'sdcard';sd.mkdir();(sd/'many').mkdir()
    local=root/'photos';local.mkdir()
    header=bytearray(66);header[:2]=b'BM'
    for offset,value in [(2,307266),(10,66),(14,40),(18,512),(22,(-300)&0xffffffff),(30,3),(34,307200),(54,0xf800),(58,0x7e0),(62,0x1f)]:struct.pack_into('<I',header,offset,value)
    struct.pack_into('<HH',header,26,1,16)
    camera=header+b''.join(struct.pack('<H',0xf800 if y<150 else 0x001f)*512 for y in range(300))
    (sd/'camera.bmp').write_bytes(camera);(local/'camera.bmp').write_bytes(camera)
    header=bytearray(54);header[:2]=b'BM'
    for offset,value in [(2,174),(10,54),(14,40),(18,7),(22,5),(34,120)]:struct.pack_into('<I',header,offset,value)
    struct.pack_into('<HH',header,26,1,24)
    (sd/'bottomup.bmp').write_bytes(header+b''.join((b'\xff\0\0' if y<3 else b'\0\0\xff')*7+b'\0'*3 for y in range(5)))
    (sd/'bad.bmp').write_bytes(b'broken')
    for name,size in [('wide.jpg','1280x720'),('portrait.jpg','720x1280')]:
        subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-f','lavfi','-i',f'testsrc2=size={size}:rate=1',
                        '-frames:v','1','-q:v','4',str(sd/name)],check=True)
    (sd/'clip.MP4').write_bytes(b'fixture')
    (sd/'corrupt.jpg').write_bytes(b'broken JPEG')
    (sd/'huge.jpg').write_bytes(b'\0'*(2*1024*1024+1))
    def png(path,w,h,color,rows,extra=b''):
        def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
        path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,color,0,0,0))+extra+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b''))
    png(sd/'alpha.png',7,5,6,b''.join(b'\0'+(b'\xff\0\0\xff'+b'\0\0\0\0'+b'\0\0\xff\x80'*5) for _ in range(5)))
    png(sd/'too-large.png',2048,2048,6,b'')
    (sd/'broken.png').write_bytes(b'broken PNG')
    (sd/'phone.HEIC').write_bytes(struct.pack('>I4s4sI4s',20,b'ftyp',b'heic',0,b'mif1'))
    (sd/'clip.MOV').write_bytes(struct.pack('>I4s4sI4s',20,b'ftyp',b'qt  ',0,b'qt  '))
    (sd/'renamed.bin').write_bytes((sd/'wide.jpg').read_bytes())
    (sd/'wrong.png').write_bytes((sd/'wide.jpg').read_bytes())
    (sd/'.hidden.jpg').write_bytes(b'hidden')
    (sd/'linked.jpg').symlink_to(sd/'wide.jpg')
    (sd/'empty.jpg').write_bytes(b'')
    for i in range(270):(sd/'many'/f'{i:03}.jpg').write_bytes(b'')
    library=local/'library';(library/'a'/'deep').mkdir(parents=True);(library/'b').mkdir()
    (library/'a'/'same.jpg').write_bytes((sd/'wide.jpg').read_bytes())
    (library/'b'/'same.jpg').write_bytes((sd/'portrait.jpg').read_bytes())
    (library/'a'/'deep'/'clip.MP4').write_bytes(b'fixture')
    (library/'b'/'clip.MOV').write_bytes((sd/'clip.MOV').read_bytes())
    (library/'a'/'hidden-link').symlink_to(library/'b',target_is_directory=True)
    (library/'b'/'loop').symlink_to(library,target_is_directory=True)
    (library/'.hidden.jpg').write_bytes(b'')
    subprocess.run(['cc','-D_GNU_SOURCE','-DALBUM_HOST_TEST','-DALBUM_ROOT="'+str(sd)+'"',
                    '-DALBUM_LOCAL_ROOT="'+str(local)+'"','-std=gnu11','-O1','-g','-pthread','-Wall','-Wextra','-Wno-misleading-indentation',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer',
                    '-I'+str(repo/'app/bilibili_live'),'-I'+str(desktop),
                    str(repo/'app/bilibili_live/tests/test_local_album.c'),str(repo/'app/bilibili_live/album_formats.c'),str(repo/'app/bilibili_live/album_png.c'),
                    str(desktop/'qpk_mjpeg.c'),str(desktop/'qpk_tjpgd.c'),'-o',str(root/'test')],check=True)
    env=os.environ.copy();env['ASAN_OPTIONS']='detect_leaks=1'
    subprocess.run([str(root/'test')],check=True,env=env,timeout=45)
