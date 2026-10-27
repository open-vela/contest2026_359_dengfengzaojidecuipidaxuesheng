/* SPDX-License-Identifier: Apache-2.0 */
/* Isolated artificial keys only. No device configuration is read. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "host/ble_hs.h"

int sysinit_active, sysinit_calls;
struct hs_config ble_hs_cfg;
#include "sdk_keys.inc"
#include SDK_RAM_FILE

static char test_directory[128];
static int fault, writes;
enum {OPEN=1, WRITE, FLUSH, SYNC, CLOSE, RENAME};

static const char *mapped(const char *path, char buffer[256])
{
  const char *base = strrchr(path, '/');
  assert(base && !strncmp(path, "/data/config", 12));
  assert(snprintf(buffer, 256, "%s/%s", test_directory, base + 1) < 256);
  return buffer;
}

static FILE *test_open(const char *path, const char *mode)
{
  char buffer[256];
  if (mode[0] == 'w')
    {
      writes++;
      if (fault == OPEN) {errno=ENOSPC;return NULL;}
    }
  return fopen(mapped(path, buffer), mode);
}

static size_t test_write(const void *p,size_t size,size_t count,FILE *f)
{if(fault==WRITE){errno=ENOSPC;return 0;}return fwrite(p,size,count,f);}
static int test_flush(FILE *f)
{if(fault==FLUSH){errno=EIO;return EOF;}return fflush(f);}
static int test_sync(int fd)
{if(fault==SYNC){errno=EIO;return -1;}return fsync(fd);}
static int test_close(FILE *f)
{int r=fclose(f);if(fault==CLOSE){errno=EIO;return EOF;}return r;}
static int test_rename(const char *from,const char *to)
{char a[256],b[256];if(fault==RENAME){errno=EIO;return -1;}return rename(mapped(from,a),mapped(to,b));}
static int test_unlink(const char *path)
{char buffer[256];return unlink(mapped(path,buffer));}
static int test_mkdir(const char *path,mode_t mode)
{(void)mode;assert(!strcmp(path,"/data/config"));errno=EEXIST;return -1;}

#define fopen test_open
#define fwrite test_write
#define fflush test_flush
#define fsync test_sync
#define fclose test_close
#define rename test_rename
#define unlink test_unlink
#define mkdir test_mkdir
#include "glass_ble_store.c"
#undef fopen
#undef fwrite
#undef fflush
#undef fsync
#undef fclose
#undef rename
#undef unlink
#undef mkdir

static void boot(void)
{assert(!fault);sysinit_active=1;glass_ble_store_init();sysinit_active=0;}

static union ble_store_value sec(unsigned id,unsigned key)
{
  union ble_store_value v={0};
  v.sec.peer_addr.type=1;v.sec.peer_addr.val[0]=id;
  v.sec.key_size=16;v.sec.ltk_present=1;
  memset(v.sec.ltk,key,sizeof(v.sec.ltk));
  return v;
}

static union ble_store_value cccd(unsigned id,unsigned handle)
{
  union ble_store_value v={0};
  v.cccd.peer_addr.type=1;v.cccd.peer_addr.val[0]=id;
  v.cccd.chr_val_handle=handle;v.cccd.flags=1;
  return v;
}

static union ble_store_key key(int object,const union ble_store_value *v)
{union ble_store_key k;value_key(object,v,&k);return k;}

static void reset(void)
{
  fault=0;test_unlink(STORE_PATH);test_unlink(HID_PATH);
  test_unlink(STORE_TEMP);test_unlink(HID_TEMP);boot();
  assert(!glass_ble_store_last_error()&&!glass_ble_store_has_hid_bond());
}

static void same_ram(int object,const union ble_store_value *want)
{
  union ble_store_key k=key(object,want);union ble_store_value got={0};
  assert(!ble_store_ram_read(object,&k,&got));
  size_t n=object==BLE_STORE_OBJ_TYPE_CCCD?sizeof(got.cccd):sizeof(got.sec);
  assert(!memcmp(&got,want,n));
}

static void compaction(void)
{
  for(int object=1;object<=3;object++)
    {
      reset();union ble_store_value a=object==3?cccd(1,10):sec(1,11);
      union ble_store_value b=object==3?cccd(2,20):sec(2,22);
      union ble_store_value c=object==3?cccd(3,30):sec(3,33);
      assert(!store_write(object,&a)&&!store_write(object,&b)&&!store_write(object,&c));
      union ble_store_key k=key(object,&a);assert(!store_delete(object,&k));
      same_ram(object,&b);same_ram(object,&c);
      k=key(object,&b);assert(!store_delete(object,&k));same_ram(object,&c);
    }
}

static void save_failures(void)
{
  for(int object=1;object<=3;object++)for(int f=OPEN;f<=RENAME;f++)
    for(int operation=0;operation<3;operation++)
      {
        reset();union ble_store_value a=object==3?cccd(1,10):sec(1,11);
        union ble_store_value b=object==3?cccd(2,20):sec(2,22);
        union ble_store_value c=object==3?cccd(3,30):sec(3,33);
        assert(!store_write(object,&a)&&!store_write(object,&b));
        struct persistent_store before=g_store,disk;
        int boots=sysinit_calls;fault=f;
        int ret;
        if(operation==0)ret=store_write(object,&c);
        else if(operation==1)
          {
            union ble_store_value changed=a;
            if(object==3)changed.cccd.flags=2;else changed.sec.ltk[4]++;
            ret=store_write(object,&changed);
          }
        else {union ble_store_key k=key(object,&a);ret=store_delete(object,&k);}
        assert(ret==BLE_HS_ESTORE_FAIL&&glass_ble_store_last_error()<0);
        assert(sysinit_calls==boots&&!memcmp(&before,&g_store,sizeof(before)));
        same_ram(object,&a);same_ram(object,&b);
        union ble_store_key k=key(object,&c);union ble_store_value v;
        assert(ble_store_ram_read(object,&k,&v)==BLE_HS_ENOENT);
        fault=0;FILE *fp=test_open(STORE_PATH,"rb");assert(fp);
        assert(fread(&disk,1,sizeof(disk),fp)==sizeof(disk));fclose(fp);
        assert(!memcmp(&disk,&before,sizeof(disk)));
        int n=writes;assert(store_write(object,&a)==BLE_HS_ESTORE_FAIL&&writes==n);
        boot();assert(!glass_ble_store_last_error());same_ram(object,&a);same_ram(object,&b);
      }
}

static void purposes(void)
{
  reset();union ble_store_value a=sec(1,11),b=sec(2,22);
  assert(!store_write(1,&a)&&!store_write(1,&b));
  struct persistent_store legacy=g_store;
  boot();assert(glass_ble_store_has_bond()&&!glass_ble_store_has_hid_bond());
  assert(!memcmp(&legacy,&g_store,sizeof(legacy)));
  assert(glass_ble_store_mark_hid_peer(1,(uint8_t[6]){99})==-ENOENT);
  assert(!glass_ble_store_mark_hid_peer(1,b.sec.peer_addr.val));
  assert(glass_ble_store_has_hid_bond());int n=writes;
  assert(!glass_ble_store_mark_hid_peer(1,b.sec.peer_addr.val)&&writes==n);
  boot();assert(glass_ble_store_has_hid_bond());
  /* A legacy firmware can reorder slots without changing key ownership. */
  struct ble_store_value_sec swap=g_store.our[0];
  g_store.our[0]=g_store.our[1];g_store.our[1]=swap;assert(!store_save());
  boot();assert(glass_ble_store_has_hid_bond());
  b.sec.ltk[7]++;assert(!store_write(1,&b));
  assert(!glass_ble_store_has_hid_bond());
  assert(!glass_ble_store_mark_hid_peer(1,b.sec.peer_addr.val));
  union ble_store_key k=key(1,&b);assert(!store_delete(1,&k));
  assert(!glass_ble_store_has_hid_bond());
  for(int f=OPEN;f<=RENAME;f++)
    {
      reset();assert(!store_write(1,&a));struct persistent_store before=g_store;
      fault=f;assert(glass_ble_store_mark_hid_peer(1,a.sec.peer_addr.val)<0);
      assert(!glass_ble_store_has_hid_bond()&&glass_ble_store_last_error()<0);
      assert(!memcmp(&before,&g_store,sizeof(before)));fault=0;
      boot();assert(!glass_ble_store_last_error()&&!glass_ble_store_has_hid_bond());
    }
  /* No key is promoted by corrupted purpose metadata. */
  FILE *fp=test_open(HID_PATH,"wb");assert(fp);fputs("invalid",fp);fclose(fp);
  boot();assert(!glass_ble_store_has_hid_bond()&&glass_ble_store_has_bond());
}

int main(int argc,char **argv)
{
  (void)argv;strcpy(test_directory,"/tmp/glass-ble-store-XXXXXX");
  assert(mkdtemp(test_directory));compaction();
  if(argc==1)
    {
      save_failures();purposes();reset();
      union ble_store_value a=sec(1,11),b=sec(2,22),c=sec(3,33),d=sec(4,44);
      assert(!store_write(1,&a)&&!store_write(1,&b)&&!store_write(1,&c));
      int n=writes;assert(store_write(1,&d)==BLE_HS_ESTORE_CAP&&writes==n);
      assert(!glass_ble_store_last_error());same_ram(1,&a);same_ram(1,&b);same_ram(1,&c);
      assert(!store_write(1,&a)&&writes==n);
      union ble_store_key k={0};assert(store_write(99,&a)==BLE_HS_ENOTSUP);
      assert(store_delete(99,&k)==BLE_HS_ENOTSUP);
      FILE *fp=test_open(STORE_PATH,"wb");assert(fp);fputs("broken",fp);fclose(fp);
      boot();assert(glass_ble_store_last_error()==-EILSEQ&&!glass_ble_store_has_bond());
      n=writes;assert(store_write(1,&a)==BLE_HS_ESTORE_FAIL&&writes==n);
      puts("BLE_STORE_OK: actual SDK RAM store; 54 insert/update/delete filesystem faults; RAM/disk rollback; sticky errors; legacy keys preserved; purpose bound to LTK, reorder/delete/renewal; no-op writes; capacity; corrupt file preserved");
    }
  reset();free(g_hid_store);g_hid_store=NULL;assert(!rmdir(test_directory));
  return 0;
}
