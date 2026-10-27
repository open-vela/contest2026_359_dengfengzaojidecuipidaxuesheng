#!/usr/bin/env python3
"""Independently compare native JPEG headers with libjpeg/Pillow tables."""
from pathlib import Path
from io import BytesIO
import argparse,json
from PIL import Image,JpegImagePlugin
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
a=p.parse_args();cases=[]
for path in sorted(a.output.glob('header-s*-q*.jpg')):
 sampling=int(path.stem.split('-')[1][1:]);quality=int(path.stem.split('-')[2][1:])
 reference=BytesIO();Image.new('L' if sampling==3 else 'RGB',(30,24)).save(reference,'JPEG',quality=quality,subsampling=sampling if sampling<3 else 0)
 with Image.open(reference) as expected,Image.open(path) as actual:
  assert actual.size==expected.size==(30,24)
  assert actual.quantization==expected.quantization,(path,'quantization')
  assert actual.mode==expected.mode
  assert JpegImagePlugin.get_sampling(actual)==JpegImagePlugin.get_sampling(expected)
 cases.append(path.name)
assert len(cases)==24
(a.output/'PILLOW.json').write_text(json.dumps({'passed':True,'cases':cases,'pillow':Image.__version__},indent=2)+'\n')
print('JPEG_HEADER_LIBJPEG_OK',len(cases))
