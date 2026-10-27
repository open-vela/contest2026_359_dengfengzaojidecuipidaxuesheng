#!/usr/bin/env python3
"""Create deterministic JPEG parser fixtures; requires Pillow, no device I/O."""
from pathlib import Path
import argparse
import hashlib
import json
from PIL import Image, JpegImagePlugin

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
cases=[]
def record(path,supported=True,reason='baseline',dimensions=None,sampling=None,components=None):
 if dimensions is None:
  with Image.open(path) as image:
   dimensions=image.size;components=len(image.getbands())
   sampling=JpegImagePlugin.get_sampling(image)
 cases.append({'file':path.name,'supported':supported,'reason':reason,
  'width':dimensions[0],'height':dimensions[1],'components':components,
  'sampling':sampling,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
for w,h in [(32,24),(30,24),(48,17)]:
 image=Image.new('RGB',(w,h))
 image.putdata([((x*17+y*3)%256,(x*5+y*19)%256,(x*11+y*7)%256)
                for y in range(h) for x in range(w)])
 for sampling in range(3):
  path=out/f'rgb-{w}x{h}-s{sampling}.jpg'
  image.save(path,'JPEG',quality=85,subsampling=sampling,optimize=False)
  record(path)
for mode in ['L','CMYK']:
 path=out/(mode+'.jpg');Image.new(mode,(32,24)).save(path,'JPEG',quality=85)
 record(path,mode=='L','grayscale' if mode=='L' else 'unsupported four components')
path=out/'optimized.jpg'
image.save(path,'JPEG',quality=85,subsampling=2,optimize=True)
record(path,True,'optimized Huffman tables')
path=out/'progressive.jpg';Image.new('RGB',(32,24)).save(path,'JPEG',progressive=True)
record(path,False,'unsupported progressive')
path=out/'odd-area.jpg';Image.new('RGB',(31,23)).save(path,'JPEG')
record(path,False,'pinned SDK requires pixel count divisible by eight')
path=out/'too-wide.jpg';Image.new('RGB',(4097,8)).save(path,'JPEG')
record(path,False,'native driver dimension bound')
source=(out/'rgb-32x24-s2.jpg').read_bytes()
def strip_dht(data,only_chroma=False):
 result=bytearray(data[:2]);pos=2
 while pos<len(data):
  assert data[pos]==255
  marker=data[pos+1]
  if marker==0xda:
   result.extend(data[pos:]);return bytes(result)
  n=int.from_bytes(data[pos+2:pos+4],'big');end=pos+2+n
  if marker!=0xc4 or (only_chroma and not (data[pos+4]&15)):
   result.extend(data[pos:end])
  pos=end
 raise AssertionError('missing scan')
path=out/'no-dht.jpg';path.write_bytes(strip_dht(source));record(path,True,'MJPEG standard Huffman fallback')
path=out/'partial-dht.jpg';path.write_bytes(strip_dht(source,True))
record(path,False,'referenced chroma Huffman table missing',dimensions=(32,24),sampling=2,components=3)
official=out/'official-no-huff.jpg'
if official.exists():record(official,True,'pinned official SDK fixture')
(out/'FIXTURES.json').write_text(json.dumps({'pillow_version':Image.__version__,
 'generator_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'cases':cases},indent=2)+'\n')
print('JPEG_FIXTURES_GENERATED',len(cases))
