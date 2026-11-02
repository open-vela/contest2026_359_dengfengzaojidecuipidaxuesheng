/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "qpk_volume.h"
static int mounted,open_error,close_error,opens,reads,closes;
static ssize_t read_size=512;
static int mount_fixture(enum qpk_volume volume)
{assert(volume==QPK_VOLUME_SD);return mounted;}
static int open_fixture(const char *path,int flags,...)
{assert(!strcmp(path,"/dev/mmcsd0"));assert(flags==O_RDONLY);opens++;return open_error?-1:41;}
static ssize_t read_fixture(int fd,void *data,size_t bytes)
{assert(fd==41&&bytes==512);reads++;if(read_size>0)memset(data,0x5a,read_size);return read_size;}
static int close_fixture(int fd){assert(fd==41);closes++;return close_error?-1:0;}
static int creates,create_error=EAGAIN;
/* Linux has a larger PTHREAD_STACK_MIN; creation is a fixture, too. */
static int stack_fixture(pthread_attr_t *attr,size_t bytes)
{(void)attr;assert(bytes==8192);return 0;}
static int create_fixture(pthread_t *thread,const pthread_attr_t *attr,
                          void *(*entry)(void *),void *arg)
{(void)thread;(void)attr;assert(entry&&arg==NULL);creates++;return create_error;}
#define qpk_volume_mounted mount_fixture
#define open open_fixture
#define read read_fixture
#define close close_fixture
#define pthread_create create_fixture
#define pthread_attr_setstacksize stack_fixture
#include "../overlay/apps/system/desktop/glass_status_storage.c"
int main(void)
{
  unsigned char sector[512];
  assert(status_storage_probe(sector)&&opens==1&&reads==1&&closes==1);
  mounted=-ENODEV;assert(!status_storage_probe(sector)&&opens==1);
  mounted=-EIO;assert(!status_storage_probe(sector)&&opens==1);
  mounted=0;open_error=1;assert(!status_storage_probe(sector)&&reads==1&&closes==1);
  open_error=0;
  const ssize_t failures[]={-1,0,1,511};
  for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);i++){
    read_size=failures[i];int closed_before=closes;
    assert(!status_storage_probe(sector)&&closes==closed_before+1);
  }
  read_size=512;close_error=1;assert(!status_storage_probe(sector));
  close_error=0;assert(status_storage_probe(sector));
  int read_before=reads;
  for(unsigned i=0;i<1000;i++)assert(!glass_status_storage_ready());
  assert(reads==read_before); /* GUI snapshot never probes the card. */
  assert(glass_status_storage_start()==EAGAIN&&!g_storage_started&&creates==1);
  assert(glass_status_storage_start()==EAGAIN&&!g_storage_started&&creates==2);
  create_error=0;assert(!glass_status_storage_start()&&g_storage_started&&creates==3);
  assert(!glass_status_storage_start()&&creates==3);
  puts("STATUS_STORAGE_OK: readonly, absent, I/O failure, short read, cleanup, atomic snapshot, retry and single worker");
  return 0;
}
