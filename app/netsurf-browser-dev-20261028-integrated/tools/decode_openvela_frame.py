#!/usr/bin/env python3
"""Decode immutable NSH RGB565 RLE chunks; refuse incomplete screen readback."""
import argparse,hashlib,json,re,struct,zlib
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--log',type=Path,action='append',required=True)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--missing',type=Path,required=True)
a=p.parse_args(); chunks={}; sizes=set()
for log in a.log:
    text=log.read_text(encoding='utf-8',errors='replace')
    sizes.update(map(int,re.findall(r'FRAME_DATA (\d+)',text)))
    for offset,hexdata in re.findall(r'F([0-9a-fA-F]{8}):([0-9a-fA-F]+)',text):
        if len(hexdata)%2==0:chunks[int(offset,16)]=bytes.fromhex(hexdata)
if len(sizes)!=1:raise RuntimeError('missing or inconsistent immutable frame size')
size=sizes.pop()
missing=[off for off in range(0,size,64) if len(chunks.get(off,b''))!=min(64,size-off)]
a.missing.write_text(json.dumps(missing)+'\n')
if missing:
    print('INCOMPLETE_FRAME',len(missing),'chunks; request desktop frame-chunk:<offset>');raise SystemExit(2)
data=b''.join(chunks[off] for off in range(0,size,64))
if size%4:raise RuntimeError('invalid RLE length')
rgb=bytearray()
for count,value in struct.iter_unpack('<HH',data):
    if not count or len(rgb)+count*3>1024*600*3:raise RuntimeError('invalid RLE run')
    colour=bytes([((value>>11)&31)*255//31,((value>>5)&63)*255//63,(value&31)*255//31])
    rgb.extend(colour*count)
if len(rgb)!=1024*600*3:raise RuntimeError('incomplete pixel count')
def chunk(kind,payload):return struct.pack('!I',len(payload))+kind+payload+struct.pack('!I',zlib.crc32(kind+payload))
rows=b''.join(b'\x00'+rgb[y*3072:(y+1)*3072] for y in range(600))
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('!IIBBBBB',1024,600,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')
a.output.write_bytes(png)
print('FRAME_DECODED',a.output,'RLE_SHA256',hashlib.sha256(data).hexdigest())
