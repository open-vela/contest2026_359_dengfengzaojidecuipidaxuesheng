/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <time.h>

static time_t test_seconds = 100;
static int portal_test_clock(clockid_t clock, struct timespec *value)
{
  (void)clock;
  value->tv_sec = test_seconds;
  value->tv_nsec = 0;
  return 0;
}
#define clock_gettime portal_test_clock
#include "../overlay/apps/system/desktop/glass_portal_http.c"
#undef clock_gettime

const unsigned char *portal_asset(const char *path,size_t *size,const char **type)
{ (void)path; (void)size; (void)type; return NULL; }
cJSON *portal_config_get(const char *section)
{ (void)section; return cJSON_CreateObject(); }
int portal_config_save(const char *section,const cJSON *patch)
{ (void)section; (void)patch; return 0; }
int portal_public_path(const char *path,char *out,size_t capacity)
{ (void)path; (void)out; (void)capacity; return -EINVAL; }
int portal_install_target(const char *path,char *out,size_t capacity)
{ (void)path; (void)out; (void)capacity; return -EINVAL; }
int portal_install_begin(const cJSON *manifest,char *error,size_t capacity)
{ (void)manifest; (void)error; (void)capacity; return -EINVAL; }
int portal_install_commit(void) { return -EINVAL; }
void portal_install_abort(void) {}

static void session_request(const char *bearer,int status)
{
  int server = socket(AF_INET,SOCK_STREAM,0);
  assert(server >= 0);
  struct sockaddr_in address = {.sin_family=AF_INET,
    .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
  assert(bind(server,(struct sockaddr *)&address,sizeof(address)) == 0);
  assert(listen(server,1) == 0);
  socklen_t length = sizeof(address);
  assert(getsockname(server,(struct sockaddr *)&address,&length) == 0);
  int browser = socket(AF_INET,SOCK_STREAM,0);
  assert(browser >= 0);
  assert(connect(browser,(struct sockaddr *)&address,sizeof(address)) == 0);
  int device = accept(server,NULL,NULL);
  assert(device >= 0);
  char headers[512];
  snprintf(headers,sizeof(headers),
    "GET /api/session HTTP/1.1\r\nHost: 127.0.0.1:8080\r\nAuthorization: Bearer %s\r\n\r\n",bearer);
  io_failed = false;
  io_deadline = milliseconds()+REQUEST_TIMEOUT_MS;
  client(device,headers);
  close(device);
  char reply[1024];
  size_t used = 0;
  ssize_t count;
  while((count=recv(browser,reply+used,sizeof(reply)-1-used,0))>0)
    used+=(size_t)count;
  assert(count == 0);
  reply[used] = 0;
  char expected[32];
  snprintf(expected,sizeof(expected),"HTTP/1.1 %d",status);
  assert(strncmp(reply,expected,strlen(expected)) == 0);
  if(status == 200) assert(strstr(reply,"\"idle_seconds\":900"));
  assert(!strstr(reply,bearer));
  close(browser);
  close(server);
}

int main(void)
{
  const char *bearer = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  strcpy(session_token,bearer);
  session_expires = test_seconds+SESSION_IDLE_SECONDS;
  for(unsigned minute=0;minute<20;minute++)
    {
      test_seconds+=60;
      session_request(bearer,200);
      assert(session_expires == test_seconds+SESSION_IDLE_SECONDS);
    }
  session_request("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",401);
  test_seconds+=SESSION_IDLE_SECONDS+1;
  session_request(bearer,401);
  session_expires = test_seconds+SESSION_IDLE_SECONDS;
  portal_session_close();
  session_request(bearer,401);
  puts("Portal session HTTP tests passed");
  return 0;
}
