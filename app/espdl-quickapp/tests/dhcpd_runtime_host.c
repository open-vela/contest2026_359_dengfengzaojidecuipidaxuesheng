/* SPDX-License-Identifier: Apache-2.0 */

#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

enum fault_e
{
  F_NONE, F_TASK, F_ALLOC, F_SIGNAL, F_SOCKET, F_TIMEOUT, F_IOCTL, F_BIND,
  F_KILL, F_WAIT
};

static atomic_int fault;
static atomic_int sockets;
static atomic_int bind_calls;
static atomic_int recv_calls;
static atomic_int children;
static atomic_bool hold_bind;
static atomic_bool bind_entered;
static atomic_bool stop_at_recv;
static unsigned sent_packets;

static ssize_t test_sendto(int fd,const void *data,size_t size,int flags,
                           const struct sockaddr *to,socklen_t length)
{
  const struct sockaddr_in *address=(const struct sockaddr_in *)to;
  assert(fd==99&&data&&size&&!flags&&length==sizeof(*address));
  assert(ntohs(address->sin_port)==68);
  sent_packets++;
  return size;
}
static pthread_t child;
static int (*child_entry)(int, char **);
static char child_interface[IFNAMSIZ];
static _Thread_local pid_t test_pid = 10;

static bool test_should_stop(void);
static void test_request_stop(void);

static void *child_main(void *arg)
{
  (void)arg;
  test_pid = 42;
  char *argv[] = {"dhcpd", child_interface, NULL};
  child_entry(2, argv);
  return NULL;
}

static int test_task_create(const char *name, int priority, int stack,
                            int (*entry)(int, char **), char *argv[])
{
  assert(!strcmp(name, "dhcpd") && priority > 0 && stack > 0);
  if (atomic_load(&fault) == F_TASK) return errno = EAGAIN, -1;
  assert(!atomic_load(&children));
  snprintf(child_interface, sizeof(child_interface), "%s", argv[0]);
  child_entry = entry;
  atomic_fetch_add(&children, 1);
  assert(!pthread_create(&child, NULL, child_main, NULL));
  return 42;
}

static void *test_malloc(size_t n)
{
  if (atomic_load(&fault) == F_ALLOC) return NULL;
  return malloc(n);
}

static int test_sigaction(int signo, const struct sigaction *act,
                          struct sigaction *old)
{
  assert(signo == SIGUSR1 && act && !old && act->sa_sigaction);
  if (atomic_load(&fault) == F_SIGNAL) return errno = EACCES, -1;
  return 0;
}

static int test_socket(int domain, int type, int protocol)
{
  assert(domain == PF_INET && (type & SOCK_DGRAM) && !protocol);
  if (atomic_load(&fault) == F_SOCKET) return errno = EMFILE, -1;
  atomic_fetch_add(&sockets, 1);
  return 99;
}

static int test_setsockopt(int fd, int level, int option,
                           const void *value, socklen_t length)
{
  assert(fd == 99 && level == SOL_SOCKET);
  if (option == SO_RCVTIMEO)
    {
      const struct timeval *timeout = value;
      assert(length == sizeof(*timeout) && timeout->tv_sec == 1 &&
             !timeout->tv_usec);
      if (atomic_load(&fault) == F_TIMEOUT) return errno = ENOPROTOOPT, -1;
    }
  return 0;
}

static int test_ioctl(int fd, unsigned long request, ...)
{
  va_list args;
  assert(fd == 99 && request == SIOCGIFADDR);
  if (atomic_load(&fault) == F_IOCTL) return errno = ENODEV, -1;
  va_start(args, request);
  struct ifreq *req = (struct ifreq *)va_arg(args, unsigned long);
  va_end(args);
  assert(!strcmp(req->ifr_name, "ap0"));
  ((struct sockaddr_in *)&req->ifr_addr)->sin_addr.s_addr = 0x0104a8c0;
  return 0;
}

static int test_bind(int fd, const struct sockaddr *address, socklen_t length)
{
  const struct sockaddr_in *addr = (const struct sockaddr_in *)address;
  assert(fd == 99 && length == sizeof(*addr));
  assert(addr->sin_family == AF_INET && ntohs(addr->sin_port) == 67 &&
         addr->sin_addr.s_addr == INADDR_ANY);
  atomic_fetch_add(&bind_calls, 1);
  atomic_store(&bind_entered, true);
  while (atomic_load(&hold_bind)) usleep(1000);
  if (atomic_load(&fault) == F_BIND) return errno = EADDRINUSE, -1;
  return 0;
}

static ssize_t test_recv(int fd, void *buffer, size_t size, int flags)
{
  assert(fd == 99 && buffer && size && !flags);
  atomic_fetch_add(&recv_calls, 1);
  if (atomic_exchange(&stop_at_recv, false)) test_request_stop();
  /* Model SO_RCVTIMEO, including signal delivered before recv sleeps. */
  usleep(2000);
  errno = test_should_stop() ? EINTR : EAGAIN;
  return -1;
}

static int test_close(int fd)
{
  assert(fd == 99);
  assert(atomic_fetch_sub(&sockets, 1) == 1);
  errno = ENOSPC; /* Closing must not overwrite the recorded startup errno. */
  return 0;
}

static int test_kill(pid_t pid, int signo)
{
  assert(pid == 42 && signo == SIGUSR1);
  if (atomic_load(&fault) == F_KILL) return errno = EPERM, -1;
  test_request_stop();
  return 0;
}

static pid_t test_waitpid(pid_t pid, int *status, int options)
{
  assert(pid == 42 && !status && !options);
  if (atomic_load(&fault) == F_WAIT) return errno = ECHILD, -1;
  assert(!pthread_join(child, NULL));
  assert(atomic_fetch_sub(&children, 1) == 1);
  return pid;
}

static size_t test_strlcpy(char *to, const char *from, size_t capacity)
{
  size_t size = strlen(from);
  if (capacity) snprintf(to, capacity, "%s", from);
  return size;
}

#define task_create test_task_create
#define getpid() test_pid
#define malloc test_malloc
#define socket test_socket
#define setsockopt test_setsockopt
#define ioctl test_ioctl
#define bind test_bind
#define recv test_recv
#define sendto test_sendto
#define close test_close
#define kill test_kill
#define waitpid test_waitpid
#define sigaction(...) test_sigaction(__VA_ARGS__)
#define strlcpy test_strlcpy
#include "dhcpd.c"
#undef malloc

static bool test_should_stop(void)
{
  return g_dhcpd_daemon.ds_state == DHCPD_STOP_REQUESTED;
}

static void test_request_stop(void)
{
  dhcpd_signal_handler(SIGUSR1, NULL, NULL);
}

static struct dhcpd_runtime_status_s status(void)
{
  struct dhcpd_runtime_status_s value;
  assert(!dhcpd_runtime_status(&value));
  assert(!value.ready || value.running);
  return value;
}

static void await_ready(void)
{
  for (unsigned i = 0; i < 2000 && !status().ready; i++) usleep(1000);
  assert(status().ready && atomic_load(&recv_calls));
}

static void reset(void)
{
  atomic_store(&fault, F_NONE);
  atomic_store(&hold_bind, false);
  assert(!dhcpd_stop());
  assert(!atomic_load(&children) && !atomic_load(&sockets));
  assert(!status().running && !status().ready && status().pid == -1);
  atomic_store(&bind_calls, 0);
  atomic_store(&recv_calls, 0);
  atomic_store(&bind_entered, false);
}

static void *stop_main(void *arg)
{
  (void)arg;
  assert(!dhcpd_stop());
  return NULL;
}

int main(void)
{
#if defined(HAVE_ROUTERIP) && defined(HAVE_DNSIP)
  g_dhcpd_daemon.ds_data=calloc(1,sizeof(*g_dhcpd_daemon.ds_data));
  assert(g_dhcpd_daemon.ds_data);
  assert(!dhcpd_set_routerip(0xc0a82a01));
  for(unsigned iteration=0;iteration<2;iteration++)
    {
      uint32_t dns=iteration?0xdf050505:0xc0a81f01;
      assert(!dhcpd_set_dnsip(dns));
      (void)dhcpd_sendoffer(99,0xc0a82a02,120);
      const uint8_t *option=g_state.ds_outpacket.options+4;
      bool router_seen=false,dns_seen=false;
      while(*option!=DHCP_OPTION_END)
        {
          assert(option+2+option[1]<=g_state.ds_outpacket.options+sizeof(g_state.ds_outpacket.options));
          uint32_t value=0;if(option[1]==4)memcpy(&value,option+2,4);
          if(option[0]==DHCP_OPTION_ROUTER){assert(ntohl(value)==0xc0a82a01);router_seen=true;}
          if(option[0]==DHCP_OPTION_DNS_SERVER){assert(ntohl(value)==dns);dns_seen=true;}
          option+=2+option[1];
        }
      assert(router_seen&&dns_seen);
    }
  assert(sent_packets==2);
  free(g_dhcpd_daemon.ds_data);g_dhcpd_daemon.ds_data=NULL;
  puts("DHCPD_OPTIONS_OK: actual offers include AP gateway and updated upstream DNS");
#endif
  assert(dhcpd_runtime_status(NULL) == -EINVAL);
  assert(dhcpd_start(NULL) == -EINVAL);
  const int faults[] = {F_TASK, F_ALLOC, F_SIGNAL, F_SOCKET, F_TIMEOUT,
                        F_IOCTL, F_BIND};
  const int errors[] = {EAGAIN, ENOMEM, EACCES, EMFILE, ENOPROTOOPT,
                        ENODEV, EADDRINUSE};
  for (unsigned i = 0; i < sizeof(faults) / sizeof(faults[0]); i++)
    {
      reset();
      atomic_store(&fault, faults[i]);
      int ret = dhcpd_start("ap0");
      assert(ret == 0 || ret == -errors[i]);
      for (unsigned j = 0; j < 2000 && status().running; j++) usleep(1000);
      assert(!status().running && !status().ready);
      assert(status().last_error == -errors[i]);
      atomic_store(&fault, F_NONE);
      assert(!dhcpd_stop());
      assert(!atomic_load(&sockets) && !atomic_load(&children));
    }

  reset();
  atomic_store(&hold_bind, true);
  assert(!dhcpd_start("ap0"));
  for (unsigned i = 0; i < 2000 && !atomic_load(&bind_entered); i++) usleep(1000);
  assert(atomic_load(&bind_entered));
  assert(status().running && !status().ready && status().pid == 42);
  assert(dhcpd_start("ap0") == -EBUSY);
  assert(dhcpd_run("ap0") == -EBUSY);
  assert(atomic_load(&children) == 1);
  atomic_store(&hold_bind, false);
  await_ready();
  atomic_store(&fault, F_KILL);
  assert(dhcpd_stop() == -EPERM);
  assert(status().running && status().pid == 42);
  assert(atomic_load(&children) == 1 && atomic_load(&sockets) == 1);
  atomic_store(&fault, F_NONE);
  assert(!dhcpd_stop());
  reset();

  assert(!dhcpd_start("ap0"));
  await_ready();
  atomic_store(&fault, F_WAIT);
  assert(dhcpd_stop() == -ECHILD);
  assert(status().pid == 42 && atomic_load(&children) == 1);
  atomic_store(&fault, F_NONE);
  assert(!dhcpd_stop());
  reset();

  /* Stop during bind: no lost request and no false readiness after signal. */
  atomic_store(&hold_bind, true);
  assert(!dhcpd_start("ap0"));
  while (!atomic_load(&bind_entered)) usleep(1000);
  pthread_t stopper;
  assert(!pthread_create(&stopper, NULL, stop_main, NULL));
  while (!test_should_stop()) usleep(1000);
  assert(!status().ready);
  atomic_store(&hold_bind, false);
  assert(!pthread_join(stopper, NULL));
  reset();

  /* Signal immediately before recv starts: receive timeout permits exit. */
  atomic_store(&stop_at_recv, true);
  assert(!dhcpd_start("ap0"));
  for (unsigned i = 0; i < 2000 && status().running; i++) usleep(1000);
  assert(!status().running && !status().ready);
  assert(!dhcpd_stop());
  reset();
  for (unsigned i = 0; i < 30; i++)
    {
      assert(!dhcpd_start("ap0"));
      await_ready();
      assert(!dhcpd_stop());
      assert(!status().running && status().pid == -1);
    }
  puts("DHCPD_RUNTIME_OK: actual daemon; startup fault matrix; bind-only readiness; lifecycle exclusion; stop failure retains ownership; lost-signal window; 30 clean cycles");
  return 0;
}
