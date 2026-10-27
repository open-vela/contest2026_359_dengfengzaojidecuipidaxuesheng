/* SPDX-License-Identifier: Apache-2.0 */
/* Actual portal startup/worker/router/assets on Linux TCP. Only platform-fault
 * injection, joinable shutdown and fake configuration services are supplied.
 * All credentials belong to this isolated native process. */
#include <assert.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include "../overlay/apps/system/desktop/glass_portal.h"

static unsigned short fixture_port;
static atomic_bool fixture_stop,fixture_block_bind;
static atomic_int fixture_fault,fixture_send_budget=-1;
static atomic_uint fixture_faults,fixture_listener_closes;
static pthread_t fixture_thread;
static void *fixture_pending;
static int fixture_held=-1;
static int fixture_poll(struct pollfd *,nfds_t,int);
static int fixture_close(int);
static int fixture_bind(int,const struct sockaddr *,socklen_t);
static ssize_t fixture_send(int,const void *,size_t,int);
static int fixture_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
static int fixture_detach(pthread_t);
#undef PORTAL_PORT
#define PORTAL_PORT fixture_port
#define poll fixture_poll
#define close fixture_close
#define bind fixture_bind
#define send fixture_send
#define pthread_create fixture_create
#define pthread_detach fixture_detach
#ifndef PORTAL_HTTP_SOURCE
#define PORTAL_HTTP_SOURCE "../overlay/apps/system/desktop/glass_portal_http.c"
#endif
#include PORTAL_HTTP_SOURCE
#undef poll
#undef close
#undef bind
#undef send
#undef pthread_create
#undef pthread_detach

static int fixture_create(pthread_t *thread,const pthread_attr_t *attr,
                           void *(*entry)(void *),void *arg)
{
  fixture_pending=arg;
  int ret=pthread_create(thread,attr,entry,arg);
  if(!ret) fixture_thread=*thread;
  return ret;
}
static int fixture_detach(pthread_t thread)
{ (void)thread;return 0; /* fixture shutdown joins the real worker */ }
static int fixture_close(int fd)
{
  if(fd>=0&&fd==atomic_load(&listener)) fixture_listener_closes++;
  return close(fd);
}
static int fixture_bind(int fd,const struct sockaddr *address,socklen_t size)
{
  if(atomic_load(&fixture_block_bind)) { errno=EADDRINUSE;return -1; }
  return bind(fd,address,size);
}
static ssize_t fixture_send(int fd,const void *buffer,size_t size,int flags)
{
  int budget=atomic_load(&fixture_send_budget);
  if(budget==0) {
    atomic_store(&fixture_send_budget,-1);errno=ECONNRESET;return -1;
  }
  if(budget>0&&size>(size_t)budget) size=(size_t)budget;
  ssize_t result=send(fd,buffer,size,flags);
  if(result>0&&budget>0) atomic_fetch_sub(&fixture_send_budget,(int)result);
  return result;
}
static int fixture_poll(struct pollfd *events,nfds_t count,int timeout)
{
  if(count==PENDING_CLIENTS+1) {
    if(atomic_load(&fixture_stop)) {
      struct pending_client *pending=fixture_pending;
      for(unsigned i=0;i<PENDING_CLIENTS;i++) close_client(pending+i);
      int fd=atomic_exchange(&listener,-1);if(fd>=0)close(fd);
      free(pending);fixture_pending=NULL;pthread_exit(NULL);
    }
    int fault=atomic_exchange(&fixture_fault,0);
    if(fault) {
      for(nfds_t i=0;i<count;i++) events[i].revents=0;
      fixture_faults++;
      if(fault==1) { events[0].revents=POLLERR;return 1; }
      if(fault==2) {
        for(nfds_t i=1;i<count;i++) if(events[i].fd>=0) {
          events[i].revents=POLLERR;return 1;
        }
        assert(false); /* controller waits for an accepted empty connection */
      }
      if(fault==3) {
        /* A real invalid client descriptor accompanies the whole-poll error;
         * the healthy listening descriptor must remain owned and open. */
        bool closed=false;
        for(nfds_t i=1;i<count;i++) if(events[i].fd>=0) {
          close(events[i].fd);closed=true;break;
        }
        assert(closed);errno=EBADF;return -1;
      }
      assert(false);
    }
  }
  return poll(events,count,timeout);
}

int glass_network_recovery_read(struct glass_recovery_snapshot_s *out)
{
  *out=(struct glass_recovery_snapshot_s){.state=GLASS_RECOVERY_READY,
    .ap_running=true,.dhcp_ready=true,.ap_ipv4={192,0,2,1}};
  strcpy(out->ssid,"Native-Fixture");return 0;
}
int glass_wifi_service_submit(enum c6_desktop_operation op,const char *ssid,const char *password)
{ (void)op;(void)ssid;(void)password;return -EBUSY; }
cJSON *portal_config_get(const char *section)
{
  cJSON *reply=cJSON_CreateObject();cJSON_AddStringToObject(reply,"fixture_section",section);
  return reply;
}
int portal_config_save(const char *section,const cJSON *patch)
{ (void)section;(void)patch;assert(false);return -EINVAL; }
int portal_public_path(const char *path,char *out,size_t size)
{ (void)path;(void)out;(void)size;assert(false);return -EINVAL; }
int portal_install_target(const char *path,char *out,size_t size)
{ (void)path;(void)out;(void)size;assert(false);return -EINVAL; }
int portal_install_begin(const cJSON *value,char *error,size_t size)
{ (void)value;(void)error;(void)size;assert(false);return -EINVAL; }
int portal_install_commit(void) { assert(false);return -EINVAL; }
void portal_install_abort(void) { assert(false); }

static void report(const char *code)
{
  int fd=atomic_load(&listener),flags=fd>=0?fcntl(fd,F_GETFD):-1;
  unsigned clients=0,clients_cloexec=0;
  struct pending_client *pending=fixture_pending;
  if(pending)for(unsigned i=0;i<PENDING_CLIENTS;i++)if(pending[i].fd>=0) {
    clients++;int value=fcntl(pending[i].fd,F_GETFD);
    if(value>=0&&(value&FD_CLOEXEC))clients_cloexec++;
  }
  printf("{\"port\":%u,\"code\":\"%s\",\"listener\":%d,\"listener_cloexec\":%s,"
    "\"accepts\":%u,\"closes\":%u,\"active\":%u,\"errors\":%u,\"error\":%d,"
    "\"stage\":%u,\"listener_closes\":%u,\"faults\":%u,\"clients\":%u,"
    "\"clients_cloexec\":%u,\"held\":%s}\n",fixture_port,code?code:"",fd,
    flags>=0&&(flags&FD_CLOEXEC)?"true":"false",atomic_load(&portal_accepts),
    atomic_load(&portal_closes),atomic_load(&portal_active),atomic_load(&portal_errors),
    atomic_load(&portal_last_error),atomic_load(&portal_stage),
    atomic_load(&fixture_listener_closes),atomic_load(&fixture_faults),clients,
    clients_cloexec,fixture_held>=0?"true":"false");fflush(stdout);
}

int main(void)
{
  /* Reserve an ephemeral port, then let unmodified service_start bind it. */
  int fd=socket(AF_INET,SOCK_STREAM,0);assert(fd>=0);
  struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(!bind(fd,(struct sockaddr *)&address,sizeof(address)));
  socklen_t size=sizeof(address);assert(!getsockname(fd,(struct sockaddr *)&address,&size));
  fixture_port=ntohs(address.sin_port);assert(fixture_port);close(fd);
  char code[PORTAL_CODE_LENGTH+1];assert(!portal_session_open(code,sizeof(code)));
  assert(!portal_service_start());assert(!portal_service_start());report(code);
  char command[80];
  while(fgets(command,sizeof(command),stdin)) {
    if(!strncmp(command,"STOP",4)) break;
    if(!strncmp(command,"LISTENERERR",11))atomic_store(&fixture_fault,1);
    else if(!strncmp(command,"CLIENTERR",9))atomic_store(&fixture_fault,2);
    else if(!strncmp(command,"POLL_EBADF",10))atomic_store(&fixture_fault,3);
    else if(!strncmp(command,"TRUNCATE",8))atomic_store(&fixture_send_budget,2048);
    else if(!strncmp(command,"INVALID_BLOCK",13)) {
      atomic_store(&fixture_block_bind,true);fd=atomic_load(&listener);if(fd>=0)close(fd);
    } else if(!strncmp(command,"INVALID",7)) {
      fd=atomic_load(&listener);if(fd>=0)close(fd);
    } else if(!strncmp(command,"UNBLOCK",7))atomic_store(&fixture_block_bind,false);
    else if(!strncmp(command,"HOLD",4)) {
      fd=atomic_load(&listener);assert(fd>=0);
      /* Model NuttX task_create's descriptor clone rule: only non-CLOEXEC
       * listener references survive into the child task group. dup keeps a
       * real Linux listening socket reference and therefore a real bind. */
      if(!(fcntl(fd,F_GETFD)&FD_CLOEXEC))fixture_held=dup(fd);
    } else if(!strncmp(command,"RELEASE",7)) {
      if(fixture_held>=0)close(fixture_held);fixture_held=-1;
    } else if(strncmp(command,"STATUS",6)) assert(false);
    report(NULL);
  }
  atomic_store(&fixture_block_bind,false);
  if(fixture_held>=0)close(fixture_held);
  atomic_store(&fixture_stop,true);assert(!pthread_join(fixture_thread,NULL));
  portal_session_close();return 0;
}
