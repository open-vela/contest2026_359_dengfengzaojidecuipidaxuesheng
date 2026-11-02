#!/usr/bin/env python3
"""Freeze the V2 pypinyin/phoneme dictionaries into a bounded SD lookup table."""
import argparse
import hashlib
import importlib.metadata
import json
from pathlib import Path
import re
import struct
import sys


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--release',type=Path,required=True);p.add_argument('--tts-source',type=Path,required=True)
    a=p.parse_args();sys.path.insert(0,str(a.tts_source))
    from pypinyin import lazy_pinyin,Style
    from pypinyin.pinyin_dict import pinyin_dict
    from pypinyin.phrases_dict import phrases_dict
    from baker4m.infer import syllable_phones,frontend
    voice=json.loads((a.release/'tts-v2/voice.json').read_text());vocab=voice['vocab'];lexicon=voice['lexicon']
    def ids(text):
        result=[]
        for syllable in lazy_pinyin(text,style=Style.TONE3,neutral_tone_with_five=True,errors=lambda s:list(s)):
            try:result.extend(vocab[p] for p in syllable_phones(syllable,lexicon,vocab))
            except ValueError:return None
        return result
    characters=[];phrases=[];cp_data=[];phone_data=[]
    for cp in sorted(pinyin_dict):
        values=ids(chr(cp))
        if values:characters.append((cp,len(phone_data),len(values)));phone_data.extend(values)
    for text in sorted(phrases_dict):
        values=ids(text)
        if not values or len(text)>64:continue
        phrases.append((ord(text[0]),len(text),len(cp_data),len(phone_data),len(values)))
        cp_data.extend(map(ord,text));phone_data.extend(values)
    kind=bytearray(261)
    for name,index in vocab.items():kind[index]=2 if name=='sp' else 1 if re.search('[1-5]$',name) else 0
    blob=bytearray(struct.pack('<8I',0x31504c47,1,len(characters),len(phrases),len(cp_data),len(phone_data),vocab['sil'],vocab['sp']))
    def words(rows):
        for row in rows:blob.extend(struct.pack('<'+'I'*len(row),*row))
    words(characters);words(phrases);words([(cp,) for cp in cp_data]);words([(phone,) for phone in phone_data]);blob.extend(kind)
    target=a.release/'tts-v2/frontend.bin'
    if target.exists() and target.read_bytes()!=blob:raise ValueError('Preserve the existing different frontend')
    target.write_bytes(blob)
    tests=['你好。','如果明天阳光很好，我们就去公园散步。','设备已经连接成功。现在可以正常使用了。','重庆的音乐很好听。','银行今天开门吗？','长大以后，我想去长城。','今天天气不错。','请把空调调到二十六度。','录音已经保存到存储卡。','句子之间的停顿自然。']
    rows=[]
    for text in tests:
        symbols=frontend(text,vocab,lexicon);rows.append(dict(text=text,ids=[vocab[x] for x in symbols],kind=[kind[vocab[x]] for x in symbols]))
    (a.release/'checks/frontend.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2))
    notice=a.release/'notices';notice.mkdir(exist_ok=True)
    distribution=importlib.metadata.distribution('pypinyin')
    for path in distribution.files:
        if 'LICENSE' in str(path).upper():
            (notice/('pypinyin-'+Path(str(path)).name)).write_bytes(distribution.locate_file(path).read_bytes())
    result=dict(bytes=len(blob),characters=len(characters),phrases=len(phrases),sha256=hashlib.sha256(blob).hexdigest(),pypinyin=distribution.version)
    (a.release/'tts-v2/frontend-provenance.json').write_text(json.dumps(result,indent=2))
    print(json.dumps(result),flush=True)


if __name__=='__main__':main()
