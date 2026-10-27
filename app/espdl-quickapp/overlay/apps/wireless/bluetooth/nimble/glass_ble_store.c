/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble_store.h"
#include "glass_ble_candidate.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "store/ram/ble_store_ram.h"

/* This NimBLE port exports the RAM-store initializer without declaring it. */
extern void ble_store_ram_init(void);

#define STORE_MAGIC 0x4d534c42u
#define STORE_VERSION 1u
#define STORE_BONDS 3
#define STORE_CCCDS 8
#define STORE_PATH GLASS_BLE_CANDIDATE_STORE_PATH
#define STORE_TEMP GLASS_BLE_CANDIDATE_STORE_TEMP
#define HID_PATH GLASS_BLE_CANDIDATE_PURPOSE_PATH
#define HID_TEMP GLASS_BLE_CANDIDATE_PURPOSE_TEMP
#define HID_MAGIC 0x48424c45u
#define HID_PEERS (STORE_BONDS * 2)

struct persistent_store
{
  uint32_t magic;
  uint16_t version;
  uint16_t sec_size;
  uint16_t cccd_size;
  uint8_t our_count;
  uint8_t peer_count;
  uint8_t cccd_count;
  uint8_t reserved;
  struct ble_store_value_sec our[STORE_BONDS];
  struct ble_store_value_sec peer[STORE_BONDS];
  struct ble_store_value_cccd cccd[STORE_CCCDS];
  uint32_t checksum;
};

static struct persistent_store g_store;
static pthread_mutex_t g_store_lock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int g_store_error;

struct hid_peer
{
  ble_addr_t peer;
  uint8_t present;
  uint8_t our_ltk[16];
  uint8_t peer_ltk[16];
};

struct hid_store
{
  uint32_t magic;
  uint16_t version;
  uint8_t count;
  uint8_t reserved;
  struct hid_peer peers[HID_PEERS];
  uint32_t checksum;
};

/* Runtime heap, not the constrained internal startup BSS. The single BLE
 * host owns this for its lifetime. Keys are never copied into diagnostics. */

static struct hid_store *g_hid_store;

static uint32_t store_checksum(const void *data, size_t length)
{
  const uint8_t *bytes = data;
  uint32_t value = 2166136261u;
  for (size_t i = 0; i < length; i++)
    value = (value ^ bytes[i]) * 16777619u;
  return value;
}

static void store_reset(void)
{
  memset(&g_store, 0, sizeof(g_store));
  g_store.magic = STORE_MAGIC;
  g_store.version = STORE_VERSION;
  g_store.sec_size = sizeof(struct ble_store_value_sec);
  g_store.cccd_size = sizeof(struct ble_store_value_cccd);
}

static bool store_valid(void)
{
  return g_store.magic == STORE_MAGIC &&
         g_store.version == STORE_VERSION &&
         g_store.sec_size == sizeof(struct ble_store_value_sec) &&
         g_store.cccd_size == sizeof(struct ble_store_value_cccd) &&
         g_store.our_count <= STORE_BONDS &&
         g_store.peer_count <= STORE_BONDS &&
         g_store.cccd_count <= STORE_CCCDS &&
         g_store.checksum == store_checksum(&g_store,
                                            offsetof(struct persistent_store,
                                                     checksum));
}

static int save_blob(const char *path, const char *temp,
                     const void *data, size_t length)
{
  FILE *file;
  int ret = 0;
  if (mkdir("/data/config", 0700) < 0 && errno != EEXIST) return -errno;
  file = fopen(temp, "wb");
  if (!file) return -errno;
  if (fwrite(data, 1, length, file) != length ||
      fflush(file) != 0 || fsync(fileno(file)) != 0)
    ret = -(errno ? errno : EIO);
  if (fclose(file) != 0 && !ret) ret = -(errno ? errno : EIO);
  if (!ret && rename(temp, path) != 0) ret = -(errno ? errno : EIO);
  if (ret) (void)unlink(temp);
  return ret;
}

static int store_save(void)
{
  g_store.checksum = store_checksum(&g_store,
                                    offsetof(struct persistent_store, checksum));
  return save_blob(STORE_PATH, STORE_TEMP, &g_store, sizeof(g_store));
}

static int sec_index(const struct ble_store_value_sec *values, unsigned count,
                     const ble_addr_t *address)
{
  for (unsigned i = 0; i < count; i++)
    if (!ble_addr_cmp(&values[i].peer_addr, address)) return (int)i;
  return -1;
}

static int cccd_index(const struct ble_store_value_cccd *values, unsigned count,
                      const ble_addr_t *address, uint16_t handle)
{
  for (unsigned i = 0; i < count; i++)
    if (!ble_addr_cmp(&values[i].peer_addr, address) &&
        values[i].chr_val_handle == handle) return (int)i;
  return -1;
}

static void remove_value(void *values, size_t size, unsigned index,
                         uint8_t *count)
{
  if (index + 1 < *count)
    memmove((uint8_t *)values + index * size,
            (uint8_t *)values + (index + 1) * size,
            (*count - index - 1) * size);
  (*count)--;
}

static void value_key(int object, const union ble_store_value *value,
                      union ble_store_key *key)
{
  memset(key, 0, sizeof(*key));
  if (object == BLE_STORE_OBJ_TYPE_CCCD)
    {
      key->cccd.peer_addr = value->cccd.peer_addr;
      key->cccd.chr_val_handle = value->cccd.chr_val_handle;
    }
  else
    {
      key->sec.peer_addr = value->sec.peer_addr;
    }
}

/* Caller already holds NimBLE's host lock and our store lock. Undo only
 * this record; never re-run the SDK's sysinit-only RAM-store initializer. */

static int commit(const struct persistent_store *previous, int object,
                  const union ble_store_value *old, bool existed,
                  const union ble_store_key *key)
{
  int ret = store_save();
  if (ret)
    {
      int undo;
      g_store = *previous;
      undo = existed ? ble_store_ram_write(object, old) :
                       ble_store_ram_delete(object, key);
      atomic_store(&g_store_error, undo ? -EIO : ret);
      return BLE_HS_ESTORE_FAIL;
    }

  atomic_store(&g_store_error, 0);
  return 0;
}

static int store_write(int object, const union ble_store_value *value)
{
  struct persistent_store previous;
  union ble_store_value old;
  union ble_store_key key;
  struct ble_store_value_sec *values = NULL;
  uint8_t *count;
  int index;
  int ret;
  bool existed;

  if (object != BLE_STORE_OBJ_TYPE_OUR_SEC &&
      object != BLE_STORE_OBJ_TYPE_PEER_SEC &&
      object != BLE_STORE_OBJ_TYPE_CCCD)
    {
      return BLE_HS_ENOTSUP;
    }

  pthread_mutex_lock(&g_store_lock);
  if (atomic_load(&g_store_error))
    {
      pthread_mutex_unlock(&g_store_lock);
      return BLE_HS_ESTORE_FAIL;
    }
  if (object == BLE_STORE_OBJ_TYPE_CCCD)
    {
      count = &g_store.cccd_count;
      index = cccd_index(g_store.cccd, *count, &value->cccd.peer_addr,
                         value->cccd.chr_val_handle);
    }
  else
    {
      values = object == BLE_STORE_OBJ_TYPE_OUR_SEC ? g_store.our : g_store.peer;
      count = object == BLE_STORE_OBJ_TYPE_OUR_SEC ?
              &g_store.our_count : &g_store.peer_count;
      index = sec_index(values, *count, &value->sec.peer_addr);
    }

  if (index < 0 && *count >= (values ? STORE_BONDS : STORE_CCCDS))
    {
      pthread_mutex_unlock(&g_store_lock);
      return BLE_HS_ESTORE_CAP;
    }

  value_key(object, value, &key);
  ret = ble_store_ram_read(object, &key, &old);
  existed = ret == 0;
  if (ret && ret != BLE_HS_ENOENT)
    {
      pthread_mutex_unlock(&g_store_lock);
      return ret;
    }

  previous = g_store;
  ret = ble_store_ram_write(object, value);
  if (!ret)
    {
      if (index < 0)
        {
          index = (*count)++;
        }

      if (values)
        {
          values[index] = value->sec;
        }
      else
        {
          g_store.cccd[index] = value->cccd;
        }

      if (memcmp(&previous, &g_store, sizeof(g_store)) == 0)
        {
          atomic_store(&g_store_error, 0);
        }
      else
        {
          ret = commit(&previous, object, &old, existed, &key);
        }
    }

  pthread_mutex_unlock(&g_store_lock);
  return ret;
}

static int store_delete(int object, const union ble_store_key *key)
{
  struct persistent_store previous;
  union ble_store_value old;
  union ble_store_key exact;
  int ret;

  pthread_mutex_lock(&g_store_lock);
  if (atomic_load(&g_store_error))
    {
      pthread_mutex_unlock(&g_store_lock);
      return BLE_HS_ESTORE_FAIL;
    }
  ret = ble_store_ram_read(object, key, &old);
  if (ret)
    {
      pthread_mutex_unlock(&g_store_lock);
      return ret;
    }

  previous = g_store;
  value_key(object, &old, &exact);
  ret = ble_store_ram_delete(object, &exact);
  if (ret)
    {
      pthread_mutex_unlock(&g_store_lock);
      return ret;
    }
  if (object == BLE_STORE_OBJ_TYPE_OUR_SEC)
    {
      int index = sec_index(g_store.our, g_store.our_count, &old.sec.peer_addr);
      if (index >= 0) remove_value(g_store.our, sizeof(g_store.our[0]),
                                   (unsigned)index, &g_store.our_count);
    }
  else if (object == BLE_STORE_OBJ_TYPE_PEER_SEC)
    {
      int index = sec_index(g_store.peer, g_store.peer_count, &old.sec.peer_addr);
      if (index >= 0) remove_value(g_store.peer, sizeof(g_store.peer[0]),
                                   (unsigned)index, &g_store.peer_count);
    }
  else if (object == BLE_STORE_OBJ_TYPE_CCCD)
    {
      int index = cccd_index(g_store.cccd, g_store.cccd_count,
                             &old.cccd.peer_addr, old.cccd.chr_val_handle);
      if (index >= 0) remove_value(g_store.cccd, sizeof(g_store.cccd[0]),
                                   (unsigned)index, &g_store.cccd_count);
    }
  ret = commit(&previous, object, &old, true, &exact);
  pthread_mutex_unlock(&g_store_lock);
  return ret;
}

/* Bind purpose to the actual long-term keys, not an array slot. An old
 * firmware can reorder/delete bonds without assigning HID to a new key. */

static bool hid_keys_match(const struct hid_peer *peer)
{
  int index;

  if (!peer->present || (peer->present & ~3u)) return false;
  if (peer->present & 1u)
    {
      index = sec_index(g_store.our, g_store.our_count, &peer->peer);
      if (index < 0 || !g_store.our[index].ltk_present ||
          memcmp(g_store.our[index].ltk, peer->our_ltk, 16)) return false;
    }

  if (peer->present & 2u)
    {
      index = sec_index(g_store.peer, g_store.peer_count, &peer->peer);
      if (index < 0 || !g_store.peer[index].ltk_present ||
          memcmp(g_store.peer[index].ltk, peer->peer_ltk, 16)) return false;
    }

  return true;
}

static void hid_store_load(void)
{
  FILE *file;
  bool valid = false;

  free(g_hid_store);
  g_hid_store = calloc(1, sizeof(*g_hid_store));
  if (!g_hid_store)
    {
      atomic_store(&g_store_error, -ENOMEM);
      return;
    }

  file = fopen(HID_PATH, "rb");
  if (file)
    {
      valid = fread(g_hid_store, 1, sizeof(*g_hid_store), file) ==
              sizeof(*g_hid_store) && g_hid_store->magic == HID_MAGIC &&
              g_hid_store->version == 1 && g_hid_store->count <= HID_PEERS &&
              g_hid_store->checksum == store_checksum(g_hid_store,
                                         offsetof(struct hid_store, checksum));
      fclose(file);
    }

  if (!valid)
    {
      /* Unknown/corrupt purpose grants no automatic HID behavior. Keep the
       * legacy bond file untouched; an explicit HID connection can re-tag. */
      memset(g_hid_store, 0, sizeof(*g_hid_store));
      g_hid_store->magic = HID_MAGIC;
      g_hid_store->version = 1;
    }
}

void glass_ble_store_init(void)
{
  FILE *file;
  atomic_store(&g_store_error, 0);
  ble_store_ram_init();
  store_reset();
  file = fopen(STORE_PATH, "rb");
  if (file)
    {
      bool loaded = fread(&g_store, 1, sizeof(g_store), file) == sizeof(g_store);
      fclose(file);
      if (!loaded || !store_valid())
        {
          store_reset();
          atomic_store(&g_store_error, -EILSEQ);
        }
    }
  else if (errno != ENOENT) atomic_store(&g_store_error, -errno);
  for (unsigned i = 0; i < g_store.our_count; i++)
    {
      union ble_store_value value = {.sec = g_store.our[i]};
      if (ble_store_ram_write(BLE_STORE_OBJ_TYPE_OUR_SEC, &value))
        atomic_store(&g_store_error, -EIO);
    }
  for (unsigned i = 0; i < g_store.peer_count; i++)
    {
      union ble_store_value value = {.sec = g_store.peer[i]};
      if (ble_store_ram_write(BLE_STORE_OBJ_TYPE_PEER_SEC, &value))
        atomic_store(&g_store_error, -EIO);
    }
  for (unsigned i = 0; i < g_store.cccd_count; i++)
    {
      union ble_store_value value = {.cccd = g_store.cccd[i]};
      if (ble_store_ram_write(BLE_STORE_OBJ_TYPE_CCCD, &value))
        atomic_store(&g_store_error, -EIO);
    }
  ble_hs_cfg.store_read_cb = ble_store_ram_read;
  ble_hs_cfg.store_write_cb = store_write;
  ble_hs_cfg.store_delete_cb = store_delete;
  hid_store_load();
}

bool glass_ble_store_has_bond(void)
{
  bool found;
  pthread_mutex_lock(&g_store_lock);
  found = !atomic_load(&g_store_error) &&
           (g_store.our_count != 0 || g_store.peer_count != 0);
  pthread_mutex_unlock(&g_store_lock);
  return found;
}

bool glass_ble_store_has_hid_bond(void)
{
  bool found = false;
  pthread_mutex_lock(&g_store_lock);
  if (!atomic_load(&g_store_error) && g_hid_store)
    for (unsigned i = 0; i < g_hid_store->count && !found; i++)
      found = hid_keys_match(&g_hid_store->peers[i]);
  pthread_mutex_unlock(&g_store_lock);
  return found;
}

int glass_ble_store_mark_hid_peer(uint8_t type, const uint8_t address[6])
{
  ble_addr_t peer = {.type = type};
  struct hid_store previous;
  struct hid_peer tag = {0};
  int slot = -1;
  int our;
  int their;
  int ret = 0;

  if (!address)
    {
      return -EINVAL;
    }

  memcpy(peer.val, address, sizeof(peer.val));
  pthread_mutex_lock(&g_store_lock);
  ret = atomic_load(&g_store_error);
  if (ret || !g_hid_store)
    {
      pthread_mutex_unlock(&g_store_lock);
      return ret ? ret : -ENOMEM;
    }

  our = sec_index(g_store.our, g_store.our_count, &peer);
  their = sec_index(g_store.peer, g_store.peer_count, &peer);
  tag.peer = peer;
  if (our >= 0 && g_store.our[our].ltk_present)
    {
      tag.present |= 1u;
      memcpy(tag.our_ltk, g_store.our[our].ltk, sizeof(tag.our_ltk));
    }

  if (their >= 0 && g_store.peer[their].ltk_present)
    {
      tag.present |= 2u;
      memcpy(tag.peer_ltk, g_store.peer[their].ltk, sizeof(tag.peer_ltk));
    }

  if (!tag.present)
    {
      pthread_mutex_unlock(&g_store_lock);
      return -ENOENT;
    }

  for (unsigned i = 0; i < g_hid_store->count; i++)
    if (!ble_addr_cmp(&g_hid_store->peers[i].peer, &peer)) slot = (int)i;
  if (slot < 0)
    for (unsigned i = 0; i < g_hid_store->count && slot < 0; i++)
      if (!hid_keys_match(&g_hid_store->peers[i])) slot = (int)i;
  if (slot < 0) slot = g_hid_store->count;
  if (slot >= HID_PEERS)
    {
      pthread_mutex_unlock(&g_store_lock);
      return -ENOSPC;
    }

  if (memcmp(&g_hid_store->peers[slot], &tag, sizeof(tag)))
    {
      previous = *g_hid_store;
      g_hid_store->peers[slot] = tag;
      if ((unsigned)slot == g_hid_store->count) g_hid_store->count++;
      g_hid_store->checksum = store_checksum(g_hid_store,
                                           offsetof(struct hid_store, checksum));
      ret = save_blob(HID_PATH, HID_TEMP, g_hid_store, sizeof(*g_hid_store));
      if (ret)
        {
          *g_hid_store = previous;
        }
      atomic_store(&g_store_error, ret);
    }

  pthread_mutex_unlock(&g_store_lock);
  return ret;
}

int glass_ble_store_last_error(void)
{
  return atomic_load(&g_store_error);
}
