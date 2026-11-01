#!/usr/bin/env python3
"""Create a new, checksummed SD-only ASR board resource version."""
import argparse,hashlib,json,shutil,struct
from pathlib import Path


def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--artifacts',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    r=a.artifacts.resolve();out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
    original=json.loads((r/'asr-c-transcription1/VERIFICATION.json').read_text(encoding='utf-8'));assert original['passed']
    for name,digest in original['assets'].items():
        source=r/'asr-c-graph7'/name;assert sha(source)==digest;shutil.copyfile(source,out/name)
    (out/'checks').mkdir();(out/'notices').mkdir()
    for name in ('0.wav','1.wav'):
        source=r/'asr-evaluation/model'/name
        case=next(v for v in original['cases'] if v['file']==name);assert sha(source)==case['source_sha256'];shutil.copyfile(source,out/'checks'/name)
    shutil.copyfile(r/'asr-mixed-rows-evaluation/fp32/README.md',out/'notices/model-README.md')
    shutil.copyfile(r/'asr-fbank-reference/python/kaldi_native_fbank-1.22.5.dist-info/licenses/LICENSE',out/'notices/Apache-2.0.txt')
    files=[dict(path=f.relative_to(out).as_posix(),bytes=f.stat().st_size,sha256=sha(f)) for f in sorted(out.rglob('*')) if f.is_file()]
    blob=bytearray(struct.pack('<4s3I',b'GLV1',1,len(files),0))
    for entry in files:
        name=entry['path'].encode();assert len(name)<96;blob.extend(name+b'\0'*(96-len(name))+struct.pack('<I',entry['bytes'])+bytes.fromhex(entry['sha256']))
    (out/'manifest.bin').write_bytes(blob)
    manifest=dict(version=out.name,format='GLV1',runtime_abi=1,asr_plan_abi='GLA1-1',tokens_abi='GLT1-1',source_archive_sha256='2cbd71b640d9c37d3784f29367333a4577b0398b62e9deeed418170b081cba8b',source='official Chinese Zipformer 14M 2023-02-23',quantization='group32 symmetric output-row Q4 feed-forward / Q8 sensitive matrices / raw remaining constants',files=files,board_tested=False,asr_ready=False,source_full_c_reference_sha256=sha(r/'asr-c-transcription1/VERIFICATION.json'))
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
    print('ASR_DEVICE_RESOURCE_PREPARED',len(files),sum(x['bytes'] for x in files),flush=True)


if __name__=='__main__':main()
