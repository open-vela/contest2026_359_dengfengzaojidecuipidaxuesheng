/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_portal.h"
#ifdef CONFIG_SYSTEM_HASS
#include "hass_portal.h"
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <ctype.h>
#include <poll.h>
#include <stdatomic.h>

#define PAIR_SECONDS 900
#define SESSION_IDLE_SECONDS 900
#define PAIR_ATTEMPTS 5
#define PAIR_WINDOW_SECONDS 30
#define PENDING_CLIENTS 4
#define HEADER_TIMEOUT_MS 5000
#define EMPTY_TIMEOUT_MS 2000
#define IO_IDLE_MS 5000
#define REQUEST_TIMEOUT_MS 120000
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

extern const unsigned char *portal_asset(const char *,size_t *,const char **);
static pthread_mutex_t session_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t service_lock=PTHREAD_MUTEX_INITIALIZER;
static char pairing_code[PORTAL_CODE_LENGTH+1],session_token[PORTAL_TOKEN_LENGTH+1];
static time_t pairing_expires,session_expires,pair_window;
static unsigned pair_attempts;
static atomic_int listener=-1;
static bool service_running;
static atomic_uint portal_accepts,portal_closes,portal_active,portal_receives,portal_stage,portal_errors;
static atomic_int portal_last_error;
/* The single request worker owns these and the file/install scratch buffers. */
static int64_t io_deadline;
static bool io_failed;

void portal_debug_status(void)
{
  pthread_mutex_lock(&service_lock);
  bool running=service_running;
  pthread_mutex_unlock(&service_lock);
  /* Never copy request headers, pairing codes or bearer tokens into logs. */
  printf("portal listener=%d running=%d accepts=%u closes=%u active=%u receives=%u errors=%u error=%d stage=%u\n",
    atomic_load(&listener),running,atomic_load(&portal_accepts),atomic_load(&portal_closes),
    atomic_load(&portal_active),atomic_load(&portal_receives),atomic_load(&portal_errors),
    atomic_load(&portal_last_error),atomic_load(&portal_stage));
}
static time_t monotime(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec; }
static int64_t milliseconds(void)
{
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  return (int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
}
static int random_bytes(void *out,size_t size)
{
  int fd=open("/dev/urandom",O_RDONLY); if(fd<0) return -errno;
  unsigned char *p=out; int ret=0;
  while(size) {
    ssize_t n=read(fd,p,size);
    if(n<0&&errno==EINTR) continue;
    if(n<=0) { ret=n<0?-errno:-EIO; break; }
    p+=n; size-=n;
  }
  close(fd); return ret;
}
static int pairing_open(char *out,size_t cap,bool refresh)
{
  if(!out||cap<sizeof(pairing_code)) return -EINVAL;
  pthread_mutex_lock(&session_lock);
  int ret=0; time_t now=monotime();
  if(refresh||pairing_expires<=now) {
    char next[sizeof(pairing_code)]; uint32_t number;
    do {
      do { ret=random_bytes(&number,sizeof(number)); } while(!ret&&number>=4294000000u);
      if(ret) break;
      snprintf(next,sizeof(next),"%06lu",(unsigned long)(number%1000000u));
    } while(!strcmp(next,pairing_code));
    if(!ret) {
      memcpy(pairing_code,next,sizeof(pairing_code)); pairing_expires=now+PAIR_SECONDS;
      pair_attempts=0; pair_window=now;
    }
  }
  if(!ret) memcpy(out,pairing_code,sizeof(pairing_code));
  pthread_mutex_unlock(&session_lock); return ret;
}
int portal_session_open(char *out,size_t cap) { return pairing_open(out,cap,false); }
int portal_session_refresh(char *out,size_t cap) { return pairing_open(out,cap,true); }
void portal_session_close(void)
{
  pthread_mutex_lock(&session_lock);
  pairing_expires=session_expires=0; pair_attempts=0; pair_window=0;
  memset(pairing_code,0,sizeof(pairing_code)); memset(session_token,0,sizeof(session_token));
  pthread_mutex_unlock(&session_lock);
}
unsigned portal_session_seconds(void)
{ pthread_mutex_lock(&session_lock); time_t left=pairing_expires-monotime(); pthread_mutex_unlock(&session_lock); return left>0?(unsigned)left:0; }
static bool authorized(const char *value)
{
  if(strncmp(value,"Bearer ",7)||strlen(value+7)!=PORTAL_TOKEN_LENGTH) return false;
  pthread_mutex_lock(&session_lock); unsigned diff=0;
  for(unsigned i=0;i<PORTAL_TOKEN_LENGTH;i++) diff|=(unsigned char)value[i+7]^(unsigned char)session_token[i];
  time_t now=monotime(); bool ok=!diff&&session_expires>now;
  if(ok) session_expires=now+SESSION_IDLE_SECONDS;
  pthread_mutex_unlock(&session_lock); return ok;
}
static int wait_socket(int fd,short events,int64_t idle_deadline)
{
  while(!io_failed) {
    int64_t end=idle_deadline<io_deadline?idle_deadline:io_deadline;
    int64_t left=end-milliseconds();
    if(left<=0) { portal_last_error=ETIMEDOUT; break; }
    struct pollfd p={.fd=fd,.events=events};
    int ret=poll(&p,1,(int)left);
    if(ret<0&&errno==EINTR) continue;
    if(ret>0&&(p.revents&events)) return 0;
    portal_last_error=ret==0?ETIMEDOUT:ret<0?errno:EIO; break;
  }
  io_failed=true; return -EIO;
}
static int sendall(int fd,const void *data,size_t n)
{
  const char *p=data; int64_t idle=milliseconds()+IO_IDLE_MS;
  while(n) {
    if(wait_socket(fd,POLLOUT,idle)) return -EIO;
    ssize_t sent=send(fd,p,n,MSG_NOSIGNAL);
    if(sent<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)) continue;
    if(sent<=0) { portal_last_error=sent<0?errno:EIO; io_failed=true; return -EIO; }
    p+=sent; n-=sent; idle=milliseconds()+IO_IDLE_MS;
  }
  return 0;
}
static void head(int fd,int status,const char *type,size_t size,bool download)
{
  static char h[1024]; int n=snprintf(h,sizeof(h),
    "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nX-Frame-Options: DENY\r\n%s%s\r\n",
    status,status==200?"OK":"Error",type,(unsigned long)size,
    download?"Content-Disposition: attachment\r\n":"",
    !strcmp(type,"text/html; charset=utf-8")?"Content-Security-Policy: default-src 'self'; script-src 'self' https://cdn.jsdelivr.net https://unpkg.com; style-src 'self'; connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'; frame-ancestors 'none'\r\n":"");
  portal_stage=32;
  sendall(fd,h,n);
  portal_stage=33;
}
static void json_reply(int fd,int status,cJSON *o)
{ portal_stage=31; char *s=o?cJSON_PrintUnformatted(o):NULL; if(!s) s=strdup("{\"error\":\"资源不足\"}"); if(s) { head(fd,status,"application/json; charset=utf-8",strlen(s),false); portal_stage=34; sendall(fd,s,strlen(s)); portal_stage=35; free(s); } cJSON_Delete(o); }
static void result(int fd,int ret,const char *msg)
{
  cJSON *o=cJSON_CreateObject(); cJSON_AddBoolToObject(o,"ok",ret==0);
  if(ret) { cJSON_AddNumberToObject(o,"code",-ret); cJSON_AddStringToObject(o,"error",msg&&*msg?msg:ret==-ENOSPC?"设备空间不足":ret==-EEXIST?"文件或应用已存在":ret==-ENOENT?"文件不存在":"请求失败，请检查输入或设备存储"); }
  json_reply(fd,ret?400:200,o);
}
static int decode(char *s)
{
  char *out=s; while(*s) {
    if(*s=='%') { if(!s[1]||!s[2]||!isxdigit((unsigned char)s[1])||!isxdigit((unsigned char)s[2])) return -EINVAL;
      char hex[3]={s[1],s[2],0}; int c=strtol(hex,NULL,16); if(c<32||c==127) return -EINVAL; *out++=c; s+=3;
    } else { if((unsigned char)*s<32) return -EINVAL; *out++=*s++; }
  } *out=0; return 0;
}
static int receive(int fd,void *buf,size_t size)
{
  char *p=buf; int64_t idle=milliseconds()+IO_IDLE_MS;
  while(size) {
    if(wait_socket(fd,POLLIN,idle)) return -EIO;
    ssize_t n=recv(fd,p,size,0); portal_receives++;
    if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)) continue;
    if(n<=0) { portal_last_error=n<0?errno:EIO; io_failed=true; return -EIO; }
    p+=n; size-=n; idle=milliseconds()+IO_IDLE_MS;
  }
  return 0;
}
/* Device libc string scanners are avoided on the request path: a bounded
 * manual split keeps this loop from depending on scanf end-of-input handling.
 */
static bool take_token(char **cursor,char *out,size_t capacity)
{
  char *p=*cursor; while(*p==' '||*p=='\t') p++;
  if(!*p) return false;
  char *start=p; while(*p&&*p!=' '&&*p!='\t') p++;
  size_t length=(size_t)(p-start); *cursor=p;
  if(length+1>capacity) return false;
  memcpy(out,start,length); out[length]=0; return true;
}
static int upload(int fd,const char *path,size_t size,const char *auth)
{
  char temp[256]; if(snprintf(temp,sizeof(temp),"%s",path)>=(int)sizeof(temp)) return -EINVAL;
  char *name=strrchr(temp,'/'); if(!name) return -EINVAL;
  strcpy(name+1,".upload");
  int out=open(temp,O_CREAT|O_TRUNC|O_WRONLY,0600); if(out<0) return -errno;
  static char buf[4096]; int ret=0;
  while(size) { size_t n=size<sizeof(buf)?size:sizeof(buf);
    if(!authorized(auth)) { ret=-EACCES; break; }
    if((ret=receive(fd,buf,n))<0) break;
    size_t written=0; while(written<n) { ssize_t w=write(out,buf+written,n-written); if(w<=0) { ret=w<0?-errno:-EIO; break; } written+=w; }
    if(ret) break; size-=n;
  }
  if(!ret && fsync(out)<0) ret=-errno;
  if(close(out)<0 && !ret) ret=-errno;
  if(!ret && rename(temp,path)<0) ret=-errno;
  if(ret) unlink(temp); return ret;
}
static void files(int fd,const char *path)
{
  DIR *d=opendir(path); if(!d) { result(fd,-errno,NULL); return; }
  cJSON *o=cJSON_CreateObject(),*items=cJSON_AddArrayToObject(o,"items"); struct dirent *e; unsigned count=0;
  while((e=readdir(d)) && count<256) {
    /* Only the two synthetic entries are hidden: an application keeps its
     * private data in dot directories (.data) and the manager browses the
     * whole volume now. */
    if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
    char full[256]; struct stat st;
    if(snprintf(full,sizeof(full),"%s/%s",path,e->d_name)>=(int)sizeof(full)||lstat(full,&st)<0||S_ISLNK(st.st_mode)) continue;
    cJSON *v=cJSON_CreateObject(); cJSON_AddStringToObject(v,"name",e->d_name); cJSON_AddBoolToObject(v,"directory",S_ISDIR(st.st_mode)); cJSON_AddNumberToObject(v,"size",st.st_size); cJSON_AddItemToArray(items,v); count++;
  }
  closedir(d); cJSON_AddBoolToObject(o,"truncated",count==256); json_reply(fd,200,o);
}
static cJSON *receive_json(int fd,size_t size)
{
  char *body=malloc(size+1); if(!body) return NULL;
  if(receive(fd,body,size)) { free(body); return NULL; } body[size]=0;
  unsigned depth=0; bool quoted=false,escape=false,invalid=false;
  for(size_t i=0;i<size;i++) { char c=body[i]; if(!c) { invalid=true; break; }
    if(escape) { escape=false; continue; } if(quoted&&c=='\\') { escape=true; continue; } if(c=='"') quoted=!quoted;
    if(!quoted&&(c=='{'||c=='[')&&++depth>8) { invalid=true; break; }
    if(!quoted&&(c=='}'||c==']')) { if(!depth) { invalid=true; break; } depth--; }
  }
  cJSON *o=invalid?NULL:cJSON_ParseWithOpts(body,NULL,true); free(body); return o;
}
static void pair_client(int fd,const cJSON *body)
{
  const cJSON *item=cJSON_GetObjectItemCaseSensitive(body,"code");
  if(!cJSON_IsObject(body)||cJSON_GetArraySize(body)!=1||!cJSON_IsString(item)||
     strlen(item->valuestring)!=PORTAL_CODE_LENGTH) { result(fd,-EINVAL,"请输入设备屏幕上的 6 位数字令牌"); return; }
  for(unsigned i=0;i<PORTAL_CODE_LENGTH;i++)
    if(item->valuestring[i]<'0'||item->valuestring[i]>'9') { result(fd,-EINVAL,"请输入设备屏幕上的 6 位数字令牌"); return; }

  char token[sizeof(session_token)]=""; unsigned retry_after=0;
  int status=200; const char *error=NULL;
  pthread_mutex_lock(&session_lock);
  time_t now=monotime();
  if(now>=pair_window+PAIR_WINDOW_SECONDS) { pair_window=now; pair_attempts=0; }
  if(pairing_expires<=now) { status=410; error="令牌已失效，请在设备上刷新令牌"; }
  else if(pair_attempts>=PAIR_ATTEMPTS) {
    status=429; error="尝试次数较多，请稍后再试";
    retry_after=(unsigned)(pair_window+PAIR_WINDOW_SECONDS-now);
  } else {
    unsigned diff=0;
    for(unsigned i=0;i<PORTAL_CODE_LENGTH;i++) diff|=(unsigned char)item->valuestring[i]^(unsigned char)pairing_code[i];
    if(diff) { pair_attempts++; status=401; error="令牌不正确，请核对设备屏幕上的 6 位数字"; }
    else {
      if(session_expires<=now) {
        unsigned char bytes[16];
        if(random_bytes(bytes,sizeof(bytes))) { status=503; error="设备暂时无法连接，请重试"; }
        else for(unsigned i=0;i<sizeof(bytes);i++) snprintf(session_token+i*2,3,"%02x",bytes[i]);
      }
      if(status==200) {
        session_expires=now+SESSION_IDLE_SECONDS; pair_attempts=0;
        memcpy(token,session_token,sizeof(token));
      }
    }
  }
  pthread_mutex_unlock(&session_lock);
  cJSON *reply=cJSON_CreateObject();
  if(status==200) { cJSON_AddStringToObject(reply,"token",token); cJSON_AddNumberToObject(reply,"idle_seconds",SESSION_IDLE_SECONDS); }
  else { cJSON_AddStringToObject(reply,"error",error); if(retry_after) cJSON_AddNumberToObject(reply,"retry_after",retry_after); }
  memset(token,0,sizeof(token)); json_reply(fd,status,reply);
}
static void client(int fd,char *headers)
{
  portal_stage=2;
  char method[8],url[384],version[16]; char *line=strstr(headers,"\r\n"); if(!line) return; *line=0;
  char *cursor=headers;
  if(!take_token(&cursor,method,sizeof(method))||!take_token(&cursor,url,sizeof(url))||
     !take_token(&cursor,version,sizeof(version))||strcmp(version,"HTTP/1.1")) { result(fd,-EINVAL,NULL); return; }
  portal_stage=21;
  char *auth="",*host="",*origin=""; size_t size=0; bool length_seen=false,invalid=false;
  char *p=line+2;
  while(*p) { char *end=strstr(p,"\r\n"); if(!end) { invalid=true; break; } *end=0; if(!*p) break;
    char *colon=strchr(p,':'); if(!colon) { invalid=true; break; } *colon++=0; while(*colon==' ') colon++;
    if(!strcasecmp(p,"Content-Length")) { char *tail; errno=0; unsigned long n=strtoul(colon,&tail,10); if(length_seen||!*colon||*tail||errno||n>SIZE_MAX||*colon=='-') invalid=true; size=n; length_seen=true; }
    else if(!strcasecmp(p,"Transfer-Encoding")) invalid=true;
    else if(!strcasecmp(p,"Authorization")) { if(*auth) invalid=true; auth=colon; }
    else if(!strcasecmp(p,"Host")) { if(*host) invalid=true; host=colon; }
    else if(!strcasecmp(p,"Origin")) { if(*origin) invalid=true; origin=colon; }
    p=end+2;
  }
  portal_stage=22;
  struct sockaddr_in local={0}; socklen_t local_len=sizeof(local); char expected[64],address[INET_ADDRSTRLEN];
  if(getsockname(fd,(struct sockaddr *)&local,&local_len)<0||!inet_ntop(AF_INET,&local.sin_addr,address,sizeof(address))) return;
  snprintf(expected,sizeof(expected),"%s:%d",address,PORTAL_PORT);
  if(invalid||strcmp(host,expected)||(*origin&&(strncmp(origin,"http://",7)||strcmp(origin+7,expected)))) { portal_stage=3; result(fd,-EINVAL,NULL); return; }
  portal_stage=4;
  char *arg=strchr(url,'?'); if(arg) *arg++=0;
  if(strncmp(url,"/api/",5)) {
    if(strcmp(method,"GET")) { result(fd,-EINVAL,NULL); return; }
    size_t n; const char *type; const unsigned char *data=portal_asset(url,&n,&type);
    if(!data) { result(fd,-ENOENT,NULL); return; } head(fd,200,type,n,false); sendall(fd,data,n); return;
  }
  if(!strcmp(url,"/api/pair")) {
    if(arg||strcmp(method,"POST")||!length_seen||!size||size>64) { result(fd,-EINVAL,NULL); return; }
    cJSON *o=receive_json(fd,size); if(!io_failed) pair_client(fd,o); cJSON_Delete(o); return;
  }
  if(!authorized(auth)) { cJSON *o=cJSON_CreateObject(); cJSON_AddStringToObject(o,"error","连接已失效，请输入设备上的令牌或重新扫码"); json_reply(fd,401,o); return; }
  if(arg) { if(strncmp(arg,"path=",5)&&strncmp(arg,"section=",8)) { result(fd,-EINVAL,NULL); return; } arg=strchr(arg,'=')+1; if(decode(arg)) { result(fd,-EINVAL,NULL); return; } }
  if(!strcmp(url,"/api/config")&&!strcmp(method,"GET")&&arg) { cJSON *o=portal_config_get(arg); if(!o) result(fd,-EIO,NULL); else json_reply(fd,200,o); return; }
  char path[256];
  if(!strcmp(url,"/api/files")&&!strcmp(method,"GET")&&arg) { int ret=portal_public_path(arg,path,sizeof(path)); if(ret) result(fd,ret,NULL); else files(fd,path); return; }
  if(!strcmp(url,"/api/file")&&arg) {
    int ret=portal_public_path(arg,path,sizeof(path)); if(ret) { result(fd,ret,NULL); return; }
    if(!strcmp(method,"PUT")&&length_seen) { struct stat st; if(lstat(path,&st)==0) { result(fd,-EEXIST,NULL); return; } result(fd,upload(fd,path,size,auth),NULL); return; }
    if(!strcmp(method,"GET")) {
      struct stat st; int in=open(path,O_RDONLY); if(in<0) { result(fd,-errno,NULL); return; }
      if(fstat(in,&st)<0||!S_ISREG(st.st_mode)) { close(in); result(fd,-EINVAL,NULL); return; }
      head(fd,200,"application/octet-stream",st.st_size,true); static char buf[4096]; ssize_t n;
      while(authorized(auth)&&(n=read(in,buf,sizeof(buf)))>0) if(sendall(fd,buf,n)) break; close(in); return;
    }
  }
  if(!strcmp(url,"/api/install/file")&&!strcmp(method,"PUT")&&arg&&length_seen) {
    int ret=portal_install_target(arg,path,sizeof(path)); struct stat st;
    if(!ret&&lstat(path,&st)==0) ret=-EEXIST;
    if(!ret) ret=upload(fd,path,size,auth);
    result(fd,ret,NULL); return;
  }
  if(strcmp(method,"POST")||!length_seen||size>8192) { result(fd,-EINVAL,NULL); return; }
  cJSON *o=receive_json(fd,size);
  if(!cJSON_IsObject(o)||!authorized(auth)) { cJSON_Delete(o); result(fd,-EINVAL,NULL); return; }
  int ret=-EINVAL; char error[128]="";
  if(!strcmp(url,"/api/config")&&arg) {
    ret=portal_config_save(arg,o);
#ifdef CONFIG_SYSTEM_HASS
    if(!ret&&!strcmp(arg,"homeassistant")) ret=hass_portal_bootstrap();
#endif
  }
  else if(!strcmp(url,"/api/mkdir")&&arg) { ret=portal_public_path(arg,path,sizeof(path)); if(!ret&&mkdir(path,0700)<0) ret=-errno; }
  else if(!strcmp(url,"/api/install/begin")) { ret=portal_install_begin(o,error,sizeof(error)); }
  else if(!strcmp(url,"/api/install/commit")) ret=portal_install_commit();
  else if(!strcmp(url,"/api/install/abort")) { portal_install_abort(); ret=0; }
  cJSON_Delete(o); result(fd,ret,error);
}
struct pending_client {
  int fd;
  size_t used;
  int64_t opened;
  char headers[4096];
};
static void close_client(struct pending_client *p)
{
  if(p->fd>=0) { close(p->fd); portal_closes++; portal_active--; }
  p->fd=-1; p->used=0;
}
/* Consume only the headers; a PUT/POST body stays in the socket for client().
 * Readiness is multiplexed so a browser's empty preconnection cannot hold up
 * requests on its other sockets. Both empty and slow headers have deadlines.
 */
static int read_headers(struct pending_client *p)
{
  size_t room=sizeof(p->headers)-1-p->used;
  if(!room) return -E2BIG;
  ssize_t n=recv(p->fd,p->headers+p->used,room,MSG_PEEK); portal_receives++;
  if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)) return 0;
  if(n<=0) return -EIO;
  size_t end=0;
  for(size_t i=p->used>3?p->used-3:0;i+4<=p->used+(size_t)n;i++)
    if(!memcmp(p->headers+i,"\r\n\r\n",4)) { end=i+4; break; }
  size_t take=end?end-p->used:(size_t)n;
  n=recv(p->fd,p->headers+p->used,take,0); portal_receives++;
  if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)) return 0;
  if(n<=0) return -EIO;
  if(memchr(p->headers+p->used,0,(size_t)n)) return -EINVAL;
  p->used+=n; p->headers[p->used]=0;
  return end&&p->used==end?1:0;
}
static int open_listener(void)
{
  int fd=socket(AF_INET,SOCK_STREAM,0); if(fd<0) return -errno;
  int yes=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
  struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(PORTAL_PORT),.sin_addr.s_addr=INADDR_ANY};
  if(fcntl(fd,F_SETFL,O_NONBLOCK)<0||bind(fd,(struct sockaddr *)&addr,sizeof(addr))<0||listen(fd,8)<0) {
    int ret=-errno; close(fd); return ret;
  }
  return fd;
}
static void retry_pause(void) { struct timespec delay={0,200000000}; nanosleep(&delay,NULL); }
static void *worker(void *arg)
{
  struct pending_client *pending=arg;
  int64_t accept_after=0;
  for(;;) {
    int listen_fd=atomic_load(&listener);
    if(listen_fd<0) {
      listen_fd=open_listener();
      if(listen_fd<0) { portal_last_error=-listen_fd; portal_errors++; retry_pause(); continue; }
      atomic_store(&listener,listen_fd);
    }
    struct pollfd events[PENDING_CLIENTS+1]; bool room=false;
    int64_t now=milliseconds();
    for(unsigned i=0;i<PENDING_CLIENTS;i++) {
      struct pending_client *p=pending+i;
      if(p->fd>=0&&now-p->opened>=(p->used?HEADER_TIMEOUT_MS:EMPTY_TIMEOUT_MS)) close_client(p);
      events[i+1]=(struct pollfd){.fd=p->fd,.events=POLLIN};
      if(p->fd<0) room=true;
    }
    events[0]=(struct pollfd){.fd=room&&now>=accept_after?listen_fd:-1,.events=POLLIN};
    portal_stage=0;
    int ready=poll(events,PENDING_CLIENTS+1,200);
    if(ready<0) {
      if(errno==EINTR) continue;
      portal_last_error=errno; portal_errors++;
      if(errno==EBADF) {
        close(listen_fd); atomic_store(&listener,-1);
        for(unsigned i=0;i<PENDING_CLIENTS;i++) close_client(pending+i);
      }
      retry_pause(); continue;
    }
    for(unsigned i=0;i<PENDING_CLIENTS;i++) {
      struct pending_client *p=pending+i; short flags=events[i+1].revents;
      if(p->fd<0||!flags) continue;
      if(flags&POLLIN) {
        portal_stage=1; int ret=read_headers(p);
        if(ret>0) {
          io_failed=false; io_deadline=milliseconds()+REQUEST_TIMEOUT_MS;
          client(p->fd,p->headers); close_client(p);
        } else if(ret<0) close_client(p);
      } else if(flags&(POLLERR|POLLHUP|POLLNVAL)) close_client(p);
    }
    if(events[0].revents&(POLLERR|POLLHUP|POLLNVAL)) {
      close(listen_fd); atomic_store(&listener,-1); portal_errors++; continue;
    }
    if(!(events[0].revents&POLLIN)) continue;
    for(unsigned i=0;i<PENDING_CLIENTS;i++) if(pending[i].fd<0) {
      int fd=accept(listen_fd,NULL,NULL);
      if(fd<0) {
        int error=errno;
        if(error==EAGAIN||error==EWOULDBLOCK||error==EINTR) break;
        portal_last_error=error; portal_errors++;
        if(error==EBADF||error==EINVAL||error==ENOTSOCK) { close(listen_fd); atomic_store(&listener,-1); }
        /* Aborted handshakes and temporary resource/network errors never
         * terminate the service or leave a stale "started" listener. */
        accept_after=milliseconds()+200; break;
      }
      if(fcntl(fd,F_SETFL,O_NONBLOCK)<0) { close(fd); continue; }
      struct timeval timeout={IO_IDLE_MS/1000,0};
      setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
      setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
      pending[i].fd=fd; pending[i].used=0; pending[i].opened=milliseconds();
      portal_accepts++; portal_active++;
    }
  }
  return NULL;
}
int portal_service_start(void)
{
  pthread_mutex_lock(&service_lock);
  if(service_running) { pthread_mutex_unlock(&service_lock); return 0; }
  /* Heap storage keeps the P4 startup BSS/idle stacks within internal SRAM. */
  struct pending_client *pending=calloc(PENDING_CLIENTS,sizeof(*pending));
  if(!pending) { pthread_mutex_unlock(&service_lock); return -ENOMEM; }
  for(unsigned i=0;i<PENDING_CLIENTS;i++) pending[i].fd=-1;
  int fd=open_listener();
  if(fd<0) { free(pending); pthread_mutex_unlock(&service_lock); return fd; }
  atomic_store(&listener,fd);
  pthread_t thread; pthread_attr_t attr; pthread_attr_init(&attr);
#ifndef __linux__
  pthread_attr_setstacksize(&attr,49152);
#endif
  int ret=pthread_create(&thread,&attr,worker,pending); pthread_attr_destroy(&attr);
  if(ret) { close(fd); atomic_store(&listener,-1); free(pending); }
  else { service_running=true; pthread_detach(thread); }
  pthread_mutex_unlock(&service_lock); return -ret;
}
