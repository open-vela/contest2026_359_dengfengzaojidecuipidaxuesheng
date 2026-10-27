/* SPDX-License-Identifier: Apache-2.0 */
/* The production HTTP parser/auth/router runs on a real loopback TCP pair.
 * All configuration strings and bearer tokens below are synthetic fixtures.
 */
#include <assert.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include "../overlay/apps/system/desktop/glass_portal.h"

static unsigned short fixture_port;
static time_t fixture_seconds = 100;
static bool revoke_after_receive;
static int fixture_clock(clockid_t clock, struct timespec *value)
{
  (void)clock;value->tv_sec=fixture_seconds;value->tv_nsec=0;return 0;
}
static ssize_t fixture_recv(int fd,void *buffer,size_t size,int flags);
#undef PORTAL_PORT
#define PORTAL_PORT fixture_port
#define clock_gettime fixture_clock
#define recv fixture_recv
#include "../overlay/apps/system/desktop/glass_portal_http.c"
#undef recv
#undef clock_gettime

static ssize_t fixture_recv(int fd,void *buffer,size_t size,int flags)
{
  ssize_t count=recv(fd,buffer,size,flags);
  if(count>0&&revoke_after_receive)
    {revoke_after_receive=false;portal_session_close();}
  return count;
}

static struct glass_recovery_snapshot_s fixture_state;
static int fixture_read_error,fixture_submit_error;
static unsigned reads,submits,accepted,credential_reads,cases;
static char submitted_ssid[128],submitted_password[128];

int glass_network_recovery_read(struct glass_recovery_snapshot_s *out)
{reads++;if(fixture_read_error)return fixture_read_error;*out=fixture_state;return 0;}
int glass_wifi_service_submit(enum c6_desktop_operation operation,
                               const char *ssid,const char *password)
{
  assert(operation==C6_DESKTOP_CONNECT);submits++;
  assert(strlen(ssid)<sizeof(submitted_ssid));
  assert(strlen(password)<sizeof(submitted_password));
  strcpy(submitted_ssid,ssid);strcpy(submitted_password,password);
  if(fixture_submit_error)return fixture_submit_error;
  accepted++;return 0;
}
int glass_network_recovery_credentials(char *ssid,size_t ssid_size,
                                        char *password,size_t password_size)
{
  (void)ssid;(void)ssid_size;(void)password;(void)password_size;
  credential_reads++;abort(); /* The HTTP router must never request a PSK. */
}
const unsigned char *portal_asset(const char *path,size_t *size,const char **type)
{(void)path;(void)size;(void)type;return NULL;}
cJSON *portal_config_get(const char *section)
{(void)section;return cJSON_CreateObject();}
int portal_config_save(const char *section,const cJSON *patch)
{(void)section;(void)patch;assert(false);return -EINVAL;}
int portal_public_path(const char *path,char *out,size_t capacity)
{(void)path;(void)out;(void)capacity;assert(false);return -EINVAL;}
int portal_install_target(const char *path,char *out,size_t capacity)
{(void)path;(void)out;(void)capacity;assert(false);return -EINVAL;}
int portal_install_begin(const cJSON *manifest,char *error,size_t capacity)
{(void)manifest;(void)error;(void)capacity;assert(false);return -EINVAL;}
int portal_install_commit(void){assert(false);return -EINVAL;}
void portal_install_abort(void){assert(false);}

struct request_s
{
  const char *method,*path,*body,*authorization,*host,*origin,*extra;
  size_t body_size,announced_size;
  bool omit_length,force_length,revoke_in_body;
};
static int fixture_listener;
static char reply[8192];
static const char fixture_bearer[]="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char fixture_authorization[]="Bearer aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

static void write_all(int fd,const char *data,size_t length)
{
  while(length)
    {ssize_t count=send(fd,data,length,MSG_NOSIGNAL);assert(count>0);data+=count;length-=(size_t)count;}
}

static cJSON *request(struct request_s q,int expected_status,
                       unsigned expected_submits,unsigned expected_reads)
{
  unsigned before_submits=submits,before_reads=reads;
  struct sockaddr_in address={.sin_family=AF_INET,
    .sin_addr.s_addr=htonl(INADDR_LOOPBACK),.sin_port=htons(fixture_port)};
  int browser=socket(AF_INET,SOCK_STREAM,0);assert(browser>=0);
  assert(connect(browser,(struct sockaddr *)&address,sizeof(address))==0);
  int device=accept(fixture_listener,NULL,NULL);assert(device>=0);
  char headers[2048],host[64],body_length[96]="",authorization[128]="",origin[128]="";
  snprintf(host,sizeof(host),"127.0.0.1:%u",fixture_port);
  size_t length=q.body_size?q.body_size:q.body?strlen(q.body):0;
  if(!q.omit_length&&(q.body||q.force_length))
    snprintf(body_length,sizeof(body_length),"Content-Length: %zu\r\n",
             q.force_length?q.announced_size:length);
  if(q.authorization)snprintf(authorization,sizeof(authorization),"Authorization: %s\r\n",q.authorization);
  if(q.origin)snprintf(origin,sizeof(origin),"Origin: %s\r\n",q.origin);
  int used=snprintf(headers,sizeof(headers),"%s %s HTTP/1.1\r\nHost: %s\r\n%s%s%s%s\r\n",
    q.method,q.path,q.host?q.host:host,authorization,origin,body_length,q.extra?q.extra:"");
  assert(used>0&&used<(int)sizeof(headers));
  write_all(browser,headers,(size_t)used);if(length)write_all(browser,q.body,length);
  assert(shutdown(browser,SHUT_WR)==0);
  struct pending_client pending={.fd=device,.opened=milliseconds()};
  int parsed=0;
  for(unsigned attempt=0;attempt<8&&!parsed;attempt++)parsed=read_headers(&pending);
  assert(parsed==1);
  io_failed=false;io_deadline=milliseconds()+REQUEST_TIMEOUT_MS;
  revoke_after_receive=q.revoke_in_body;
  client(device,pending.headers);close(device);
  size_t received=0;ssize_t count;
  while((count=recv(browser,reply+received,sizeof(reply)-1-received,0))>0)
    {received+=(size_t)count;assert(received+1<sizeof(reply));}
  assert(count==0||(count<0&&errno==ECONNRESET));reply[received]=0;
  close(browser);
  char status[48];snprintf(status,sizeof(status),"HTTP/1.1 %d ",expected_status);
  if(strncmp(reply,status,strlen(status)))
    fprintf(stderr,"HTTP case %u %s %s expected %d, received %.24s\n",
            cases+1,q.method,q.path,expected_status,reply);
  assert(!strncmp(reply,status,strlen(status)));
  assert(strstr(reply,"Cache-Control: no-store\r\n"));
  assert(!strstr(reply,fixture_bearer));
  assert(!strstr(reply,"fixture-router-password"));
  assert(!strstr(reply,"fixture-ap-password"));
  assert(submits-before_submits==expected_submits);
  assert(reads-before_reads==expected_reads);
  assert(credential_reads==0);
  const char *body=strstr(reply,"\r\n\r\n");assert(body);
  cJSON *result=cJSON_ParseWithOpts(body+4,NULL,true);assert(cJSON_IsObject(result));
  cases++;return result;
}

static void reject_body(const char *body)
{
  cJSON *result=request((struct request_s){.method="POST",.path="/api/network/connect",
    .body=body,.authorization=fixture_authorization},400,0,0);
  assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(result,"ok")));
  cJSON_Delete(result);
}

static void renew(void)
{strcpy(session_token,fixture_bearer);session_expires=fixture_seconds+SESSION_IDLE_SECONDS;}

int main(void)
{
  fixture_listener=socket(AF_INET,SOCK_STREAM,0);assert(fixture_listener>=0);
  struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(bind(fixture_listener,(struct sockaddr *)&address,sizeof(address))==0);
  assert(listen(fixture_listener,1)==0);socklen_t length=sizeof(address);
  assert(getsockname(fixture_listener,(struct sockaddr *)&address,&length)==0);
  fixture_port=ntohs(address.sin_port);assert(fixture_port);
  fixture_state=(struct glass_recovery_snapshot_s){.state=GLASS_RECOVERY_READY,
    .ap_running=true,.dhcp_ready=true,.busy=true,.associated=true,
    .operation=C6_DESKTOP_CONNECT,.ap_ipv4={172,31,240,1},.error=-EAGAIN};
  strcpy(fixture_state.ssid,"ESPClaw-Fixture");renew();
  const char *valid="{\"ssid\":\"Fixture Router\",\"password\":\"fixture-router-password\"}";
  cJSON *o;
  /* Both read and write routes are behind the real bearer/session check. */
  o=request((struct request_s){.method="GET",.path="/api/network"},401,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid},401,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="GET",.path="/api/network",.authorization="Basic fixture"},401,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization="Bearer bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},401,0,0);cJSON_Delete(o);
  session_expires=fixture_seconds;
  o=request((struct request_s){.method="GET",.path="/api/network",.authorization=fixture_authorization},401,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization},401,0,0);cJSON_Delete(o);renew();
  /* Mismatched browser authority/origin never reaches even the read service. */
  o=request((struct request_s){.method="GET",.path="/api/network",.authorization=fixture_authorization,
    .host="attacker.invalid:8080"},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.origin="http://attacker.invalid"},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.extra="Host: attacker.invalid\r\n"},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.extra="Authorization: Bearer fake\r\n"},400,0,0);cJSON_Delete(o);
  char origin[64];snprintf(origin,sizeof(origin),"http://127.0.0.1:%u",fixture_port);
  o=request((struct request_s){.method="GET",.path="/api/network",.authorization=fixture_authorization,
    .origin=origin},200,0,1);
  assert(cJSON_GetArraySize(o)==16);
  assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o,"internet_sharing")));
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"ok")));
  assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"busy")));
  assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o,"ipv4_ready")));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(o,"ipv4")->valuestring,""));
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(o,"ap_ip")->valuestring,"172.31.240.1"));
  assert(!cJSON_GetObjectItemCaseSensitive(o,"password"));
  assert(!cJSON_GetObjectItemCaseSensitive(o,"psk"));
  assert(!cJSON_GetObjectItemCaseSensitive(o,"token"));cJSON_Delete(o);
  fixture_state.busy=false;fixture_state.error=0;fixture_state.ipv4_ready=true;
  fixture_state.ethernet.link=fixture_state.ethernet.leased=fixture_state.ethernet.sharing=true;
  memcpy(fixture_state.ipv4,(uint8_t[]){192,168,9,42},4);
  o=request((struct request_s){.method="GET",.path="/api/network",.authorization=fixture_authorization},200,0,1);
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(o,"ipv4")->valuestring,"192.168.9.42"));cJSON_Delete(o);
  fixture_read_error=-EIO;
  o=request((struct request_s){.method="GET",.path="/api/network",.authorization=fixture_authorization},400,0,1);
  assert(cJSON_GetObjectItemCaseSensitive(o,"code")->valueint==EIO);cJSON_Delete(o);fixture_read_error=0;
  /* POST success confirms acceptance only, with no fabricated link/IP fields. */
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization},200,1,0);
  assert(cJSON_GetArraySize(o)==1&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o,"ok")));
  assert(!strcmp(submitted_ssid,"Fixture Router"));
  assert(!strcmp(submitted_password,"fixture-router-password"));cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",
    .body="{\"ssid\":\"  Open Router  \",\"password\":\"\"}",.authorization=fixture_authorization},200,1,0);
  assert(!strcmp(submitted_ssid,"  Open Router  ")&&!submitted_password[0]);cJSON_Delete(o);
  const char *invalid[]={"{}","[]","null","{\"ssid\":\"x\"}","{\"password\":\"x\"}",
    "{\"ssid\":null,\"password\":\"x\"}","{\"ssid\":3,\"password\":\"x\"}",
    "{\"ssid\":\"x\",\"password\":false}","{\"ssid\":\"x\",\"password\":{}}",
    "{\"ssid\":\"x\",\"password\":\"x\",\"extra\":\"y\"}",
    "{\"ssid\":\"x\",\"ssid\":\"y\",\"password\":\"z\"}",
    "{\"ssid\":\"x\",\"password\":\"y\",\"password\":\"z\"}",
    "{\"ssid\":\"x\",\"ssid\":\"y\"}","{\"SSID\":\"x\",\"password\":\"y\"}",
    "{\"ssid\":\"x\",\"password\":\"y\"} trailing",
    "{\"ssid\":\"x\\u0000suffix\",\"password\":\"fixture-router-password\"}",
    "{\"ssid\":\"x\",\"password\":\"pass\\u0000suffix\"}",
    "{\"ssid\\u0000suffix\":\"x\",\"password\":\"fixture-router-password\"}"};
  for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)reject_body(invalid[i]);
  char raw_nul[]="{\"ssid\":\"x!suffix\",\"password\":\"fixture-router-password\"}";
  *strchr(raw_nul,'!')=0;
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=raw_nul,
    .body_size=sizeof(raw_nul)-1,.authorization=fixture_authorization},400,0,0);cJSON_Delete(o);
  /* Escaped backslash followed by u0000 is literal user input, not NUL. */
  o=request((struct request_s){.method="POST",.path="/api/network/connect",
    .body="{\"ssid\":\"Fake\\\\u0000router\",\"password\":\"fixture-router-password\"}",
    .authorization=fixture_authorization},200,1,0);
  assert(!strcmp(submitted_ssid,"Fake\\u0000router"));cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.omit_length=true},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.force_length=true,.announced_size=8193},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.extra="Content-Length: 1\r\n"},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.extra="Transfer-Encoding: chunked\r\n"},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="GET",.path="/api/network?path=ignored",
    .authorization=fixture_authorization},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="POST",.path="/api/network/connect?path=ignored",.body=valid,
    .authorization=fixture_authorization},400,0,0);cJSON_Delete(o);
  o=request((struct request_s){.method="GET",.path="/api/network/connect",
    .authorization=fixture_authorization},400,0,0);cJSON_Delete(o);
  unsigned before_accepted=accepted;fixture_submit_error=-EBUSY;
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization},400,1,0);
  assert(cJSON_GetObjectItemCaseSensitive(o,"code")->valueint==EBUSY);cJSON_Delete(o);
  assert(accepted==before_accepted);fixture_submit_error=-EINVAL;
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization},400,1,0);
  assert(cJSON_GetObjectItemCaseSensitive(o,"code")->valueint==EINVAL);cJSON_Delete(o);
  assert(accepted==before_accepted);fixture_submit_error=0;
  o=request((struct request_s){.method="POST",.path="/api/network/connect",.body=valid,
    .authorization=fixture_authorization,.revoke_in_body=true},400,0,0);cJSON_Delete(o);
  assert(accepted==before_accepted);close(fixture_listener);
  printf("PORTAL_NETWORK_HTTP_OK: %u real TCP routes; auth/Host/Origin; JSON/NUL rejection; literal escapes; single accepted action; EBUSY/error; no PSK access\n",cases);
  return 0;
}
