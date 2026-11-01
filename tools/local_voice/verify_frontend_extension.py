#!/usr/bin/env python3
"""Check retained V2 phones and explicit Mandarin rendering of Latin letters."""
import argparse,hashlib,json,subprocess
from pathlib import Path
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--release',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
 root=Path(__file__).resolve().parents[2];d=root/'app/espdl-quickapp/overlay/apps/system/desktop';out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 sources=[d/('glass_local_'+v+'.c') for v in ('model','sha','tokenizer','llm','tts','frontend')]+[root/'app/espdl-quickapp/tests/local_voice_host.c'];exe=out/'frontend'
 hashes={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}
 subprocess.run(['gcc','-std=gnu11','-O2','-g','-ffp-contract=off','-fno-pie','-no-pie','-fsanitize=address,undefined','-I'+str(d),*[str(s) for s in sources],'-lm','-o',str(exe)],check=True)
 def phones(text):
  r=subprocess.run([str(exe),'frontend',str(a.release.resolve()),text],capture_output=True,text=True,timeout=30,check=True)
  return list(map(int,r.stdout.split(':',1)[1].split()))
 original=json.loads((a.release/'checks/frontend.json').read_text(encoding='utf-8'))
 for row in original:assert phones(row['text'])==row['ids'],row['text']
 letters=['诶','比','西','迪','伊','艾弗','吉','艾尺','艾','杰','开','艾勒','艾姆','恩','欧','批','丘','阿尔','艾丝','提','优','维','达不溜','艾克斯','歪','贼德']
 for i,text in enumerate(letters):
  for letter in (chr(65+i),chr(97+i)):assert phones(letter)==phones(text),(letter,text)
 for raw,text in [('我是AI。','我是人工智能。'),('AI助手','人工智能助手'),('ＡＩ助手','人工智能助手'),('MAID','艾姆诶艾迪'),('AIx','诶艾艾克斯'),('26°C','摄氏二十六度')]:assert phones(raw)==phones(text),(raw,text)
 assert hashes=={str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}
 result=dict(passed=True,scope='ASAN/UBSAN retained ten V2 phone sequences exact; all 26 Latin letters in both cases rendered using original Mandarin phones; standalone AI expands to 人工智能; no English acoustic model claimed',retained_v2_sentences=len(original),latin_letter_cases=52,source_sha256=hashes)
 (out/'VERIFICATION.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8');print('V2_FRONTEND_EXTENSION_OK original10-exact/52-letters/AI-boundaries/NFKC/temperature',flush=True)
if __name__=='__main__':main()
