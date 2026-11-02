#!/usr/bin/env python3
"""Exercise actual portal path routing: GETs never create internal directories."""
import argparse,hashlib,json,subprocess,tempfile
from pathlib import Path
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
repo=Path(__file__).resolve().parents[1]
source=repo/'app/espdl-quickapp/overlay/apps/system/desktop/glass_portal_files.c'
owned=Path(tempfile.mkdtemp(prefix='owned-path-fixture-',dir=out))
assert owned.resolve().is_relative_to(out)
text=source.read_text()
production='\n'.join(function(text,name) for name in ['portal_component','portal_relative_path','portal_safe_path','portal_public_path','portal_public_write_path'])
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
static unsigned mkdir_calls;
static bool fail_internal;
static const char *mapped(const char *path)
{
 static char buffer[512];
 if(!strncmp(path,"/sdcard",7)&&(path[7]==0||path[7]=='/')) {
  assert(snprintf(buffer,sizeof(buffer),"%s%s",PORTAL_TEST_SD_ROOT,path+7)<(int)sizeof(buffer));
  return buffer;
 }
 return path;
}
static int fixture_lstat(const char *path,struct stat *st){return lstat(mapped(path),st);}
static int fixture_mkdir(const char *path,mode_t mode)
{
 mkdir_calls++;
 if(fail_internal&&!strncmp(path,PORTAL_DATA_ROOT,sizeof(PORTAL_DATA_ROOT)-1)){errno=ENOSPC;return -1;}
 return mkdir(mapped(path),mode);
}
#define lstat fixture_lstat
#define mkdir fixture_mkdir
'''
post=r'''
#undef lstat
#undef mkdir
int main(void)
{
 assert(!mkdir(PORTAL_DATA_ROOT,0700)&&!mkdir(PORTAL_TEST_SD_ROOT,0700));
 FILE *f=fopen(PORTAL_TEST_SD_ROOT "/clip.wav","wb");assert(f);
 assert(fputs("original fixture",f)>0&&!fclose(f));
 char path[256];struct stat st;
 fail_internal=true;
 assert(!portal_public_path("sdcard/clip.wav",path,sizeof(path))&&!strcmp(path,"/sdcard/clip.wav"));
 assert(!portal_public_path("sdcard",path,sizeof(path))&&!strcmp(path,"/sdcard"));
 assert(!portal_public_path("data",path,sizeof(path)));
 assert(!portal_public_path("files/new.bin",path,sizeof(path)));
 assert(!portal_public_write_path("sdcard/new.bin",path,sizeof(path)));
 assert(mkdir_calls==0&&lstat(PORTAL_DATA_ROOT "/files",&st)<0&&errno==ENOENT);
 assert(portal_public_write_path("files/new.bin",path,sizeof(path))==-ENOSPC);
 assert(mkdir_calls==1&&lstat(PORTAL_DATA_ROOT "/files",&st)<0);
 fail_internal=false;
 assert(!portal_public_write_path("files/new.bin",path,sizeof(path)));
 assert(mkdir_calls==2&&!lstat(PORTAL_DATA_ROOT "/files",&st)&&S_ISDIR(st.st_mode));
 assert(portal_public_path("sdcard/../escape",path,sizeof(path))==-EINVAL);
 assert(portal_public_write_path("files/../escape",path,sizeof(path))==-EINVAL);
 assert(portal_public_path("sdcard/a%2fb",path,sizeof(path))==-EINVAL);
 assert(portal_public_path(NULL,path,sizeof(path))==-EINVAL);
 assert(portal_public_path("sdcard",NULL,sizeof(path))==-EINVAL);
 assert(portal_public_path("sdcard",path,0)==-EINVAL);
 assert(!rmdir(PORTAL_DATA_ROOT "/files"));
 assert(!symlink(PORTAL_TEST_SD_ROOT,PORTAL_DATA_ROOT "/files"));
 assert(portal_public_path("files/clip.wav",path,sizeof(path))==-EINVAL);
 assert(portal_public_write_path("files/new.bin",path,sizeof(path))==-EINVAL);
 assert(mkdir_calls==2);
 puts("PORTAL_READ_PATHS_OK: production routing stays read-only on SD/data/files GETs, explicit first-write directory creation, ENOSPC propagation, no fallback, traversal/link rejection");
}
'''
fixture=out/'portal_read_paths.c';fixture.write_text(pre+'\n'+production+'\n'+post)
binary=out/'portal_read_paths'
subprocess.run(['gcc','-D_GNU_SOURCE','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
 '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie',
 '-DPORTAL_DATA_ROOT="'+str(owned/'data')+'"',
 '-DPORTAL_TEST_SD_ROOT="'+str(owned/'sd')+'"',str(fixture),'-o',str(binary)],check=True)
result=subprocess.run([str(binary)],text=True,capture_output=True,check=True)
(out/'RESULT.json').write_text(json.dumps({'passed':True,'source_sha256':hashlib.sha256(source.read_bytes()).hexdigest(),
 'sanitizers':['address','undefined'],'scope':'Actual production path functions, local filesystem fixture; SD root mapping and mkdir failure/count interception only',
 'result':result.stdout.strip()},indent=2)+'\n')
print(result.stdout,end='')
