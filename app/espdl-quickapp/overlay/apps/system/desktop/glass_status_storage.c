/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include "glass_status.h"
#include "qpk_volume.h"

static bool g_storage_ready;
static uint32_t g_storage_epoch;

bool glass_status_storage_ready(void)
{ return __atomic_load_n(&g_storage_ready,__ATOMIC_ACQUIRE); }
uint32_t glass_status_storage_epoch(void)
{ return __atomic_load_n(&g_storage_epoch,__ATOMIC_ACQUIRE); }

#ifdef CONFIG_ESP32P4_SDMMC
static bool g_storage_started;
#ifndef GLASS_STATUS_SD_DEVICE
#define GLASS_STATUS_SD_DEVICE "/dev/mmcsd0"
#endif

/* This board has no configured card-detect pin. Mounted filesystem metadata
 * can survive removal, so verify one sector through a fresh read-only block
 * proxy as well. Closing each probe avoids its single-sector cache. Never
 * mount, reprobe, eject or write media here. All I/O stays off the GUI thread. */
static bool status_storage_probe(void *sector)
{
  if(qpk_volume_mounted(QPK_VOLUME_SD)!=0)return false;
  int fd=open(GLASS_STATUS_SD_DEVICE,O_RDONLY);
  if(fd<0)return false;
  ssize_t count=read(fd,sector,512);
  int closed=close(fd);
  return count==512&&closed==0;
}

static void *status_storage_worker(void *unused)
{
  (void)unused;
  void *sector=NULL;
  if(posix_memalign(&sector,64,512)!=0){
    __atomic_store_n(&g_storage_started,false,__ATOMIC_RELEASE);
    return NULL;
  }
  for(;;){
    bool ready=status_storage_probe(sector);
    bool previous=__atomic_exchange_n(&g_storage_ready,ready,__ATOMIC_ACQ_REL);
    if(previous!=ready)__atomic_add_fetch(&g_storage_epoch,1,__ATOMIC_RELEASE);
    sleep(2);
  }
  return NULL;
}
#endif

int glass_status_storage_start(void)
{
#ifdef CONFIG_ESP32P4_SDMMC
  bool expected=false;
  if(!__atomic_compare_exchange_n(&g_storage_started,&expected,true,false,
                                 __ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE))return 0;
  pthread_attr_t attr;
  int ret=pthread_attr_init(&attr);
  if(!ret){
    ret=pthread_attr_setstacksize(&attr,8192);
    if(!ret)ret=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);
    if(!ret){pthread_t thread;ret=pthread_create(&thread,&attr,status_storage_worker,NULL);}
    pthread_attr_destroy(&attr);
  }
  if(ret)__atomic_store_n(&g_storage_started,false,__ATOMIC_RELEASE);
  return ret;
#else
  return 0;
#endif
}
