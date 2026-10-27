#!/usr/bin/env python3
"""Exercise real Hosted HCI functions and SDK copying with eight synthetic peers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
from prepare_c6_hci_frames import patch_source, unpatch_source
from render_all_ui import function

PREFIX=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_HCI_IF 4
#define PRIO_Q_BT 0
#define SOC_ESP_NIMBLE_CONTROLLER 1
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_HEXLOGV(...) ((void)0)
#define HCI_DRIVER_TYPE_CMD 1
#define HCI_DRIVER_TYPE_ACL 2
#define HCI_DRIVER_DIR_H2C 1
#define ESP_HCI_INTERNAL_ACL_MBUF_LEADINGSPCAE 4
#define ESP_HCI_INTERNAL_BUF_CMD 3
typedef int hci_driver_data_type_t;
typedef struct {max_align_t alignment;size_t bytes;} allocation;
static _Atomic unsigned live,allocated,released,forwarded;
static _Thread_local int fail_alloc,reject_queue;
static void *tracked_malloc(size_t n){if(fail_alloc){fail_alloc=0;return NULL;}allocation *a=malloc(sizeof(*a)+n);assert(a);a->bytes=n;atomic_fetch_add(&live,1);atomic_fetch_add(&allocated,1);return a+1;}
static void tracked_free(void *p){if(!p)return;allocation *a=(allocation *)p-1;assert(a->bytes);a->bytes=0;atomic_fetch_sub(&live,1);atomic_fetch_add(&released,1);free(a);}
typedef struct {uint8_t if_type,if_num;uint16_t payload_len;uint8_t *payload;void *wlan_buf_handle;void (*free_buf_handle)(void *);} interface_buffer_handle_t;
static interface_buffer_handle_t queue[20];
static unsigned head,queued,peak,received,seen[8],sent[8];
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER,space=PTHREAD_COND_INITIALIZER;
static int send_to_host_queue(interface_buffer_handle_t *b,unsigned priority){assert(priority==PRIO_Q_BT);if(reject_queue)return ESP_FAIL;
 pthread_mutex_lock(&lock);while(queued==20)pthread_cond_wait(&space,&lock);
 queue[(head+queued)%20]=*b;queued++;if(queued>peak)peak=queued;
 pthread_cond_signal(&ready);pthread_mutex_unlock(&lock);return 0;}
struct os_mbuf {size_t capacity,length;uint8_t data[];};
static struct os_mbuf *os_msys_get_pkthdr(uint32_t n,int leading){assert(leading==4);struct os_mbuf *m=tracked_malloc(sizeof(*m)+n);assert(m);m->capacity=n;m->length=0;return m;}
static int os_mbuf_append(struct os_mbuf *m,const void *data,uint32_t n){assert(n<=m->capacity);memcpy(m->data,data,n);m->length=n;return 0;}
static void *r_ble_hci_trans_buf_alloc(int type){assert(type==3);return tracked_malloc(260);}
static void check_acl(const uint8_t *p,size_t n,unsigned *sequence){assert(n==20&&p[0]>=1&&p[0]<=8&&p[1]==0x20&&p[2]==16&&p[3]==0);
 unsigned peer=p[0]-1,value=(unsigned)p[4]|((unsigned)p[5]<<8)|((unsigned)p[6]<<16)|((unsigned)p[7]<<24);
 assert(value==sequence[peer]++);for(unsigned i=8;i<20;i++)assert(p[i]==(uint8_t)(peer+value+i));}
static int forward_packet(int type,uint8_t *data,uint32_t length,int direction){assert(direction==1);
 if(type==2){struct os_mbuf *m=(void *)data;assert(m->length==length-1);check_acl(m->data,m->length,sent);}
 else{assert(type==1&&length>=4);assert((unsigned)data[2]+4==length);}
 atomic_fetch_add(&forwarded,1);tracked_free(data);return 0;}
static struct {int (*forward_cb)(int,uint8_t *,uint32_t,int);} s_hci_driver_vhci_env={forward_packet};
static int hci_driver_vhci_host_tx(hci_driver_data_type_t,uint8_t *,uint32_t);
static int esp_vhci_host_check_send_available(void){return 1;}
static void esp_vhci_host_send_packet(uint8_t *data,uint16_t len){assert(!hci_driver_vhci_host_tx(data[0],data,len));}
#define malloc tracked_malloc
#define free tracked_free
'''

SUFFIX=r'''
#undef malloc
#undef free
static void packet(uint8_t *p,unsigned peer,unsigned seq){p[0]=2;p[1]=peer+1;p[2]=0x20;p[3]=16;p[4]=0;
 for(unsigned i=0;i<4;i++)p[5+i]=(uint8_t)(seq>>(i*8));for(unsigned i=8;i<20;i++)p[i+1]=(uint8_t)(peer+seq+i);}
static void *consume(void *unused){(void)unused;for(unsigned n=0;n<8000;n++){
 pthread_mutex_lock(&lock);while(!queued)pthread_cond_wait(&ready,&lock);
 interface_buffer_handle_t b=queue[head];head=(head+1)%20;queued--;pthread_cond_signal(&space);pthread_mutex_unlock(&lock);
 assert(b.if_type==ESP_HCI_IF&&b.if_num==0&&b.payload_len==21&&b.payload==b.wlan_buf_handle);
 assert(b.payload[0]==2);check_acl(b.payload+1,b.payload_len-1,seen);b.free_buf_handle(b.wlan_buf_handle);received++;}return NULL;}
static void *produce(void *arg){unsigned peer=(unsigned)(uintptr_t)arg;for(unsigned seq=0;seq<1000;seq++){
 uint8_t p[21];packet(p,peer,seq);assert(!host_rcv_pkt(p,sizeof(p)));memset(p,0,sizeof(p));}return NULL;}
int main(int argc,char **argv){assert(argc==2);
 if(!strcmp(argv[1],"oversize")){uint8_t p[1024]={1,0,0,0};process_hci_rx_pkt(p+1,1023);
#ifdef CANDIDATE
  assert(!atomic_load(&forwarded)&&!atomic_load(&live));puts("HCI_OVERSIZE_REJECTED");return 0;
#else
  assert(!"Old path must fail under ASan");
#endif
 }
 uint8_t fault[21];packet(fault,0,0);fail_alloc=1;assert(host_rcv_pkt(fault,sizeof(fault))==ESP_FAIL&&!atomic_load(&live));
 reject_queue=1;assert(host_rcv_pkt(fault,sizeof(fault))==ESP_FAIL&&!atomic_load(&live));reject_queue=0;
 pthread_t consumer,producers[8];assert(!pthread_create(&consumer,NULL,consume,NULL));
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&producers[i],NULL,produce,(void *)(uintptr_t)i));
 for(unsigned i=0;i<8;i++)pthread_join(producers[i],NULL);pthread_join(consumer,NULL);
 assert(received==8000&&!queued&&peak<=20&&!atomic_load(&live));
 for(unsigned seq=0;seq<1000;seq++)for(unsigned peer=0;peer<8;peer++){uint8_t p[21];packet(p,peer,seq);process_hci_rx_pkt(p+1,20);}
 assert(atomic_load(&forwarded)==8000&&!atomic_load(&live));
 for(unsigned i=0;i<8;i++)assert(seen[i]==1000&&sent[i]==1000);
 uint8_t minimum[4]={1,3,12,0};process_hci_rx_pkt(minimum+1,3);
 uint8_t maximum[259]={1,1,2,255};memset(maximum+4,0x6a,255);process_hci_rx_pkt(maximum+1,258);
#ifdef CANDIDATE
 unsigned before=atomic_load(&forwarded);uint8_t malformed[65]={0};
 for(unsigned type=1;type<=5;type++)for(unsigned n=0;n<=64;n++){
   malformed[0]=type;malformed[3]=255;malformed[4]=255;process_hci_rx_pkt(malformed+1,n);}
 process_hci_rx_pkt(NULL,4);process_hci_rx_pkt(minimum+1,UINT16_MAX);
 assert(atomic_load(&forwarded)==before);
#endif
 assert(!atomic_load(&live)&&atomic_load(&allocated)==atomic_load(&released));
 printf("HCI_EIGHT_STRESS_OK c2h=%u h2c=8000 command_boundaries=2 peak_queue=%u allocated=%u freed=%u\n",received,peak,atomic_load(&allocated),atomic_load(&released));return 0;}
'''


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,required=True)
    p.add_argument('--sdk',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    supplied=a.source.read_text();original=unpatch_source(supplied);candidate=patch_source(original)
    assert supplied in (original,candidate)
    assert patch_source(candidate)==candidate
    (out/'candidate_slave_bt.c').write_text(candidate)
    sdk_path=a.sdk/'components/bt/porting/transport/driver/vhci/hci_driver_standard.c'
    sdk=function(sdk_path.read_text(),'hci_driver_vhci_host_tx')
    if sdk.startswith('hci_driver_vhci_host_tx'):sdk='static int\n'+sdk
    results=[]
    flags=['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
           '-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie','-pthread']
    env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1','UBSAN_OPTIONS':'halt_on_error=1'}
    for name,source in [('baseline',original),('candidate',candidate)]:
        parts=[function(source,'host_rcv_pkt')]
        if name=='candidate':parts.append(function(source,'openvela_hci_payload_valid'))
        parts.extend([function(source,'process_hci_rx_pkt'),sdk])
        test=out/(name+'.c');test.write_text(PREFIX+'\n'.join(parts)+SUFFIX)
        binary=out/name
        cmd=[*flags,*(['-DCANDIDATE'] if name=='candidate' else []),str(test),'-o',str(binary)]
        compiled=subprocess.run(cmd,capture_output=True,text=True)
        (out/(name+'-compile.log')).write_text(compiled.stdout+compiled.stderr);compiled.check_returncode()
        for mode in ['stress','oversize']:
            run=subprocess.run([str(binary),mode],env=env,capture_output=True,text=True)
            (out/(name+'-'+mode+'.log')).write_text(run.stdout+run.stderr)
            negative=name=='baseline' and mode=='oversize'
            if negative:assert run.returncode!=0 and 'heap-buffer-overflow' in run.stderr
            else:run.check_returncode()
            results.append({'variant':name,'mode':mode,'expected_failure':negative,'passed':True})
    report={'passed':True,'scope':'actual_Hosted_and_SDK_functions_with_queue_API_stubs_not_radio',
            'synthetic_peers':8,'candidate_c2h_frames':8000,'candidate_h2c_acl_frames':8000,
            'malformed_short_type_cases':325,'source_before_sha256':hashlib.sha256(original.encode()).hexdigest(),
            'source_sha256':hashlib.sha256(candidate.encode()).hexdigest(),
            'sdk_source_sha256':hashlib.sha256(sdk_path.read_bytes()).hexdigest(),
            'results':results,'device_actions':False,
            'limits':['valid Hosted outer header/headroom assumed','queue rejection modeled before ownership transfer',
                      'SDK internal allocation-failure assertions unchanged']}
    (out/'VERIFICATION.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':main()
