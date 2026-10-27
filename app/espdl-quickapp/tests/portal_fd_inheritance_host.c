/* SPDX-License-Identifier: Apache-2.0 */
/* Run the actual portal worker and NuttX fdlist_copy against native TCP FDs.
 * Only the file-table primitives are replaced by native dup/close adapters. */
#define main portal_route_fixture_main
#include "portal_network_host.c"
#undef main

#define FAR
#define OK 0
#define CONFIG_NFILE_DESCRIPTORS_PER_BLOCK 8
#define CLONE_FDS 256
typedef void posix_spawn_file_actions_t;
struct file { int native_fd; unsigned refs; };
struct fd { bool f_cloexec; };
struct fdlist
{
  int fl_rows;
  struct file files[CLONE_FDS];
  struct fd flags[CLONE_FDS];
  bool present[CLONE_FDS];
};
static void fdlist_get_by_index(struct fdlist *list,int row,int column,
                                struct file **file,struct fd **flags)
{
  int index=row*CONFIG_NFILE_DESCRIPTORS_PER_BLOCK+column;
  assert(index<CLONE_FDS);
  *file=list->present[index]?&list->files[index]:NULL;
  *flags=&list->flags[index];
  if(*file)(*file)->refs++;
}
static void file_put(struct file *file)
{assert(file->refs>0);file->refs--;}
static int fdlist_extend(struct fdlist *list,int rows)
{assert(rows*CONFIG_NFILE_DESCRIPTORS_PER_BLOCK<=CLONE_FDS);list->fl_rows=rows;return 0;}
static void fdlist_install(struct fdlist *list,int fd,struct file *file,
                           struct fd *flags,int ignored,bool copy)
{
  (void)flags;assert(!ignored&&copy&&fd<CLONE_FDS);
  int native=dup(file->native_fd);assert(native>=0);
  list->present[fd]=true;list->files[fd]=(struct file){native,1};
}
static bool spawn_file_is_duplicateable(const void *actions,int fd,bool cloexec)
{(void)actions;(void)fd;(void)cloexec;assert(false);return false;}
#include "target-fdlist-copy.inc"

static void parent_add(struct fdlist *parent,int fd)
{
  assert(fd>=0&&fd<CLONE_FDS);
  int flags=fcntl(fd,F_GETFD);assert(flags>=0);
  parent->present[fd]=true;
  parent->files[fd]=(struct file){fd,1};
  parent->flags[fd].f_cloexec=!!(flags&FD_CLOEXEC);
  int rows=fd/CONFIG_NFILE_DESCRIPTORS_PER_BLOCK+1;
  if(rows>parent->fl_rows)parent->fl_rows=rows;
}

int main(void)
{
  /* The real worker executes the production accept4 path, rather than a
   * test-only accept wrapper. No request is sent, so it keeps the client. */
  fixture_port=0;
  int listen_fd=open_listener();assert(listen_fd>=0);
  struct sockaddr_in address;socklen_t length=sizeof(address);
  assert(getsockname(listen_fd,(struct sockaddr *)&address,&length)==0);
  fixture_port=ntohs(address.sin_port);assert(fixture_port);
  address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  atomic_store(&listener,listen_fd);
  struct pending_client pending[PENDING_CLIENTS]={0};
  for(unsigned i=0;i<PENDING_CLIENTS;i++)pending[i].fd=-1;
  pthread_t thread;assert(pthread_create(&thread,NULL,worker,pending)==0);
  int browser=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0);assert(browser>=0);
  assert(connect(browser,(struct sockaddr *)&address,sizeof(address))==0);
  for(unsigned i=0;i<200&&!atomic_load(&portal_accepts);i++)usleep(10000);
  assert(atomic_load(&portal_accepts)==1);
  assert(pthread_cancel(thread)==0&&pthread_join(thread,NULL)==0);
  int client_fd=-1;
  for(unsigned i=0;i<PENDING_CLIENTS;i++)if(pending[i].fd>=0)client_fd=pending[i].fd;
  assert(client_fd>=0);

  int marker[2];assert(pipe(marker)==0);
  assert(fcntl(marker[1],F_SETFD,FD_CLOEXEC)==0);
  struct fdlist parent={0},child={0};
  parent_add(&parent,listen_fd);parent_add(&parent,client_fd);
  parent_add(&parent,marker[0]);parent_add(&parent,marker[1]);
  /* task_setup uses cloexec=true, unlike fork. Execute its actual kernel
   * cloning function, extracted unchanged from the supplied target tree. */
  assert(fdlist_copy(&parent,&child,NULL,true)==0);
  assert(child.present[marker[0]]&&!child.present[marker[1]]);
  close(listen_fd);atomic_store(&listener,-1);
  int reopened=open_listener();
  if(reopened<0)fprintf(stderr,"INHERITED_LISTENER_REPRODUCED: bind error %d while child retains descriptor\n",-reopened);
  assert(reopened>=0);close(reopened);
  assert(!child.present[listen_fd]);
  close(client_fd);
  struct pollfd event={.fd=browser,.events=POLLIN};
  int ready=poll(&event,1,250);
  if(!ready)fprintf(stderr,"INHERITED_CLIENT_REPRODUCED: child prevents peer EOF\n");
  assert(ready>0&&(event.revents&(POLLIN|POLLHUP)));
  char byte;assert(recv(browser,&byte,1,0)==0);
  assert(!child.present[client_fd]);close(browser);
  close(marker[0]);close(marker[1]);
  for(unsigned i=0;i<CLONE_FDS;i++)if(child.present[i])close(child.files[i].native_fd);
  printf("PORTAL_FD_INHERITANCE_OK: real worker; target fdlist_copy; listener rebind; client EOF; unrelated FD inheritance preserved\n");
  return 0;
}
