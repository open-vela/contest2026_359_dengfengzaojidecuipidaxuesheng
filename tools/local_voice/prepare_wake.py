#!/usr/bin/env python3
"""Externalize only generated tensor constants; retain SDK MFCC/inference."""
import argparse
import hashlib
import json
from pathlib import Path,PurePosixPath
import re
import struct
import zipfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--archive',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--release',type=Path,required=True)
    a=p.parse_args();original=a.output/'original';external=a.output/'external'
    original.mkdir(parents=True,exist_ok=True);external.mkdir(exist_ok=True)
    with zipfile.ZipFile(a.archive) as z:
        for info in z.infolist():
            rel=PurePosixPath(info.filename)
            if rel.is_absolute() or '..' in rel.parts or any(':' in part for part in rel.parts):raise ValueError('Unsafe ZIP path')
            if info.is_dir():continue
            path=original/Path(*rel.parts);path.parent.mkdir(parents=True,exist_ok=True)
            raw=z.read(info)
            if path.exists() and path.read_bytes()!=raw:raise ValueError('Preserve a different SDK snapshot')
            path.write_bytes(raw)
    source=original/'tflite-model/tflite_learn_1153216_4_compiled.cpp';text=source.read_text(encoding='utf-8')
    blob=bytearray();bindings=[];constants=[]
    pattern=re.compile(r'const MODEL_SECTION\(EI_MODEL_SECTION\) ALIGN\(\d+\) (float|int32_t) (tensor_data\d+)\[([^]]+)\] = \{(.*?)\};',re.S)
    def replace(match):
        kind,name,shape,values=match.groups();dimensions=list(map(int,shape.split('*')));count=1
        for d in dimensions:count*=d
        values=re.sub(r'/\*.*?\*/','',values,flags=re.S)
        tokens=[t.strip() for t in values.split(',') if t.strip()]
        if len(tokens)!=count:raise ValueError('Tensor constant size mismatch: '+name)
        data=struct.pack('<'+('f' if kind=='float' else 'i')*count,*[(float(t.rstrip('f')) if kind=='float' else int(t)) for t in tokens])
        while len(blob)%16:blob.append(0)
        offset=len(blob);blob.extend(data);constants.append(dict(name=name,type=kind,offset=offset,bytes=len(data)))
        bindings.append(f'  g0::{name}=reinterpret_cast<const {kind} *>(data+{offset});')
        return f'const {kind} *{name}=nullptr;'
    changed,count=pattern.subn(replace,text)
    if count!=11:raise ValueError('Unexpected EON constants: '+str(count))
    # TensorInfo holds captured pointers, so refresh its constant descriptors
    # before init. The generated operators and their options stay untouched.
    pointer_rows=[]
    tensor_table=re.search(r'TensorInfo_t tensorData\[\].*?=\s*\{(.*?)\n\};',changed,re.S)
    if not tensor_table:
        tensor_table=re.search(r'TensorInfo_t tensorData\[[^]]*\]\s*=\s*\{(.*?)\n\};',changed,re.S)
    if not tensor_table:raise ValueError('Missing generated tensor directory')
    rows=[line for line in tensor_table.group(1).splitlines() if line.strip().startswith('{')]
    for index,line in enumerate(rows):
        constant=re.search(r'g0::(tensor_data\d+)',line)
        if constant:pointer_rows.append(f'  tensorData[{index}].data=const_cast<void *>(reinterpret_cast<const void *>(g0::{constant[1]}));')
    if not pointer_rows:raise ValueError('No external tensor bindings')
    appended='\nextern "C" int glv_eon_bind(const unsigned char *data,size_t size) {\n'
    appended+=f'  if(!data||size!={len(blob)}||reinterpret_cast<uintptr_t>(data)%16)return -1;\n'
    appended+='\n'.join(bindings+pointer_rows)+'\n  return 0;\n}\n'
    target=external/'tflite_learn_1153216_4_compiled.cpp';target.write_text(changed+appended,encoding='utf-8',newline='\n')
    wake=a.release/'wake';wake.mkdir(exist_ok=True)
    (wake/'weights.bin').write_bytes(blob)
    metadata=dict(format='EON_EXTERNAL1',sample_rate=16000,window_samples=32000,labels=['blank','noise','wake_word'],wake_phrase='你好 openvela',constants=constants,
                  original_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),external_sha256=hashlib.sha256(target.read_bytes()).hexdigest(),weights_sha256=hashlib.sha256(blob).hexdigest())
    (wake/'model.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    (a.output/'PREPARATION.json').write_text(json.dumps(metadata,ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps(dict(constants=count,weights_bytes=len(blob),bound_tensors=len(pointer_rows))),flush=True)


if __name__=='__main__':main()
