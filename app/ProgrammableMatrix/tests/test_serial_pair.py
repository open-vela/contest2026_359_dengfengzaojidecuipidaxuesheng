"""Compile the production line parser with isolated UART and action substitutes."""
from pathlib import Path
import shutil
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'main/button.c').read_text()
parser = source[source.index('static void serial_pair_poll'):source.index('static void button_task')]
harness = r'''
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#define UART_NUM_0 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
static const unsigned char *input;
static unsigned left, pairs, provisions;
static int uart_read_bytes(int port,void *buffer,unsigned length,int ticks){
    (void)port;(void)ticks;
    unsigned n=left<length?left:length;
    memcpy(buffer,input,n);input+=n;left-=n;return (int)n;
}
static void wifi_ble_button_short_press(void){pairs++;}
static void wifi_ble_serial_provision(void){provisions++;}
PARSER
static void feed(const char *data,unsigned length){
    input=(const unsigned char *)data;left=length;
    while(left)serial_pair_poll();
}
int main(void){
    feed("jbox pair\r\n",11);assert(pairs==1);
    feed("help\nunknown\njbox ",18);assert(pairs==1);
    feed("pair\n",5);assert(pairs==2);
    feed("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxjbox pair\n",50);assert(pairs==2);
    feed("jbox\0 pair\n",11);assert(pairs==2);
    feed("jbox pair\n",10);assert(pairs==3);
    feed("jbox pair\xff\n",11);assert(pairs==3);
    feed("jbox pair\n",10);assert(pairs==4);
    feed("jbox provision\r\n",16);assert(provisions==1 && pairs==4);
    feed("jbox provision ignored\n",23);assert(provisions==1);
    return 0;
}
'''.replace('PARSER', parser)
with tempfile.TemporaryDirectory(prefix='jbox-serial-') as directory:
    out = Path(directory)
    (out / 'parser.c').write_text(harness)
    executable = out / 'parser.exe'
    subprocess.run([shutil.which('gcc') or 'gcc', '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(out / 'parser.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('UART pairing parser: CRLF, fragments, bounds, binary rejection and dispatch PASS')
