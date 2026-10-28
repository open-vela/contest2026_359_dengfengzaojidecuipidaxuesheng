#!/usr/bin/env python3
"""Compare actual album scan bodies on identical files; host I/O counts, not SD latency."""
import argparse,json,struct,subprocess,tempfile
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--baseline',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
fixture=r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "local_album.c"
static unsigned stats,opens;static size_t bytes;
int __real_lstat(const char *,struct stat *);
FILE *__real_fopen(const char *,const char *);
size_t __real_fread(void *,size_t,size_t,FILE *);
int __wrap_lstat(const char *path,struct stat *st){stats++;return __real_lstat(path,st);}
FILE *__wrap_fopen(const char *path,const char *mode){opens++;return __real_fopen(path,mode);}
size_t __wrap_fread(void *p,size_t size,size_t count,FILE *file)
{size_t n=__real_fread(p,size,count,file);bytes+=n*size;return n;}
static double now(void){struct timespec ts;assert(!clock_gettime(CLOCK_MONOTONIC,&ts));return ts.tv_sec+ts.tv_nsec/1e9;}
int main(void)
{
  double elapsed=0;unsigned iterations=12;
  for(unsigned i=0;i<iterations;i++){
    struct album_job *job=calloc(1,sizeof(*job));assert(job);
    job->listing=calloc(1,sizeof(*job->listing));assert(job->listing);
    strcpy(job->path,ALBUM_ROOT);job->kind=ALBUM_PHOTO;
    stats=opens=0;bytes=0;double start=now();
    struct album_listing *list=album_read_directory(job);elapsed+=now()-start;
    assert(list&&!list->error&&!list->truncated&&list->count==128);
    free(list);free(job);
  }
  printf("{\"listing_bytes\":%zu,\"entry_bytes\":%zu,\"lstat_per_scan\":%u,\"opens_per_scan\":%u,\"bytes_read_per_scan\":%zu,\"iterations\":%u,\"mean_scan_ms\":%.6f}\n",
    sizeof(struct album_listing),sizeof(struct album_entry),stats,opens,bytes,iterations,elapsed*1000/iterations);
}
'''
records={}
with tempfile.TemporaryDirectory(prefix='album-perf-') as tmp:
    root=Path(tmp);sd=root/'sdcard';sd.mkdir()
    for folder in range(16):
        directory=sd/f'd{folder:02}';directory.mkdir()
        for i in range(8):(directory/f'p{i:02}.jpg').write_bytes(b'\xff\xd8\xff')
        for i in range(4):(directory/f'v{i:02}.mp4').write_bytes(struct.pack('>I4s4sI4s',20,b'ftyp',b'isom',0,b'isom'))
        for i in range(16):(directory/f'n{i:02}.txt').write_bytes(b'plain text')
    for label,source in [('before',a.baseline.resolve()),('after',repo)]:
        work=out/label;work.mkdir(exist_ok=True);test=work/'benchmark.c';test.write_text(fixture)
        desktop=source/'app/espdl-quickapp/overlay/apps/system/desktop';album=source/'app/bilibili_live'
        command=['cc','-D_GNU_SOURCE','-DALBUM_HOST_TEST','-DALBUM_ROOT="'+str(sd)+'"','-std=gnu11','-O2','-pthread',
            '-I'+str(album),'-I'+str(desktop),str(test),str(album/'album_formats.c'),str(album/'album_png.c'),
            str(desktop/'qpk_mjpeg.c'),str(desktop/'qpk_tjpgd.c'),'-Wl,--wrap=lstat,--wrap=fopen,--wrap=fread','-o',str(work/'benchmark')]
        subprocess.run(command,check=True)
        records[label]=json.loads(subprocess.check_output([str(work/'benchmark')],text=True))
assert records['before']['opens_per_scan']==records['after']['opens_per_scan']==448
assert records['before']['bytes_read_per_scan']==records['after']['bytes_read_per_scan']
assert records['after']['lstat_per_scan']==464 and records['before']['lstat_per_scan']==912
assert records['after']['listing_bytes']<records['before']['listing_bytes']*.4
records['listing_reduction_percent']=100*(1-records['after']['listing_bytes']/records['before']['listing_bytes'])
records['metadata_query_reduction_percent']=100*(1-records['after']['lstat_per_scan']/records['before']['lstat_per_scan'])
records['scope']='Host backend scan timing; 128 photos, 64 videos, 256 text files across 16 directories. Physical SD and UI frame latency unmeasured.'
(out/'PERFORMANCE.json').write_text(json.dumps(records,indent=2)+'\n')
print('ALBUM_PERFORMANCE_OK',json.dumps(records))
