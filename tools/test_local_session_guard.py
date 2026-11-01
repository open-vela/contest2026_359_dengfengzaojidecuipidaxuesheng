#!/usr/bin/env python3
"""Validate reversible source wraps and concurrent storage generation logic."""
import argparse,hashlib,json,subprocess
from pathlib import Path
from prepare_local_session_guard import FAT,SPI,CONTRACT,FAT_SUPPORT,SPI_SUPPORT,definition,transform,wrapper,restored_body

def main():
    p=argparse.ArgumentParser();p.add_argument('--nuttx',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    out=a.output.resolve();out.mkdir(parents=True,exist_ok=False);sources={}
    for rel in (FAT,SPI):
        current=(a.nuttx/rel).read_text();base=transform(current,rel,False);enabled=transform(base,rel,True)
        assert transform(enabled,rel,True)==enabled
        assert transform(enabled,rel,False)==base
        names=[n for n in CONTRACT if n.startswith('fat_')==(rel==FAT)]
        for n in names:
            old,_=definition(base,n);new,_=definition(enabled,n+'_local_session_body')
            assert old==restored_body(new,n)
        try:transform(base.replace('return ret;','return ret + 1;',1),rel,True)
        except RuntimeError:pass
        else:
            # A changed unrelated helper may be allowed, but changing a
            # contracted entrypoint must always fail closed.
            old,_=definition(base,names[0]);changed=old.replace('{','{\n  /* changed */',1)
            try:transform(base.replace(old,changed),rel,True)
            except RuntimeError:pass
            else:raise AssertionError('Changed contracted body accepted')
        sources[rel]=hashlib.sha256(base.encode()).hexdigest()
    c='''#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/types.h>
#define FAR
#define OK 0
const uint32_t g_fat_local_session_guard_abi=1;
struct file {int f_oflags;};struct inode {int unused;};
#if NUTTX_FLAGS
#define O_RDONLY 1
#define O_WRONLY 2
#define O_RDWR 3
#else
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#endif
#define O_ACCMODE 3
#define O_CREAT 64
#define O_TRUNC 512
static int body_result=123;
'''+SPI_SUPPORT
    for rel,name in ((FAT,'fat_open'),(FAT,'fat_seek'),(SPI,'mmcsd_write')):
        base=transform((a.nuttx/rel).read_text(),rel,False);body,kind=definition(base,name)
        header=body[:body.index('\n{')]
        c+=header.replace(name+'(',name+'_local_session_body(',1)+'\n{return body_result;}\n'+wrapper(body,name,kind)+'\n'
    c+='''
static void *writer(void *unused){(void)unused;for(unsigned i=0;i<10000;i++){
 mmcsd_local_session_change_begin();uint32_t g;assert(mmcsd_local_session_stamp(&g)==-EAGAIN);
 mmcsd_local_session_change_end();}return NULL;}
int main(void){uint32_t g;assert(!mmcsd_local_session_stamp(&g)&&g==0);
 mmcsd_local_session_change_begin();mmcsd_local_session_change_begin();
 assert(mmcsd_local_session_stamp(&g)==-EAGAIN);mmcsd_local_session_change_end();
 assert(mmcsd_local_session_stamp(&g)==-EAGAIN);mmcsd_local_session_change_end();
 assert(!mmcsd_local_session_stamp(&g)&&g==4);
 pthread_t workers[3];for(unsigned i=0;i<3;i++)assert(!pthread_create(workers+i,NULL,writer,NULL));
 for(unsigned i=0;i<3;i++)assert(!pthread_join(workers[i],NULL));
 assert(!mmcsd_local_session_stamp(&g)&&g==60004);
 struct file f={O_RDONLY};struct inode inode={0};
 assert(fat_open(&f,"model",O_RDONLY,0)==123);assert(fat_seek(&f,0,0)==123);
 assert(!mmcsd_local_session_stamp(&g)&&g==60004);
 assert(fat_open(&f,"model",O_WRONLY,0)==123);assert(!mmcsd_local_session_stamp(&g)&&g==60006);
 f.f_oflags=O_RDWR;assert(fat_seek(&f,0,0)==123);assert(!mmcsd_local_session_stamp(&g)&&g==60008);
 body_result=-EIO;assert(mmcsd_write(&inode,NULL,0,1)==-EIO);
 assert(!mmcsd_local_session_stamp(&g)&&g==60010);
 assert(fat_open(&f,"model",O_RDONLY|O_TRUNC,0)==-EIO);assert(!mmcsd_local_session_stamp(&g)&&g==60012);
 g_local_session_generation=UINT32_MAX;mmcsd_local_session_change_begin();mmcsd_local_session_change_end();
 assert(mmcsd_local_session_stamp(&g)==-EOVERFLOW);puts("STORAGE_GENERATION_CONCURRENCY_OK");return 0;}
'''
    (out/'guard.c').write_text(c);exe=out/'guard'
    for style in (0,1):
        subprocess.run(['gcc','-std=gnu11','-O2','-g','-DNUTTX_FLAGS='+str(style),'-fsanitize=address,undefined','-fno-pie','-no-pie',str(out/'guard.c'),'-pthread','-o',str(exe)],check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True,check=True);(out/('run-'+str(style)+'.log')).write_text(result.stdout+result.stderr)
    # Check both POSIX O_RDONLY=0 and NuttX O_RDONLY=1 interpretation.
    fat=(a.nuttx/FAT).read_text();base=transform(fat,FAT,False)
    body,kind=definition(base,'fat_open');text=wrapper(body,'fat_open',kind)
    assert '!= O_RDONLY' in text
    report=dict(passed=True,source_base_sha256=sources,original_functions_preserved=True,mutation_entrypoints=list(CONTRACT),
                generation_concurrency_passed=True,wraparound_fails_closed=True,
                tool_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n');print(result.stdout,flush=True)
if __name__=='__main__':main()
