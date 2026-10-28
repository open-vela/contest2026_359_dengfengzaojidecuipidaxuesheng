#!/usr/bin/env python3
"""Compare every patch-touched portable source with the prepared ESP source tree."""
import argparse,hashlib,json,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--core',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[1]
reference=root/'deps/netsurf-modern';prepared=a.core/'deps/netsurf-modern'
patches=sorted((root/'patches').glob('*.patch'));paths={}
for patch in patches:
    for path in re.findall(r'^\+\+\+ b/([^\r\n]+)',patch.read_text(),re.M):
        paths.setdefault(path,[]).append(patch.name)
files=[]
for name,contributors in sorted(paths.items()):
    src,dst=reference/name,prepared/name
    item={'path':name,'patches':contributors,'reference_exists':src.is_file(),'esp_exists':dst.is_file()}
    if src.is_file() and dst.is_file():
        item.update(reference_sha256=hashlib.sha256(src.read_bytes()).hexdigest(),
                    esp_sha256=hashlib.sha256(dst.read_bytes()).hexdigest())
        item['identical']=item['reference_sha256']==item['esp_sha256']
        left,right=src.read_text(),dst.read_text()
        if item['identical']:
            item['portable_equivalent']=True
        elif left==right:
            item.update(portable_equivalent=True,difference='CRLF/LF only')
        elif name=='libcss/src/parse/properties/properties.gen' and left.replace(
                'CODE:0,SPEAK_PUNCTUATION_CODE','LIBCSS_CODE:0,SPEAK_PUNCTUATION_CODE')==right:
            item.update(portable_equivalent=True,difference='NuttX CODE macro collision renamed to LIBCSS_CODE')
    files.append(item)
report={'patch_count':len(patches),'touched_file_count':len(files),
        'identical_files':sum(f.get('identical',False) for f in files),'files':files,
        'portable_equivalent_files':sum(f.get('portable_equivalent',False) for f in files),
        'scope':'source identity only; does not prove that each source was rebuilt into the installed ELF'}
a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k!='files'},indent=2))
for item in files:
    if not item.get('identical'):print('DIFF_OR_MISSING',item['path'])
