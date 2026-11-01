#!/usr/bin/env python3
"""Verify actual RV32 coefficient checks avoid software double arithmetic."""
import argparse,hashlib,json,re,subprocess,struct
from pathlib import Path
def main():
 p=argparse.ArgumentParser();p.add_argument('--candidate',type=Path,required=True);p.add_argument('--toolchain',type=Path,required=True);p.add_argument('--tts-kernels',action='store_true');a=p.parse_args();root=a.candidate.resolve();elf=root/'firmware/nuttx.elf'
 text=subprocess.check_output([str(a.toolchain/'riscv32-esp-elf-objdump'),'-d',str(elf)],text=True)
 bodies={}
 for chunk in re.split(r'(?=^[0-9a-f]+ <[^>]+>:\n)',text,flags=re.M):
  m=re.match(r'[0-9a-f]+ <([^>]+)>:\n',chunk)
  if m and (m[1].startswith('glv_tensor_row') or m[1] in ('glv_matvec','glv_matbatch') or a.tts_kernels and m[1].split('.')[0] in ('conv_impl','conv_add','vocoder')):bodies[m[1]]=chunk
 assert len(bodies)>=2
 for name,body in bodies.items():assert not re.search(r'__muldf3|__truncdfsf2',body),(name,'software double operation in coefficient loop')
 raw=elf.read_bytes();phoff=struct.unpack_from('<I',raw,28)[0];phsize,phnum=struct.unpack_from('<HH',raw,42)
 def u32_at(address):
  for i in range(phnum):
   kind,offset,va,pa,size,mem,flags,align=struct.unpack_from('<8I',raw,phoff+i*phsize)
   if kind==1 and va<=address and address+4<=va+size:return struct.unpack_from('<I',raw,offset+address-va)[0]
  return None
 native_class=any('fclass.s' in body for body in bodies.values())
 body=bodies['glv_matvec'];constants=[u32_at(int(a,16)) for a in re.findall(r'# ([0-9a-f]+) <',body)]
 native_range='fabs.s' in body and 'fle.s' in body and 0x7f7fffff in constants
 assert native_class or native_range,'Expected native IEEE float classification or fabs <= FLT_MAX'
 for name in ('glv_matvec','glv_matbatch'):
  body=bodies[name];assert 'fmul.s' in body and 'fadd.s' in body and 'fmadd.s' not in body,'Preserve separate FP32 accumulation: '+name
 tts={}
 if a.tts_kernels:
  names=[name for name in bodies if name.split('.')[0] in ('conv_impl','conv_add','vocoder')]
  assert {'conv_impl','vocoder'}<={name.split('.')[0] for name in names}
  for name in names:
   body=bodies[name];assert 'fmul.s' in body and 'fadd.s' in body
   assert not re.search(r'\b(?:fmadd|fmsub|fnmadd|fnmsub)\.s\b|__(?:add|sub|mul|div)df3',body),name
   frame=re.search(r'\baddi\s+sp,sp,-(\d+)',body);assert frame,name
   size=int(frame[1]);assert size<=2048,(name,size)
   tts[name]=dict(stack_frame_bytes=size,separate_fp32_accumulation=True)
 (root/'HOTLOOP-DISASSEMBLY.txt').write_text('\n'.join(bodies.values()))
 meta=json.loads((root/'firmware/BUILD-METADATA.json').read_text())
 report=dict(passed=True,elf_sha256=hashlib.sha256(raw).hexdigest(),image_sha256=meta['sha256']['nuttx.bin'],functions=sorted(bodies),native_float_classification=True,
  classification='fclass.s' if native_class else 'fabs.s / fle.s against verified FLT_MAX',software_double_in_coefficient_checks=False,separate_fp32_accumulation=True,tts_kernels=tts)
 (root/'HOTLOOP-AUDIT.json').write_text(json.dumps(report,indent=2)+'\n');print('LOCAL_VOICE_NATIVE_FLOAT_CHECK_AUDIT_OK')
if __name__=='__main__':main()
