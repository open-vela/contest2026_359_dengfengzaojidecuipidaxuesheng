#!/usr/bin/env python3
"""Reject poll error events even when SO_ERROR remains zero."""
from pathlib import Path
import argparse,subprocess,json,hashlib
from render_all_ui import function
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--previous',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
source=Path(__file__).resolve().parents[1]/'app/espdl-quickapp/overlay/apps/system/desktop/glass_ethernet.c'
base=r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
struct glass_ethernet_s {bool leased;in_addr_t address;};
static bool leased=true;static int events=POLLOUT,poll_result=1,so_error,closed;
static void glass_wired_snapshot(struct glass_ethernet_s *s){s->leased=leased;s->address=1;}
static int test_socket(int d,int t,int p){assert(d==AF_INET&&t==SOCK_STREAM&&p==0);return 17;}
static int test_fcntl(int fd,int cmd,...){(void)cmd;assert(fd==17);return 0;}
static int test_setsockopt(int fd,int l,int n,const void *v,socklen_t z){(void)l;(void)n;(void)v;(void)z;assert(fd==17);return 0;}
static int test_bind(int fd,const struct sockaddr *s,socklen_t z){(void)s;(void)z;assert(fd==17);return 0;}
static int test_connect(int fd,const struct sockaddr *s,socklen_t z){(void)s;(void)z;assert(fd==17);errno=EINPROGRESS;return -1;}
static int test_poll(struct pollfd *p,nfds_t n,int t){assert(p->fd==17&&n==1&&t==5000);p->revents=events;errno=EBUSY;return poll_result;}
static int test_getsockopt(int fd,int l,int n,void *v,socklen_t *z){assert(fd==17&&l==SOL_SOCKET&&n==SO_ERROR&&*z==sizeof(int));*(int*)v=so_error;return 0;}
static int test_close(int fd){assert(fd==17);closed++;return 0;}
#define socket test_socket
#define fcntl test_fcntl
#define setsockopt test_setsockopt
#define bind test_bind
#define connect test_connect
#define poll test_poll
#define getsockopt test_getsockopt
#define close test_close
'''
main=r'''
int main(void){
 assert(ethernet_socket(SOCK_STREAM,2,80)==17&&closed==0);
 events=POLLERR;
#ifdef PREVIOUS
 assert(ethernet_socket(SOCK_STREAM,2,80)==17&&closed==0);
 puts("PREVIOUS_FALSE_SUCCESS_REPRODUCED");
#else
 for(int i=0;i<5;i++){
  int masks[]={POLLERR,POLLHUP,POLLNVAL,POLLOUT|POLLERR,POLLIN};events=masks[i];
  assert(ethernet_socket(SOCK_STREAM,2,80)==-EIO&&closed==i+1);
 }
 events=POLLOUT;so_error=ECONNREFUSED;
 assert(ethernet_socket(SOCK_STREAM,2,80)==-ECONNREFUSED&&closed==6);
 so_error=0;poll_result=0;assert(ethernet_socket(SOCK_STREAM,2,80)==-ETIMEDOUT&&closed==7);
 poll_result=-1;assert(ethernet_socket(SOCK_STREAM,2,80)==-EBUSY&&closed==8);
 leased=false;assert(ethernet_socket(SOCK_STREAM,2,80)==-ENETDOWN&&closed==8);
 puts("ETHERNET_PROBE_READY_OK");
#endif
 return 0;
}
'''
results={}
for label,path in [('previous',a.previous),('fixed',source)]:
 file=out/(label+'.c');file.write_text(base+function(path.read_text(),'ethernet_socket')+main);exe=out/label
 cmd=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(file),'-o',str(exe)]
 if label=='previous':cmd.insert(1,'-DPREVIOUS')
 subprocess.run(cmd,check=True);result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
 (out/(label+'.log')).write_text(result.stdout+result.stderr);assert result.returncode==0,result.stderr
 print(result.stdout,end='');results[label]=result.stdout.strip()
(out/'RESULT.json').write_text(json.dumps({'passed':True,'results':results,'production_sha256':hashlib.sha256(source.read_bytes()).hexdigest()},indent=2))
