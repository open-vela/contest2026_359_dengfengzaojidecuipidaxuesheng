#!/usr/bin/env python3
"""Export a new small policy release; original model weights remain unchanged."""
import argparse,hashlib,json,struct
from pathlib import Path

def main():
 p=argparse.ArgumentParser();p.add_argument('--model',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();root=a.output.resolve();root.mkdir(exist_ok=False)
 metadata=json.loads((a.model/'model.json').read_text());weights=(a.model/'weights.bin').read_bytes();digest=hashlib.sha256(weights).digest()
 kept=sorted(v['name'] for v in metadata['tensors'] if len(v['shape'])==3 and (v['name'] in ('vocoder.input_conv.weight','vocoder.output_conv.1.weight') or v['name'].startswith('vocoder.blocks.2.')))
 assert len(kept)==8 and all(len(x.encode())<80 for x in kept)
 body=struct.pack('<8sII32sIII',b'GLV8P001',1,1,digest,1,len(kept),0)+b''.join(struct.pack('<80s',x.encode()) for x in kept)
 (root/'policy.bin').write_bytes(body);row=dict(path='policy.bin',bytes=len(body),sha256=hashlib.sha256(body).hexdigest())
 manifest=dict(format='LOCALVOICE1',runtime_abi=1,policy_format=1,release=root.name,source='Original Baker V2 Q8 weights, host disjoint activation calibration and sensitivity experiment',base_weights_sha256=digest.hex(),runtime_compatibility='GLV_TTS_INT8 affine/mixed policy v1; not supported by candidate49',kept_precision_bits=16,other_convolution_precision_bits=8,activation_zero_point=True,kept_layers=kept,quality_accepted=False,files=[row])
 (root/'manifest.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2)+chr(10));(root/'manifest.bin').write_bytes(struct.pack('<4sIII',b'GLV1',1,1,0)+struct.pack('<96sI32s',b'policy.bin',len(body),bytes.fromhex(row['sha256'])))
 assert hashlib.sha256((a.model/'weights.bin').read_bytes()).digest()==digest
 print(json.dumps(dict(release=str(root),bytes=len(body),kept=8,int8_convolutions=66,sha256=row['sha256'])),flush=True)
if __name__=='__main__':main()
