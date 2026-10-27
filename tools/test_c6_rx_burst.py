#!/usr/bin/env python3
"""Check actual multi-block CMD53 setup and bounded TCP/CRC diagnostic."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import threading
import zlib
from render_all_ui import function

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--nuttx', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
repo = Path(__file__).resolve().parents[1]
folder = repo / 'app/espdl-quickapp/overlay/apps/system/c6probe'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
text = (folder / 'esp_hosted.c').read_text()
prefix = r'''
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define FAR
#define OK 0
#define C6_RX_DMA_BLOCKS 4u
#define C6_SDIO_BLOCK_SIZE 512
#define C6_SDIO_FUNCTION 1
#define SDIOWAIT_TRANSFERDONE 1
#define SDIOWAIT_TIMEOUT 2
#define SDIOWAIT_ERROR 4
#define SDIO_CMD53RD 53
typedef unsigned sdio_eventset_t;
struct sdio_dev_s {int unused;};
static struct {int lock;} g_hosted;
static unsigned g_rx_block_limit=4,block_size,block_count,dma_bytes,arg_seen,cancels,sends;
static int setup_ret,send_ret,response_ret,lock_ret,test_event=1;
static unsigned char g_dma_block[2048] __attribute__((aligned(64)));
static int nxmutex_lock(int *p){(void)p;return lock_ret;}
static void nxmutex_unlock(int *p){(void)p;}
#define SDIO_BLOCKSETUP(d,s,n) ((void)(d),block_size=(s),block_count=(n))
#define SDIO_WAITENABLE(d,e,t) ((void)(d),(void)(e),assert((t)==1000))
#define SDIO_DMARECVSETUP(d,b,n) ((void)(d),assert((void *)(b)==(void *)g_dma_block),dma_bytes=(n),setup_ret)
#define SDIO_SENDCMD(d,c,a) ((void)(d),(void)(c),arg_seen=(a),sends++,send_ret)
#define SDIO_EVENTWAIT(d) ((void)(d),test_event)
#define SDIO_RECVR5(d,c,p) ((void)(d),(void)(c),*(p)=0,response_ret)
#define SDIO_CANCEL(d) ((void)(d),cancels++)
'''
suffix = r'''
int main(void){
 struct sdio_dev_s d;
 for(unsigned n=1;n<=4;n++){
  assert(!hosted_do_read_block(&d,0x1e123,n));
  assert(block_size==512&&block_count==n&&dma_bytes==n*512);
  assert((arg_seen&511)==n&&((arg_seen>>9)&0x1ffff)==0x1e123);
  assert((arg_seen&(1u<<27))&&(arg_seen&(1u<<26))&&!(arg_seen&(1u<<31)));
 }
 unsigned before=sends;assert(hosted_do_read_block(&d,0,0)==-EINVAL);
 assert(hosted_do_read_block(&d,0,5)==-EINVAL&&sends==before);
 setup_ret=-ENOMEM;cancels=0;assert(hosted_do_read_block(&d,0,4)==-ENOMEM&&cancels==1&&sends==before);
 setup_ret=0;send_ret=-EIO;cancels=0;assert(hosted_do_read_block(&d,0,4)==-EIO&&cancels==1);
 send_ret=0;test_event=SDIOWAIT_TIMEOUT;assert(hosted_do_read_block(&d,0,4)==-ETIMEDOUT);
 test_event=SDIOWAIT_ERROR;assert(hosted_do_read_block(&d,0,4)==-EIO);
 test_event=1;response_ret=-EILSEQ;assert(hosted_do_read_block(&d,0,4)==-EILSEQ);
 assert(esp_hosted_rx_block_limit(2)==-EINVAL&&g_rx_block_limit==4);
 lock_ret=-EAGAIN;assert(esp_hosted_rx_block_limit(1)==-EAGAIN&&g_rx_block_limit==4);
 lock_ret=0;assert(esp_hosted_rx_block_limit(1)==1&&g_rx_block_limit==1);
 assert(esp_hosted_rx_block_limit(4)==4&&g_rx_block_limit==4);
}
'''
c = out / 'cmd53.c'
c.write_text(prefix + function(text, 'esp_hosted_rx_block_limit') +
             function(text, 'hosted_do_read_block') + suffix)
flags = ['gcc', '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
subprocess.run([*flags, str(c), '-o', str(out / 'cmd53')], check=True)
subprocess.run([str(out / 'cmd53')], check=True)
(out / 'nuttx').mkdir(exist_ok=True)
(out / 'nuttx/crc32.h').write_text('#define FAR\n#include <stdint.h>\n#include <stddef.h>\nuint32_t crc32part(const uint8_t *,size_t,uint32_t);\n')
c = out / 'bench.c'
c.write_text('''#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#define FAR
''' + '#include "' + str(folder / 'c6_rx_bench.inc') + '"\n' +
             'int main(int argc,char **argv){return c6_rx_bench_command(argc,argv);}\n')
exe = out / 'bench'
subprocess.run([*flags, '-I'+str(out), str(c),
                str(a.nuttx / 'libs/libc/misc/lib_crc32.c'), '-o', str(exe)], check=True)
payload = bytes((n * 37 ^ n >> 8 ^ n >> 16) & 255 for n in range(100003))
results = {}
for label, actual in [('good', payload), ('short', payload[:-1]),
                       ('extra', payload+b'x'), ('corrupt', b'x'+payload[1:])]:
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    port = listener.getsockname()[1]
    def serve():
        with listener:
            conn, _ = listener.accept()
            with conn:
                for at in range(0, len(actual), 511):
                    conn.sendall(actual[at:at+511])
    t = threading.Thread(target=serve, daemon=True)
    t.start()
    run = subprocess.run([str(exe), 'rxbench', '127.0.0.1', str(port),
                          str(len(payload)), f'{zlib.crc32(payload):08x}'],
                         capture_output=True, text=True, timeout=10)
    t.join(2)
    (out / (label+'.log')).write_text(run.stdout+run.stderr)
    assert run.returncode == (0 if label == 'good' else 1), run.stderr
    results[label] = run.stdout.strip()
for args in [[], ['rxbench','bad','1','10','12345678'],
             ['rxbench','127.0.0.1','0','10','12345678'],
             ['rxbench','127.0.0.1','1','16777217','12345678'],
             ['rxbench','127.0.0.1','1','10','-0000001']]:
    run = subprocess.run([str(exe), *args], capture_output=True, timeout=5)
    assert run.returncode == 1
(out / 'RESULT.json').write_text(json.dumps({'passed':True,'cases':results},indent=2))
print('C6_RX_BURST_PASS: 1..4 blocks; command bits/lengths; DMA/send/R5/event errors; bounded mode control; exact TCP length and CRC; malformed input')
