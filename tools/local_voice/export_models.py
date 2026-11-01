#!/usr/bin/env python3
"""Export the user's fixed Llama/V2 assets; never run scripts from the ZIPs."""
import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import unicodedata
import zipfile

import numpy as np

LLM_SHA='b1edf722e5dcc8968690185e6072c486160e0fcdc9583899002df060ddb37d41'
TTS_SHA='decfc646b814840389fe1dc528b6d42ee588656e3eaaaf582affa81f2b8da464'
RELEASE='edgeidiot-sft2-baker-v2-20261031'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def save(path,data):
    path.parent.mkdir(parents=True,exist_ok=True)
    if path.exists() and path.read_bytes()!=data:
        raise ValueError('Refuse to replace a different asset: '+str(path))
    path.write_bytes(data)


def model_file(folder,tensors,metadata,mode):
    """Small JSON directory plus aligned tensor rows; no pickle on the device."""
    blob=bytearray();rows=[]
    for name,value in sorted(tensors.items()):
        x=np.asarray(value,dtype='<f4')
        if not np.isfinite(x).all():raise ValueError('Nonfinite tensor: '+name)
        while len(blob)%16:blob.append(0)
        shape=list(x.shape);dtype='f32';payload=x.tobytes();row_stride=0
        if x.ndim>=2 and mode=='q4' and x.shape[-1]%32==0:
            a=x.reshape(-1,32)
            scale=np.maximum(np.abs(a).max(1)/7,np.float32(1e-12)).astype('<f4')
            q=np.clip(np.rint(a/scale[:,None]),-7,7).astype(np.int8)
            packed=((q[:,0::2]&15)|((q[:,1::2]&15)<<4)).astype('u1')
            block=np.empty((len(scale),20),dtype='u1')
            block[:,:4]=scale.view('u1').reshape(-1,4);block[:,4:]=packed
            payload=block.tobytes();dtype='q4_32';row_stride=x.shape[-1]//32*20
        elif x.ndim>=2 and mode=='q8':
            # ConvTranspose weights are left in their original [in,out,k]
            # order. The C accessor uses this exact serialized convention.
            a=x.reshape(x.shape[0],-1)
            scale=np.maximum(np.abs(a).max(1)/127,np.float32(1e-12)).astype('<f4')
            q=np.clip(np.rint(a/scale[:,None]),-127,127).astype('i1')
            row=np.empty((len(scale),a.shape[1]+4),dtype='u1')
            row[:,:4]=scale.view('u1').reshape(-1,4);row[:,4:]=q.view('u1')
            payload=row.tobytes();dtype='q8_row';row_stride=a.shape[1]+4
        rows.append(dict(name=name,shape=shape,dtype=dtype,offset=len(blob),bytes=len(payload),row_stride=row_stride))
        blob.extend(payload)
    save(folder/'weights.bin',bytes(blob))
    index=dict(format='GLVM1',metadata=metadata,tensors=rows,data_bytes=len(blob),data_sha256=sha(blob))
    save(folder/'model.json',json.dumps(index,ensure_ascii=False,separators=(',',':')).encode())
    binary_index(folder,index)
    return index


def binary_index(folder,index):
    types={'f32':0,'q4_32':1,'q8_row':2}
    blob=bytearray(struct.pack('<4sIII32s',b'GLM1',1,len(index['tensors']),index['data_bytes'],bytes.fromhex(index['data_sha256'])))
    for row in index['tensors']:
        name=row['name'].encode('ascii');shape=row['shape']
        if len(name)>=80 or len(shape)>4:raise ValueError('Tensor descriptor exceeds ABI')
        blob.extend(struct.pack('<80s4I5I',name,*(shape+[0]*(4-len(shape))),len(shape),types[row['dtype']],row['offset'],row['bytes'],row['row_stride']))
    save(folder/'model.bin',bytes(blob))


def tokenizer_file(folder,source):
    import regex
    from tokenizers import normalizers
    d=json.loads(source);vocab=d['model']['vocab']
    if d['model']['type']!='BPE' or len(vocab)!=8192:raise ValueError('Unexpected tokenizer')
    bs=list(range(33,127))+list(range(161,173))+list(range(174,256));cs=bs.copy();n=0
    for b in range(256):
        if b not in bs:bs.append(b);cs.append(256+n);n+=1
    encode=dict(zip(bs,cs));decode={chr(c):b for b,c in encode.items()}
    entries=[None]*len(vocab);raw=bytearray()
    specials={t['id'] for t in d['added_tokens'] if t.get('special')}
    for token,index in sorted(vocab.items(),key=lambda v:v[1]):
        data=b'' if index in specials else bytes(decode[c] for c in token)
        entries[index]=(len(raw),len(data));raw.extend(data)
    pairs=[]
    for pair in d['model']['merges']:
        a,b=pair if isinstance(pair,list) else pair.split(' ')
        pairs.append((vocab[a],vocab[b],vocab[a+b]))
    # Generate compatibility decompositions with the tokenizer's own Unicode
    # implementation; the device performs canonical ordering/composition.
    nfkd=normalizers.NFKD();maps=[];seq=[];ccc=[];compose=[];ranges=[]
    letter=regex.compile(r'\A\p{L}\Z');number=regex.compile(r'\A\p{N}\Z');space=regex.compile(r'\A\s\Z')
    previous=0;start=0
    for cp in range(0x110000):
        if 0xd800<=cp<=0xdfff:ch=''
        else:ch=chr(cp)
        value=nfkd.normalize_str(ch) if ch else ch
        # Hangul decomposition/composition is algorithmic on the MCU.
        if value!=ch and not 0xac00<=cp<=0xd7a3:
            maps.append((cp,len(seq),len(value)));seq.extend(map(ord,value))
        combining=unicodedata.combining(ch) if ch else 0
        if combining:ccc.append((cp,combining))
        canonical=unicodedata.decomposition(ch) if ch else ''
        if canonical and not canonical.startswith('<'):
            ds=[int(s,16) for s in canonical.split()]
            if len(ds)==2 and unicodedata.normalize('NFC',''.join(map(chr,ds)))==ch:
                compose.append((*ds,cp))
        flags=(1 if letter.fullmatch(ch) else 0)|(2 if number.fullmatch(ch) else 0)|(4 if space.fullmatch(ch) else 0)
        if flags!=previous:
            if previous:ranges.append((start,cp-1,previous))
            start=cp;previous=flags
    if previous:ranges.append((start,0x10ffff,previous))
    compose.sort()
    blob=bytearray(struct.pack('<16I',0x3154564c,1,len(vocab),len(pairs),len(maps),len(seq),len(ccc),len(compose),len(ranges),len(raw),*([0]*6)))
    def ints(rows):
        for row in rows:
            blob.extend(struct.pack('<'+'I'*len(row),*row))
    ints(entries);blob.extend(raw)
    while len(blob)%4:blob.append(0)
    ints([(vocab[chr(encode[b])],) for b in range(256)])
    ints(pairs);ints(maps);ints([(x,) for x in seq]);ints(ccc);ints(compose);ints(ranges)
    save(folder/'tokenizer.bin',bytes(blob))
    save(folder/'tokenizer.source.json',source)
    return dict(vocab=len(vocab),merges=len(pairs),bytes=len(blob),unicode_version=unicodedata.unidata_version)


def export(args):
    import torch
    from safetensors.torch import load
    from tokenizers import Tokenizer
    root=args.output.resolve()/RELEASE
    if (root/'manifest.json').exists():raise ValueError('Release already complete; choose a fresh output')
    root.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.llm_zip) as z:
        def read(suffix):return z.read(next(n for n in z.namelist() if n.endswith('/'+suffix)))
        raw=read('model/model.safetensors')
        if sha(raw)!=LLM_SHA:raise ValueError('Wrong EdgeIdiot checkpoint')
        tensors=load(raw);del raw
        config=json.loads(read('model/config.json'))
        expected=dict(hidden_size=512,intermediate_size=1408,num_hidden_layers=12,num_attention_heads=8,num_key_value_heads=2,vocab_size=8192,head_dim=64)
        if any(config.get(k)!=v for k,v in expected.items()):raise ValueError('Unsupported Llama architecture')
        llm=model_file(root/'edgeidiot',{k:v.float().numpy() for k,v in tensors.items()},config,'q4')
        del tensors
        tokenizer_info=tokenizer_file(root/'edgeidiot',read('model/tokenizer.json'))
        save(root/'edgeidiot/chat_template.jinja',read('model/chat_template.jinja'))
        tok=Tokenizer.from_str(read('model/tokenizer.json').decode())
        tests=['你好','今天天气怎么样？','我在学习中文。','价格是１２３元。','Cafe\u0301，ＡＢＣ！','你好\n\n世界','  hello  world  ','I\'m testing 123.','👩\u200d💻 😀','你好 openvela']
        save(root/'checks/tokenizer.json',json.dumps([dict(text=t,ids=tok.encode(t,add_special_tokens=False).ids) for t in tests],ensure_ascii=False,indent=2).encode())
        save(root/'notices/edgeidiot-provenance.json',read('model-provenance.json'))
    if sha(args.tts.read_bytes())!=TTS_SHA:raise ValueError('Wrong V2 voice checkpoint')
    ck=torch.load(args.tts,map_location='cpu',weights_only=True)
    if ck['sample_rate']!=24000 or ck['vocoder_config']['channels']!=208:raise ValueError('Unexpected V2 voice')
    metadata={k:ck[k] for k in ['format_version','sample_rate','acoustic_config','vocoder_config','metadata','vocab','lexicon']}
    metadata['controls']=dict(speed=.9,volume=1.,pause_scale=1.2,vowel_scale=1.05,pitch_semitones=0.,intonation_scale=1.,pad_start_ms=125.,pad_end_ms=0.)
    state={f'acoustic.{k}':v.float().numpy() for k,v in ck['acoustic_state'].items()}
    state.update({f'vocoder.{k}':v.float().numpy() for k,v in ck['vocoder_state'].items()})
    tts=model_file(root/'tts-v2',state,metadata,'q8')
    save(root/'tts-v2/voice.json',json.dumps(metadata,ensure_ascii=False,separators=(',',':')).encode())
    print(json.dumps(dict(release=str(root),llm_bytes=llm['data_bytes'],tts_bytes=tts['data_bytes'],tokenizer=tokenizer_info,tts_acoustic_config=ck['acoustic_config']),ensure_ascii=False),flush=True)
    manifest=dict(format='LOCALVOICE1',runtime_abi=1,release=RELEASE,context_tokens=512,max_new_tokens=96,models_ready=['edgeidiot','tts-v2'],asr_ready=False,source=dict(llm_sha256=LLM_SHA,tts_sha256=TTS_SHA),files=[])
    for p in sorted(root.rglob('*')):
        if p.is_file():manifest['files'].append(dict(path=p.relative_to(root).as_posix(),bytes=p.stat().st_size,sha256=sha(p.read_bytes())))
    save(root/'manifest.json',json.dumps(manifest,ensure_ascii=False,indent=2).encode())


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--llm-zip',type=Path,required=True);p.add_argument('--tts',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    export(p.parse_args())
