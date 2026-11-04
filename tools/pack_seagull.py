#!/usr/bin/env python3
"""Regenerate lossless RGB565+A8 bird planes from the supplied RGBA frames."""
from pathlib import Path
import argparse,hashlib,json

def rle(data,unit):
    values=[data[i:i+unit] for i in range(0,len(data),unit)];out=bytearray();i=0
    while i<len(values):
        run=1
        while i+run<len(values) and run<128 and values[i+run]==values[i]:run+=1
        if run>=3:out.append(0x80|(run-1));out.extend(values[i]);i+=run
        else:
            start=i;i+=run
            while i<len(values) and i-start<128:
                run=1
                while i+run<len(values) and run<3 and values[i+run]==values[i]:run+=1
                if run>=3:break
                i+=min(run,128-(i-start))
            out.append(i-start-1);out.extend(b''.join(values[start:i]))
    return bytes(out)

def unpack(data,unit):
    out=bytearray();i=0
    while i<len(data):
        tag=data[i];i+=1;n=(tag&127)+1
        if tag&128:out.extend(data[i:i+unit]*n);i+=unit
        else:out.extend(data[i:i+n*unit]);i+=n*unit
    return bytes(out)

def main():
    root=Path(__file__).resolve().parents[1];p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--input',type=Path,default=root/'app/espdl-quickapp/assets/seagull')
    p.add_argument('--output',type=Path,required=True,help='Explicit output; never overwrite firmware sources implicitly')
    p.add_argument('--check',type=Path,help='Compare with an existing C source, ignoring CRLF/LF')
    a=p.parse_args();frames=sorted(a.input.glob('*.rgba'));assert frames,'No RGBA input frames'
    rows=[];code=['/* Generated lossless pack: pack_seagull.py. RGB565+A8 planes. */','#include "glass_seagull.h"','']
    for f in frames:
        rgba=f.read_bytes();assert len(rgba)==128*128*4,f
        rgb=bytearray();alpha=bytearray()
        for i in range(0,len(rgba),4):
            r,g,b,v=rgba[i:i+4];c=(r>>3)<<11|(g>>2)<<5|(b>>3);rgb.extend((c&255,c>>8));alpha.append(v)
        rc,ac=rle(rgb,2),rle(alpha,1);assert unpack(rc,2)==rgb and unpack(ac,1)==alpha
        for suffix,data in [('rgb',rc),('alpha',ac)]:
            code.append(f'static const uint8_t f_{f.stem}_{suffix}[] = {{')
            code.extend(' '+','.join(f'0x{x:02x}' for x in data[i:i+24])+',' for i in range(0,len(data),24));code.append('};')
        rows.append({'key':f.stem,'packed_bytes':len(rc)+len(ac),'raw_bytes':len(rgb)+len(alpha),'sha256':hashlib.sha256(rgb+alpha).hexdigest()})
    code.append('const struct seagull_packed_frame seagull_packed_frames[SEAGULL_PACKED_COUNT] = {')
    for row in rows:
        k=row['key'];code.append(f' {{ f_{k}_rgb, sizeof(f_{k}_rgb), f_{k}_alpha, sizeof(f_{k}_alpha) }},')
    code.append('};\n');data='\n'.join(code).encode();a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_bytes(data)
    if a.check:assert a.check.read_bytes().replace(b'\r\n',b'\n')==data,'Generated bird planes differ'
    print(json.dumps({'frames':len(rows),'packed_bytes':sum(x['packed_bytes'] for x in rows),'lossless_plane_roundtrip':True,'source_matched':bool(a.check)}))
if __name__=='__main__':main()
