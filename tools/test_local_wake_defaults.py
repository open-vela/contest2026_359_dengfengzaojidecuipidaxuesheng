"""Exercise the production startup setting reader with absent and saved files."""
from pathlib import Path
import argparse
import os
import re
import subprocess

p=argparse.ArgumentParser()
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
repo=Path(__file__).resolve().parents[1]
source=repo/'app/espdl-quickapp/overlay/apps/system/desktop/glass_local_voice.c'
text=source.read_text(encoding='utf-8')
match=re.search(r'FILE \*f=fopen\(GLV_CONFIG_PATH,"rb"\);int target=.*?if\(f\)fclose\(f\);',text)
assert match, 'Production startup reader not found'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
code='''#include <assert.h>
#include <stdio.h>
#include <unistd.h>
static const char *config_path;
#define GLV_CONFIG_PATH config_path
static void read_setting(int *local,int *enabled_out){
'''+match[0]+'''
*local=target!='0';*enabled_out=enabled=='1';
}
int main(int argc,char **argv){
 assert(argc==2);config_path=argv[1];unlink(config_path);int local,enabled;
 read_setting(&local,&enabled);assert(local==1&&enabled==1);
 const char *cases[]={"11","01","10","00"};
 for(unsigned i=0;i<4;i++){
  FILE *f=fopen(config_path,"wb");assert(f);assert(fwrite(cases[i],2,1,f)==1);fclose(f);
  read_setting(&local,&enabled);
  assert(local==(cases[i][0]=='1'));assert(enabled==(cases[i][1]=='1'));
 }
 unlink(config_path);puts("WAKE_DEFAULTS_OK: first boot enabled; saved local/cloud target and on/off retained");
}
'''
(out/'test.c').write_text(code,encoding='utf-8')
subprocess.run(['gcc','-std=c99','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(out/'test.c'),'-o',str(out/'test')],check=True)
subprocess.run([str(out/'test'),str(out/'wake-test-setting')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
