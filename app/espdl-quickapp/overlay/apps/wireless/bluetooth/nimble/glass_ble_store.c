/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_ble_store.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
#define STORE_PATH "/data/config/ble_mouse_store.bin"
#define STORE_TEMP "/data/config/.ble_mouse_store.tmp"

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

static int store_save(void)
{
  FILE *file;
  int ret = 0;
  if (mkdir("/data/config", 0700) < 0 && errno != EEXIST) return -errno;
  g_store.checksum = store_checksum(&g_store,
                                    offsetof(struct persistent_store, checksum));
  file = fopen(STORE_TEMP, "wb");
  if (!file) return -errno;
  if (fwrite(&g_store, 1, sizeof(g_store), file) != sizeof(g_store) ||
      fflush(file) != 0 || fsync(fileno(file)) != 0)
    ret = -(errno ? errno : EIO);
  if (fclose(file) != 0 && !ret) ret = -errno;
  if (!ret && rename(STORE_TEMP, STORE_PATH) != 0) ret = -errno;
  if (ret) (void)unlink(STORE_TEMP);
  return ret;
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

static int store_write(int object, const union ble_store_value *value)
{
  int ret = ble_store_ram_write(object, value);
  if (ret) return ret;
  if (object == BLE_STORE_OBJ_TYPE_OUR_SEC ||
      object == BLE_STORE_OBJ_TYPE_PEER_SEC)
    {
      struct ble_store_value_sec *values = object == BLE_STORE_OBJ_TYPE_OUR_SEC ?
                                           g_store.our : g_store.peer;
      uint8_t *count = object == BLE_STORE_OBJ_TYPE_OUR_SEC ?
                       &g_store.our_count : &g_store.peer_count;
      int index = sec_index(values, *count, &value->sec.peer_addr);
      if (index < 0 && *count < STORE_BONDS) index = (*count)++;
      if (index >= 0) values[index] = value->sec;
    }
  else if (object == BLE_STORE_OBJ_TYPE_CCCD)
    {
      int index = cccd_index(g_store.cccd, g_store.cccd_count,
                             &value->cccd.peer_addr,
                             value->cccd.chr_val_handle);
      if (index < 0 && g_store.cccd_count < STORE_CCCDS)
        index = g_store.cccd_count++;
      if (index >= 0) g_store.cccd[index] = value->cccd;
    }
  (void)store_save();
  return 0;
}

static int find_sec_key(const struct ble_store_value_sec *values, unsigned count,
                        const struct ble_store_key_sec *key)
{
  if (ble_addr_cmp(&key->peer_addr, BLE_ADDR_ANY))
    return key->idx < count ? key->idx : -1;
  if (key->idx != 0) return -1;
  return sec_index(values, count, &key->peer_addr);
}

static int find_cccd_key(const struct ble_store_key_cccd *key)
{
  unsigned skipped = 0;
  for (unsigned i = 0; i < g_store.cccd_count; i++)
    {
      if (ble_addr_cmp(&key->peer_addr, BLE_ADDR_ANY) &&
          ble_addr_cmp(&g_store.cccd[i].peer_addr, &key->peer_addr)) continue;
      if (key->chr_val_handle &&
          g_store.cccd[i].chr_val_handle != key->chr_val_handle) continue;
      if (key->idx > skipped++) continue;
      return (int)i;
    }
  return -1;
}

static int store_delete(int object, const union ble_store_key *key)
{
  int ret = ble_store_ram_delete(object, key);
  if (ret) return ret;
  if (object == BLE_STORE_OBJ_TYPE_OUR_SEC)
    {
      int index = find_sec_key(g_store.our, g_store.our_count, &key->sec);
      if (index >= 0) remove_value(g_store.our, sizeof(g_store.our[0]),
                                   (unsigned)index, &g_store.our_count);
    }
  else if (object == BLE_STORE_OBJ_TYPE_PEER_SEC)
    {
      int index = find_sec_key(g_store.peer, g_store.peer_count, &key->sec);
      if (index >= 0) remove_value(g_store.peer, sizeof(g_store.peer[0]),
                                   (unsigned)index, &g_store.peer_count);
    }
  else if (object == BLE_STORE_OBJ_TYPE_CCCD)
    {
      int index = find_cccd_key(&key->cccd);
      if (index >= 0) remove_value(g_store.cccd, sizeof(g_store.cccd[0]),
                                   (unsigned)index, &g_store.cccd_count);
    }
  (void)store_save();
  return 0;
}

void glass_ble_store_init(void)
{
  FILE *file;
  ble_store_ram_init();
  store_reset();
  file = fopen(STORE_PATH, "rb");
  if (file)
    {
      bool loaded = fread(&g_store, 1, sizeof(g_store), file) == sizeof(g_store);
      fclose(file);
      if (!loaded || !store_valid()) store_reset();
    }
  for (unsigned i = 0; i < g_store.our_count; i++)
    {
      union ble_store_value value = {.sec = g_store.our[i]};
      (void)ble_store_ram_write(BLE_STORE_OBJ_TYPE_OUR_SEC, &value);
    }
  for (unsigned i = 0; i < g_store.peer_count; i++)
    {
      union ble_store_value value = {.sec = g_store.peer[i]};
      (void)ble_store_ram_write(BLE_STORE_OBJ_TYPE_PEER_SEC, &value);
    }
  for (unsigned i = 0; i < g_store.cccd_count; i++)
    {
      union ble_store_value value = {.cccd = g_store.cccd[i]};
      (void)ble_store_ram_write(BLE_STORE_OBJ_TYPE_CCCD, &value);
    }
  ble_hs_cfg.store_read_cb = ble_store_ram_read;
  ble_hs_cfg.store_write_cb = store_write;
  ble_hs_cfg.store_delete_cb = store_delete;
}

bool glass_ble_store_has_bond(void)
{
  return g_store.our_count != 0 || g_store.peer_count != 0;
}
