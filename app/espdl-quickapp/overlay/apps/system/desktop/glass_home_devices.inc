/* SPDX-License-Identifier: Apache-2.0
 * Bounded desktop shortcuts. Identity changes are persisted; state updates
 * remain in RAM. Only two individual states are read every 30 seconds while
 * the desktop is visible, using the shared asynchronous HA service.
 */
#include "glass_home_devices.h"

struct home_device_s
{
  char entity[128];
  char name[128];
  char state[80];
  bool pinned;
};

static struct
{
  struct home_device_s rows[2];
  char server[192];
  unsigned count;
  unsigned revision;
  bool loaded;
  char save_json[1537];
  uint32_t save_at;
  bool save_dirty;
#ifdef CONFIG_SYSTEM_HASS
  int client;
  int pending;
  unsigned index;
  uint32_t next;
  uint32_t configuration;
#endif
} g_home_devices;

static void glass_home_devices_paint(void);

static bool home_device_string(const cJSON *object, const char *key,
                               char *out, size_t size)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (!cJSON_IsString(value) || !value->valuestring[0] ||
      strlen(value->valuestring) >= size) return false;
  for (const unsigned char *p = (void *)value->valuestring; *p; p++)
    if (*p < 32 || *p == 127) return false;
  strcpy(out, value->valuestring);
  return true;
}

static bool home_device_entity(const char *id)
{
  bool dot = false;
  for (const char *p = id; *p; p++)
    {
      if (*p == '.' && !dot && p != id && p[1]) dot = true;
      else if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
                 *p == '_')) return false;
    }
  return dot;
}

static void home_device_state(const cJSON *body, const char *value,
                              char *out, size_t size)
{
  static const char *const labels[][2] =
    {{"on", "已开启"}, {"off", "已关闭"}, {"unavailable", "设备离线"},
     {"unknown", "状态未知"}, {"idle", "空闲"}, {"open", "已打开"},
     {"closed", "已关闭"}, {"opening", "正在打开"}, {"closing", "正在关闭"},
     {"playing", "正在播放"}, {"paused", "已暂停"}, {"standby", "待机"},
     {"locked", "已上锁"}, {"unlocked", "已解锁"}, {"heat", "制热"},
     {"cool", "制冷"}, {"auto", "自动"}, {"cleaning", "清扫中"},
     {"docked", "已回充"}, {"returning", "正在回充"}};
  const char *text = value;
  for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
    if (!strcmp(value, labels[i][0])) { text = glass_tr(labels[i][1]); break; }
  const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(body, "attributes");
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "entity_id");
  const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(attributes, "brightness");
  if (cJSON_IsString(id) && !strncmp(id->valuestring, "light.", 6) &&
      !strcmp(value, "on") && cJSON_IsNumber(brightness) &&
      brightness->valuedouble >= 0 && brightness->valuedouble <= 255)
    {
      int percent = (int)(brightness->valuedouble * 100 / 255 + .5);
      if (!percent && brightness->valuedouble > 0) percent = 1;
      snprintf(out, size, "%s · %d%%", text, percent);
    }
  else snprintf(out, size, "%s", text);
}

static int home_devices_accept(const char *json, bool persist)
{
  struct home_device_s rows[2] = {0};
  char server[192];
  if (!json || strlen(json) > 1536) return -EINVAL;
  cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
  const cJSON *items = cJSON_GetObjectItemCaseSensitive(root, "items");
  int count = cJSON_GetArraySize(items);
  bool valid = home_device_string(root, "server", server, sizeof(server)) &&
               cJSON_IsArray(items) && count >= 0 && count <= 2;
  for (int i = 0; valid && i < count; i++)
    {
      const cJSON *item = cJSON_GetArrayItem(items, i);
      valid = home_device_string(item, "entity", rows[i].entity, sizeof(rows[i].entity)) &&
              home_device_entity(rows[i].entity) &&
              home_device_string(item, "name", rows[i].name, sizeof(rows[i].name)) &&
              home_device_string(item, "state", rows[i].state, sizeof(rows[i].state));
      rows[i].pinned = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "pinned"));
      if (i && !strcmp(rows[0].entity, rows[i].entity)) valid = false;
    }
  cJSON_Delete(root);
  if (!valid) return -EINVAL;
#ifdef CONFIG_SYSTEM_HASS
  if (persist)
    {
      char endpoint[192];
      int client = hass_open(HASS_READ);
      if (client <= 0) return client ? client : -EIO;
      int ret = hass_get_url(client, endpoint, sizeof(endpoint));
      hass_close(client);
      if (ret < 0 || strcmp(endpoint, server)) return -ENOTCONN;
    }
#endif
  bool selection = strcmp(server, g_home_devices.server) ||
                   count != (int)g_home_devices.count;
  for (int i = 0; i < count && !selection; i++)
    selection = strcmp(rows[i].entity, g_home_devices.rows[i].entity) ||
                strcmp(rows[i].name, g_home_devices.rows[i].name) ||
                rows[i].pinned != g_home_devices.rows[i].pinned;
  if (!persist)
    for (int i = 0; i < count; i++) strcpy(rows[i].state, glass_tr("等待同步"));
#ifdef CONFIG_SYSTEM_HASS
  if (selection && g_home_devices.client > 0)
    {
      hass_close(g_home_devices.client);
      g_home_devices.client = 0;
      g_home_devices.pending = 0;
    }
  if (selection) { g_home_devices.index = 0; g_home_devices.next = 0; }
#endif
  memcpy(g_home_devices.rows, rows, sizeof(rows));
  strcpy(g_home_devices.server, server);
  g_home_devices.count = count;
  g_home_devices.revision++;
  if (persist && selection)
    {
      /* Rank changes can follow each tap. Coalesce writes just like the HA
       * usage counter rather than writing flash for every device visit. */
      strcpy(g_home_devices.save_json, json);
      if (!g_home_devices.save_dirty) g_home_devices.save_at = lv_tick_get() + 10000;
      g_home_devices.save_dirty = true;
    }
  return 0;
}

int glass_home_devices_publish(const char *json)
{
  g_home_devices.loaded = true;
  return home_devices_accept(json, true);
}

static void home_devices_load(void)
{
  if (g_home_devices.loaded) return;
  g_home_devices.loaded = true;
  char json[1537];
  size_t length;
  if (!qpk_storage_read(QPK_DIR "/.data", "com.openvela.homeassistant",
                        "eh_desktop", json, sizeof(json), &length))
    home_devices_accept(json, false);
}

static void home_devices_tick(void)
{
  home_devices_load();
  if (g_home_devices.save_dirty &&
      (int32_t)(lv_tick_get() - g_home_devices.save_at) >= 0)
    {
      int ret = qpk_storage_write(QPK_DIR "/.data", "com.openvela.homeassistant",
                                  "eh_desktop", g_home_devices.save_json,
                                  strlen(g_home_devices.save_json));
      if (!ret) g_home_devices.save_dirty = false;
      else g_home_devices.save_at = lv_tick_get() + 30000;
    }
#ifdef CONFIG_SYSTEM_HASS
  bool visible = g_glass.home && lv_obj_is_visible(g_glass.home) &&
    (!g_desktop.panel || lv_obj_has_flag(g_desktop.panel, LV_OBJ_FLAG_HIDDEN));
  if (!visible)
    {
      if (g_home_devices.client > 0) hass_close(g_home_devices.client);
      g_home_devices.client = g_home_devices.pending = 0;
      return;
    }
  if (g_home_devices.client <= 0) g_home_devices.client = hass_open(HASS_READ);
  if (g_home_devices.client <= 0) return;
  char endpoint[192];
  uint32_t configuration = hass_configuration_revision();
  if (hass_get_url(g_home_devices.client, endpoint, sizeof(endpoint)) < 0 ||
      strcmp(endpoint, g_home_devices.server))
    {
      if (g_home_devices.count) { g_home_devices.count = 0; g_home_devices.revision++; }
      hass_close(g_home_devices.client);
      g_home_devices.client = g_home_devices.pending = 0;
      return;
    }
  if (configuration != g_home_devices.configuration)
    {
      g_home_devices.configuration = configuration;
      hass_close(g_home_devices.client);
      g_home_devices.client = g_home_devices.pending = 0;
      g_home_devices.index = 0; g_home_devices.next = 0;
      return;
    }
  if (g_home_devices.pending > 0)
    {
      struct hass_result_s result = {0};
      int ret = hass_poll(g_home_devices.client, &result);
      if (ret >= 0 && !result.done) { hass_result_free(&result); return; }
      struct home_device_s *row = &g_home_devices.rows[g_home_devices.index];
      const char *state = glass_tr("同步失败");
      cJSON *body = NULL;
      char entity[128], value[129], formatted[80];
      if (ret >= 0 && result.request_id == (uint32_t)g_home_devices.pending &&
          !result.error && result.status == 200)
        {
          body = cJSON_ParseWithOpts(result.body ? result.body : "", NULL, true);
          if (home_device_string(body, "entity_id", entity, sizeof(entity)) &&
              !strcmp(entity, row->entity) && home_device_string(body, "state", value, sizeof(value)))
            {
              home_device_state(body, value, formatted, sizeof(formatted));
              state = strlen(value) >= sizeof(formatted) ? glass_tr("已同步") : formatted;
            }
        }
      else if (result.status == 404) state = glass_tr("设备已移除");
      else if (result.status == 401 || result.status == 403) state = glass_tr("需要重新连接");
      if (strcmp(row->state, state)) { snprintf(row->state, sizeof(row->state), "%s", state); g_home_devices.revision++; }
      cJSON_Delete(body);hass_result_free(&result);
      g_home_devices.pending = 0;
      if (++g_home_devices.index >= g_home_devices.count)
        { g_home_devices.index = 0; g_home_devices.next = lv_tick_get() + 30000; }
    }
  if (g_home_devices.count && (int32_t)(lv_tick_get() - g_home_devices.next) >= 0)
    {
      int request = hass_get_state(g_home_devices.client,
                                   g_home_devices.rows[g_home_devices.index].entity);
      if (request > 0) g_home_devices.pending = request;
      else g_home_devices.next = lv_tick_get() + (request == -EBUSY ? 5000 : 30000);
    }
#endif
}
