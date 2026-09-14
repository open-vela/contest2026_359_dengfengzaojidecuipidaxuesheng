"""Exercise the actual Hosted transport lifecycle with a blocked RX callback."""
from pathlib import Path
import sys
import subprocess
import tempfile

TOOLS = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / 'nuttx/wireless/bluetooth').mkdir(parents=True)
        board = 'CONFIG_SYSTEM_C6BLE_V3_EXPERIMENTAL' if '--v3' in sys.argv else 'CONFIG_ESP32P4_SELECTS_REV_LESS_V3'
        (root / 'nuttx/config.h').write_text('#define ' + board + ' 1\n')
        (root / 'nuttx/wireless/bluetooth/bt_driver.h').write_text(
            '#include <stddef.h>\n#include <stdint.h>\nstruct bt_driver_s;\n')
        (root / 'esp_hosted.h').write_text('''
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define ESP_HOSTED_IF_HCI 4
#define ESP_HOSTED_CAP_BT_SDIO 4
#define ESP_HOSTED_CAP_BLE_ONLY 8
typedef void (*esp_hosted_rx_cb_t)(void *,uint8_t,const uint8_t *,uint16_t,uint8_t);
struct esp_hosted_caps_s {bool valid; uint8_t capability;};
const struct esp_hosted_caps_s *esp_hosted_get_caps(void);
int esp_hosted_poll(void);
int esp_hosted_send(uint8_t,uint8_t,const uint8_t *,uint16_t);
''')
        source = (TOOLS / 'c6/ble_hosted.c').as_posix()
        (root / 'test.c').write_text('#include "' + source + '"\n' + r'''
#include <assert.h>
#include <stdatomic.h>
static struct esp_hosted_caps_s caps;
static pthread_mutex_t dispatch = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static esp_hosted_rx_cb_t callback;
static void *callback_arg;
static bool entered, finish;
static atomic_bool closed;
static int claims, releases;
const struct esp_hosted_caps_s *esp_hosted_get_caps(void) {return &caps;}
int esp_hosted_hci_claim(esp_hosted_rx_cb_t cb, void *arg) {
 pthread_mutex_lock(&dispatch);
 if(callback) {pthread_mutex_unlock(&dispatch);return -EBUSY;}
 callback=cb;callback_arg=arg;claims++;
 pthread_mutex_unlock(&dispatch);return 0;
}
int esp_hosted_hci_release(esp_hosted_rx_cb_t cb, void *arg) {
 pthread_mutex_lock(&dispatch);
 assert(callback==cb && callback_arg==arg);
 callback=NULL;callback_arg=NULL;releases++;
 pthread_mutex_unlock(&dispatch);return 0;
}
int esp_hosted_poll(void) {
 static const uint8_t data[]={14,4,1,3,12,0};
 pthread_mutex_lock(&dispatch);
 if(callback) callback(callback_arg,4,data,sizeof(data),4);
 pthread_mutex_unlock(&dispatch);return 0;
}
int esp_hosted_send(uint8_t type,uint8_t num,const uint8_t *data,uint16_t len) {
 assert(type==4 && num==0 && data && len==4);return 0;
}
static int receive(void *arg,uint8_t type,const uint8_t *data,size_t len) {
 assert(arg && type==4 && data && len==6);
 pthread_mutex_lock(&gate);entered=true;pthread_cond_broadcast(&condition);
 while(!finish) pthread_cond_wait(&condition,&gate);
 pthread_mutex_unlock(&gate);return 0;
}
static void *close_transport(void *arg) {
 struct c6_ble_transport *ops=arg;
 ops->stop(ops->context);closed=true;return NULL;
}
int main(void) {
 struct c6_ble_transport *ops=c6_ble_hosted_create();assert(ops);
 uint8_t reset[]={1,3,12,0};int owner;
 assert(ops->send(ops->context,reset,4)==-ENOTCONN);
 assert(ops->start(ops->context,receive,&owner)==-ENETDOWN);
 caps.valid=true;assert(ops->start(ops->context,receive,&owner)==-ENOTSUP);
 caps.capability=12;
 assert(!ops->start(ops->context,receive,&owner));
 assert(ops->start(ops->context,receive,&owner)==-EBUSY);
 pthread_mutex_lock(&gate);
 while(!entered) pthread_cond_wait(&condition,&gate);
 pthread_mutex_unlock(&gate);
 assert(!ops->send(ops->context,reset,4));
 pthread_t closer;assert(!pthread_create(&closer,NULL,close_transport,ops));
 struct c6_ble_hosted_s *priv=ops->context;
 bool stopped=false;
 for(int i=0;i<1000;i++) {
  pthread_mutex_lock(&priv->lock);stopped=!priv->running;pthread_mutex_unlock(&priv->lock);
  if(stopped) break;
  usleep(1000);
 }
 assert(stopped && !closed);
 assert(ops->send(ops->context,reset,4)==-ENOTCONN);
 pthread_mutex_lock(&gate);finish=true;pthread_cond_broadcast(&condition);pthread_mutex_unlock(&gate);
 assert(!pthread_join(closer,NULL));assert(closed && releases==1);
 ops->stop(ops->context);assert(releases==1);
 assert(!ops->start(ops->context,receive,&owner));
 ops->stop(ops->context);assert(claims==2 && releases==2);
 c6_ble_hosted_destroy(ops);
 return 0;
}
''')
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-I', str(root), '-I', str(TOOLS / 'c6'),
                        str(root / 'test.c'), '-o', str(root / 'test')], check=True)
        subprocess.run([str(root / 'test')], check=True, timeout=15)
    print('PASS: actual Hosted binding capability gates, callback drain, send rejection and reopen')


if __name__ == '__main__':
    main()
