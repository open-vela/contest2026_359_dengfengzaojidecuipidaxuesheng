#!/usr/bin/env python3
"""Compile the shared Material motion design into native LVGL masks."""
from pathlib import Path
import struct
import generate_glass_icons as icons
from material_icon_shapes import ACCENTS, FILLED, RUNNING
from icon_motion_profiles import profile

ROOT = Path(__file__).resolve().parents[1]
SIZES = [14,16,20,24,32,64]


def rle_encode(data):
    """LVGL byte-block RLE, preserving every A8 coverage value."""
    result=bytearray();pos=0
    while pos<len(data):
        end=pos+1
        while end<len(data) and data[end]==data[pos] and end-pos<127:end+=1
        if end-pos>=3:
            result.extend((end-pos,data[pos]));pos=end;continue
        start=pos;pos=end
        while pos<len(data) and pos-start<127:
            end=pos+1
            while end<len(data) and data[end]==data[pos] and end-pos<127:end+=1
            if end-pos>=3:break
            pos=min(end,start+127)
        result.append(0x80|pos-start);result.extend(data[start:pos])
    return bytes(result)


def rle_decode(data):
    result=bytearray();pos=0
    while pos<len(data):
        count=data[pos];pos+=1
        if count&0x80:
            size=count&0x7f;result.extend(data[pos:pos+size]);pos+=size
        else:result.extend(bytes([data[pos]])*count);pos+=1
    return bytes(result)


def generate(fixture=None):
    masks, layers, symbols = bytearray(), [], []
    mask_offsets = {}
    for name, code in sorted(icons.SYMBOLS.items(), key=lambda item: item[1]):
        # Only the 59 registered Mijia appliances have motion. System icons
        # keep their static font glyphs and do not carry animation masks.
        if not name.startswith('home_'):
            continue
        first = len(layers)
        shapes = icons.I[name]
        gesture = profile(name, len(shapes))
        moving = gesture["moving"]
        accents = ACCENTS.get(name, set())
        running=RUNNING[name]
        groups={}
        for i, shape in enumerate(shapes):
            flags=int(i in accents) | (2 if i in moving else 0)
            if i in running['moving']:flags|=16
            if i in running.get('idle_hidden',set()):flags|=4096
            if running.get('natural_accent') and i in accents:flags|=16384
            if i in gesture['reverse']:flags|=4
            delay = gesture['delays'].get(i, 0)
            run_delay = running['delays'].get(i, 0)
            groups.setdefault((flags, delay, run_delay),[]).append(shape)
        # Closed contours are outlines, not an instruction to flood interiors.
        group_order=sorted(groups)
        extras={}
        for shape,flags in running['extra']:extras.setdefault(flags,[]).append(shape)
        order=[(f,0,0) for f in extras if f&96]+[(f,0,0) for f in extras if f&0x0f00]+[(8,0,0)]+group_order+[(f,0,0) for f in extras if not f&0x0f00 and not f&96]
        for size in SIZES:
            for flags, delay, run_delay in order:
                mask=icons.raster(name,size,extras[flags] if flags in extras else [FILLED[name]] if flags == 8 else groups[(flags, delay, run_delay)])
                if flags==32:
                    # Soft falloff to the floor, compiled once into the mask.
                    for y in range(size):
                        fade=max(0,min(1,(22-y*24/size)/11))
                        for x in range(size):mask.putpixel((x,y),round(mask.getpixel((x,y))*fade))
                box=mask.getbbox() or (0,0,1,1)
                cropped=mask.crop(box);w,h=cropped.size;stride=(w+3)//4*4
                data=cropped.tobytes()
                packed=b''.join(data[row*w:(row+1)*w]+bytes(stride-w) for row in range(h))
                if packed not in mask_offsets:
                    mask_offsets[packed]=len(masks)
                    masks.extend(packed)
                layers.append((mask_offsets[packed],flags,box[0],box[1],w,h,stride,delay,run_delay))
        symbols.append((code,first,len(order),gesture['kind'],*gesture['pivot'],*gesture['direction'],running['kind'],running['period'],*running['pivot'],*running['direction'],running.get('color',0)))
    compact=bytearray();compact_rows={};compressed_count=0
    for data,offset in mask_offsets.items():
        encoded=rle_encode(data)
        assert rle_decode(encoded)==data
        encoded=struct.pack('<III',1,len(encoded),len(data))+encoded
        compressed=len(encoded)<len(data)
        payload=encoded if compressed else data
        compact_rows[offset]=(len(compact),len(payload),compressed)
        compact.extend(payload);compressed_count+=int(compressed)
    if fixture:
        fixture.parent.mkdir(parents=True,exist_ok=True)
        with fixture.open('wb') as stream:
            stream.write(struct.pack('<4sI',b'RLE1',len(mask_offsets)))
            for data in mask_offsets:
                encoded=rle_encode(data)
                stream.write(struct.pack('<II',len(data),len(encoded)))
                stream.write(encoded);stream.write(data)
    out = ['/* SPDX-License-Identifier: Apache-2.0 */',
           '/* Google Material Symbols Rounded; adapted A8 gesture layers. */',
           '/* Source and Apache-2.0 license: materials/material-symbols/. */',
           '/* LVGL RLE is lossless; small masks retain raw A8 when smaller. */',
           '#if LV_USE_RLE',
           'static const unsigned char icon_mask_pixels[] = {']
    out += ['  '+','.join(f'0x{x:02x}' for x in compact[i:i+32])+',' for i in range(0,len(compact),32)]
    out += ['};','static const lv_image_dsc_t icon_mask_images[] = {']
    for offset,_,_,_,w,h,stride,_,_ in layers:
        packed_offset,length,compressed=compact_rows[offset]
        flags='LV_IMAGE_FLAGS_COMPRESSED' if compressed else '0'
        out.append('  {.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,'
            f'.flags={flags},.w={w},.h={h},.stride={stride}'+'},'
            f'.data_size={length},.data=icon_mask_pixels+{packed_offset}'+'},')
    out += ['};','#else','static const unsigned char icon_mask_pixels[] = {']
    out += ['  '+','.join(f'0x{x:02x}' for x in masks[i:i+32])+',' for i in range(0,len(masks),32)]
    out += ['};','static const lv_image_dsc_t icon_mask_images[] = {']
    out += ['  {.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_A8,'
            f'.w={w},.h={h},.stride={stride}'+'},'
            f'.data_size={stride*h},.data=icon_mask_pixels+{offset}'+'},' for offset,_,_,_,w,h,stride,_,_ in layers]
    out += ['};','#endif', 'static const struct icon_layer icon_layers[] = {']
    out += [f'  {{&icon_mask_images[{i}],{flags},{x},{y},{delay},{run_delay}}},' for i,(_,flags,x,y,_,_,_,delay,run_delay) in enumerate(layers)]
    out += ['};', 'static const struct icon_symbol icon_symbols[] = {']
    out += ['  {0x%x,%s},' % (row[0],','.join(map(str,row[1:]))) for row in symbols]
    out += ['};', '']
    target = ROOT/'app/espdl-quickapp/overlay/apps/system/desktop/glass_icon_geometry.inc'
    target.write_text('\n'.join(out), encoding='utf-8')
    print(f'{len(symbols)} native animated outlines, {len(layers)} A8 layers, {len(masks)} raw bytes / {len(compact)} lossless packed bytes ({compressed_count} RLE masks)')


if __name__ == '__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rle-fixture',type=Path)
    generate(parser.parse_args().rle_fixture)
