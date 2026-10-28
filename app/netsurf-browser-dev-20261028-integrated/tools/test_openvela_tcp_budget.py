#!/usr/bin/env python3
"""Sanitize the actual opt-in DNS/TCP connector, without replacing its logic."""
import argparse,subprocess
from pathlib import Path
import openvela_tcp_budget
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--target',type=Path,required=True)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output;out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1]
desktop=a.target/'apps/system/desktop'
source=(desktop/'glass_https.c').read_text()
header=(desktop/'glass_https.h').read_text()
if 'glass_https_bounded_tls_ops' not in source:source=openvela_tcp_budget.source(source)
if 'glass_https_bounded_tls_ops' not in header:header=openvela_tcp_budget.header(header)
(out/'glass_https.h').write_text(header)
functions=source[source.index('static int request_state('):source.index('static int tls_close(')]
phase=source[source.index('static int bounded_connect('):source.index('static ssize_t tls_send(')]
prefix=r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <unistd.h>
#include "glass_https.h"
int64_t glass_https_milliseconds(void) {
 struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
 return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
'''
phase_shim=r'''
struct webclient_tls_connection { struct glass_https_request request; };
static struct webclient_tls_connection phase_conn;
static int tls_connect_common(void *ctx,const char *host,const char *port,unsigned timeout,
    struct webclient_tls_connection **out,uint32_t budget) {
 (void)host;(void)port;(void)timeout;(void)budget;
 phase_conn.request=*(struct glass_https_request *)ctx;
 *out=&phase_conn;return 0;
}
'''
suffix=r'''
static int descriptors(void) {
 int n=0;for(int i=0;i<1024;i++) if(fcntl(i,F_GETFD)>=0)n++;return n;
}
static void *cancel_later(void *p) { usleep(30000);atomic_store((atomic_bool *)p,true);return NULL; }
int main(void) {
 int before=descriptors();
 int listener=socket(AF_INET,SOCK_STREAM,0);assert(listener>=0);
 struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
 assert(!bind(listener,(struct sockaddr *)&address,sizeof(address)));
 assert(!listen(listener,0));socklen_t size=sizeof(address);
 assert(!getsockname(listener,(struct sockaddr *)&address,&size));
 int occupied=socket(AF_INET,SOCK_STREAM,0);assert(occupied>=0);
 assert(!connect(occupied,(struct sockaddr *)&address,sizeof(address)));
 char port[16];snprintf(port,sizeof(port),"%u",ntohs(address.sin_port));
 atomic_bool cancelled=false;
 struct glass_https_diagnostic d={0};
 struct glass_https_request request={.abort_flag=&cancelled,.diagnostic=&d};
 int64_t start=glass_https_milliseconds();request.deadline_ms=start+2000;
 int result=tcp_connect(&request,"127.0.0.1",port,120);
 int64_t elapsed=glass_https_milliseconds()-start;
 assert(result==-ETIMEDOUT && elapsed>=110 && elapsed<500);
 assert(request.deadline_ms==start+2000); /* handshake time was not shortened */
 pthread_t worker;assert(!pthread_create(&worker,NULL,cancel_later,&cancelled));
 start=glass_https_milliseconds();
 assert(tcp_connect(&request,"127.0.0.1",port,1000)==-ECANCELED);
 assert(!pthread_join(worker,NULL));assert(glass_https_milliseconds()-start<250);
 atomic_store(&cancelled,false);
 /* A shorter existing deadline still wins; zero preserves legacy semantics. */
 start=glass_https_milliseconds();request.deadline_ms=start+80;
 assert(tcp_connect(&request,"127.0.0.1",port,1000)==-ETIMEDOUT);
 assert(glass_https_milliseconds()-start<300);
 start=glass_https_milliseconds();request.deadline_ms=start+120;
 assert(tcp_connect(&request,"127.0.0.1",port,0)==-ETIMEDOUT);
 assert(glass_https_milliseconds()-start<500);
 int accepted=accept(listener,NULL,NULL);assert(accepted>=0);close(accepted);close(occupied);
 request.deadline_ms=glass_https_milliseconds()+2000;
 int fd=tcp_connect(&request,"127.0.0.1",port,200);assert(fd>=0);close(fd);
 close(listener);assert(descriptors()==before);
 struct glass_https_bounded_request bounded={.request={.deadline_ms=1,.idle_timeout_ms=60000},.connect_timeout_ms=5000};
 struct webclient_tls_connection *connected=NULL;
 int64_t now=glass_https_milliseconds();
 assert(!bounded_connect(&bounded,"localhost","443",15,&connected));
 assert(connected->request.deadline_ms>=now+60000 && bounded.request.deadline_ms==1);
 bounded.request.idle_timeout_ms=0;
 assert(!bounded_connect(&bounded,"localhost","443",15,&connected));
 assert(connected->request.deadline_ms==1); /* no opt-in: preserve fixed deadline */
 puts("PASS: real bounded TCP connector: pending SYN deadline, cancellation, unchanged handshake budget, legacy deadline, success and no descriptor leaks");
 puts("PASS: real bounded TLS wrapper renews first HTTP I/O budget after successful handshake without changing legacy request deadline");
}
'''
(out/'tcp-budget.c').write_text(prefix+functions+phase_shim+phase+suffix)
subprocess.run(['gcc','-std=gnu11','-D_GNU_SOURCE','-g','-fsanitize=address,undefined',
    '-fno-omit-frame-pointer','-include',str(root/'tests/openvela/host/nuttx/config.h'),
    '-I',str(out),'-I',str(root/'tests/openvela/host'),'-I',str(a.target/'apps/include'),str(out/'tcp-budget.c'),
    '-pthread','-o',str(out/'tcp-budget')],check=True)
subprocess.run([str(out/'tcp-budget')],check=True,timeout=10)
