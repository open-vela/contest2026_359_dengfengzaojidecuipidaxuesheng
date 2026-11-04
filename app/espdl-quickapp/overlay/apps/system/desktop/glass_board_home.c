/* SPDX-License-Identifier: Apache-2.0
 * In-process smart home MCP tools for the board server.
 *
 * Every tool runs through the shared Home Assistant service with the same
 * whitelist, registered-service table and read-back verification as the native
 * panel. No tool opens a page or touches LVGL, and no result contains the
 * configured address or token: the chat dialog renders the returned card and
 * the smart home app stays closed. */
#include "glass_agent.h"
#include "glass_board_home.h"
#include "../hass/hass_service.h"
#include "../hass/hass_actions.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

#define HOME_PAGE 16
#define HOME_ENTITY_BYTES 96
#define HOME_TOOL_WAIT_MS 12000
#define HOME_SUBMIT_RETRY_MS 2000
#define HOME_VERIFY_ATTEMPTS 4
#define HOME_VERIFY_INTERVAL_MS 150

static const char home_tool_catalog[] =
"{\"name\":\"home_device\",\"description\":\"Discover all functions of one physical Home Assistant / Mijia device by an exact entity id. Groups only actual device-registry associations, never names. Returns paginated member entities, current attributes, registered actions and parameter ranges/options. Read-only. Use the member's exact entity and advertised service/arguments for home_service; don't operate every member or infer extra actions. Optional query filters function names/entity ids inside this device only; total is the filtered count and member_total is all its state entities.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\",\"minimum\":0},\"query\":{\"type\":\"string\",\"maxLength\":96}},\"required\":[\"entity\"],\"additionalProperties\":false}},"
"{\"name\":\"home_light\",\"description\":\"Preferred single-call light query/control by the user's exact light name or light entity id. Resolves only a unique light; ambiguities return candidates without writing. With query only, reads fresh state including brightness/color. To control, optionally provide on, brightness (percent), rgb_color [r,g,b]; one write followed by verification. No home_list or home_state needed before/after this tool.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"minLength\":1,\"maxLength\":96},\"on\":{\"type\":\"boolean\"},\"brightness\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},\"rgb_color\":{\"type\":\"array\",\"items\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255},\"minItems\":3,\"maxItems\":3}},\"required\":[\"query\"],\"additionalProperties\":false}},"
"{\"name\":\"home_status\",\"description\":\"Read smart home connection status: configured, busy and states-cache freshness. Never returns the address or token.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
"{\"name\":\"home_list\",\"description\":\"Find smart home entities by optional query matching name or entity id. Returns 16 matches per page via offset. Read-only; find exact ids before control.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"maxLength\":96},\"offset\":{\"type\":\"integer\",\"minimum\":0}},\"additionalProperties\":false}},"
"{\"name\":\"home_state\",\"description\":\"Read one entity's current state by exact entity id (domain.object_id). Read-only.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity\":{\"type\":\"string\"}},\"required\":[\"entity\"],\"additionalProperties\":false}},"
"{\"name\":\"home_control\",\"description\":\"Turn one light, switch, input_boolean or fan on or off; optional brightness 1-100 for lights only. Verifies by reading the entity back and renders a result card in the chat instead of opening any page.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity\":{\"type\":\"string\"},\"on\":{\"type\":\"boolean\"},\"brightness\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100}},\"required\":[\"entity\",\"on\"],\"additionalProperties\":false}},"
"{\"name\":\"home_service\",\"description\":\"Run one documented Home Assistant service on one entity, for example scene turn_on, cover set_cover_position with {\\\"position\\\":60} or climate set_temperature. Unknown fields are rejected.\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"entity\":{\"type\":\"string\"},\"service\":{\"type\":\"string\"},\"arguments\":{\"type\":\"object\"}},\"required\":[\"entity\",\"service\"],\"additionalProperties\":false}}";

static void home_sleep_ms(unsigned ms) { usleep((useconds_t)ms * 1000); }

static uint64_t home_now(void)
{
  struct timespec time;
  if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) return 0;
  return (uint64_t)time.tv_sec * 1000 + time.tv_nsec / 1000000;
}

static char *home_error(const char *code)
{
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
  cJSON_AddBoolToObject(root, "ok", false);
  cJSON_AddStringToObject(root, "error", code);
  char *text = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return text;
}

static char *home_result(cJSON *root)
{
  char *text = root ? cJSON_PrintUnformatted(root) : NULL;
  cJSON_Delete(root);
  return text;
}

static const char *home_string(const cJSON *args, const char *key)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, key);
  return cJSON_IsString(value) ? value->valuestring : "";
}

static bool home_args_only(const cJSON *args, const char *keys)
{
  if (!cJSON_IsObject(args)) return false;
  for (const cJSON *value = args->child; value; value = value->next) {
    if (cJSON_GetObjectItemCaseSensitive(args, value->string) != value) return false;
    if (!value->string || strlen(value->string) > 31) return false;
    for (const char *p = value->string; *p; p++)
      if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    char field[64];
    snprintf(field, sizeof(field), ",%s,", value->string);
    if (!strstr(keys, field)) return false;
  }
  return true;
}

static bool home_integer(const cJSON *args, const char *key, int low, int high, int *out)
{
  const cJSON *value = cJSON_GetObjectItemCaseSensitive(args, key);
  if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble < low || value->valuedouble > high ||
      value->valuedouble != (int)value->valuedouble) return false;
  *out = (int)value->valuedouble;
  return true;
}

static bool home_entity_valid(const char *entity)
{
  if (!entity || strlen(entity) >= HOME_ENTITY_BYTES || *entity < 'a' || *entity > 'z') return false;
  unsigned dots = 0;
  for (const char *p = entity; *p; p++) {
    if (*p == '.') { if (p == entity || !p[1] || ++dots > 1) return false; }
    else if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
  }
  return dots == 1;
}

static bool home_identity(const cJSON *body, const char *entity)
{
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "entity_id");
  return cJSON_IsString(id) && !strcmp(id->valuestring, entity);
}

/* Bounded, UTF-8-safe copy for anything that reaches the dialog card. */
static void home_text(char *out, size_t capacity, const char *text)
{
  size_t used = 0;
  if (!text) text = "";
  while (*text && used + 1 < capacity) {
    unsigned char code = (unsigned char)*text;
    size_t size = code < 0x80 ? 1 : (code >= 0xc2 && code <= 0xdf) ? 2 :
                  (code >= 0xe0 && code <= 0xef) ? 3 : (code >= 0xf0 && code <= 0xf4) ? 4 : 0;
    if (!size) { text++; continue; }
    if (size > 1) {
      if (used + size + 1 > capacity) break;
      bool valid = true;
      for (size_t i = 1; i < size; i++) {
        if (!text[i] || ((unsigned char)text[i] & 0xc0) != 0x80) { valid = false; break; }
      }
      if (!valid) { text++; continue; }
      memcpy(out + used, text, size); used += size; text += size;
      continue;
    }
    out[used++] = code >= 0x20 && code != 0x7f ? (char)code : ' ';
    text++;
  }
  out[used] = 0;
  while (used && out[used - 1] == ' ') out[--used] = 0;
}

/* Same wording as the native device cards. */
static const char *home_label(const char *value)
{
  static const char *const labels[][2] =
    {{"on", "已开启"}, {"off", "已关闭"}, {"unavailable", "设备离线"},
     {"unknown", "状态未知"}, {"idle", "空闲"}, {"open", "已打开"},
     {"closed", "已关闭"}, {"opening", "正在打开"}, {"closing", "正在关闭"},
     {"playing", "正在播放"}, {"paused", "已暂停"}, {"standby", "待机"},
     {"locked", "已上锁"}, {"unlocked", "已解锁"}, {"heat", "制热"},
     {"cool", "制冷"}, {"auto", "自动"}, {"cleaning", "清扫中"},
     {"docked", "已回充"}, {"returning", "正在回充"}};
  for (unsigned i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
    if (!strcmp(value, labels[i][0])) return labels[i][1];
  return NULL;
}

static const char *home_kind(const char *domain)
{
  if (!strcmp(domain, "light")) return "light";
  if (!strcmp(domain, "switch") || !strcmp(domain, "input_boolean")) return "switch";
  if (!strcmp(domain, "fan")) return "fan";
  if (!strcmp(domain, "sensor")) return "sensor";
  if (!strcmp(domain, "binary_sensor")) return "binary";
  if (!strcmp(domain, "climate")) return "climate";
  if (!strcmp(domain, "media_player")) return "media";
  if (!strcmp(domain, "cover")) return "cover";
  if (!strcmp(domain, "vacuum")) return "vacuum";
  if (!strcmp(domain, "camera")) return "camera";
  if (!strcmp(domain, "humidifier")) return "humidifier";
  if (!strcmp(domain, "water_heater")) return "heater";
  if (!strcmp(domain, "lock")) return "lock";
  if (!strcmp(domain, "scene")) return "scene";
  if (!strcmp(domain, "script")) return "script";
  return "generic";
}

static const char *home_domain(const char *entity, char *out, size_t capacity)
{
  const char *dot = strchr(entity, '.');
  size_t size = dot ? (size_t)(dot - entity) : 0;
  if (size >= capacity) size = capacity - 1;
  memcpy(out, entity, size); out[size] = 0;
  return out;
}

/* One dialog card from a Home Assistant entity state. */
static cJSON *home_card(const cJSON *body)
{
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "entity_id");
  const cJSON *state = cJSON_GetObjectItemCaseSensitive(body, "state");
  if (!cJSON_IsString(id) || !cJSON_IsString(state)) return NULL;
  char domain[24], name[64], raw[64], text[80];
  home_domain(id->valuestring, domain, sizeof(domain));
  const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(body, "attributes");
  const cJSON *friendly = cJSON_GetObjectItemCaseSensitive(attributes, "friendly_name");
  const char *fallback = strchr(id->valuestring, '.');
  home_text(name, sizeof(name), cJSON_IsString(friendly) ? friendly->valuestring :
            fallback ? fallback + 1 : id->valuestring);
  home_text(raw, sizeof(raw), state->valuestring);
  const char *label = home_label(raw);
  const cJSON *unit = cJSON_GetObjectItemCaseSensitive(attributes, "unit_of_measurement");
  if (!label && cJSON_IsString(unit) && unit->valuestring[0]) {
    char safe[16];
    home_text(safe, sizeof(safe), unit->valuestring);
    snprintf(text, sizeof(text), "%s %s", raw, safe);
  } else snprintf(text, sizeof(text), "%s", label ? label : raw);
  int level = -1;
  const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(attributes, "brightness");
  const cJSON *percentage = cJSON_GetObjectItemCaseSensitive(attributes, "percentage");
  const cJSON *position = cJSON_GetObjectItemCaseSensitive(attributes, "current_position");
  if (!strcmp(domain, "light") && cJSON_IsNumber(brightness) &&
      brightness->valuedouble >= 0 && brightness->valuedouble <= 255)
    level = (int)(brightness->valuedouble * 100 / 255 + .5);
  else if (!strcmp(domain, "fan") && cJSON_IsNumber(percentage) &&
           percentage->valuedouble >= 0 && percentage->valuedouble <= 100)
    level = (int)percentage->valuedouble;
  else if (!strcmp(domain, "cover") && cJSON_IsNumber(position) &&
           position->valuedouble >= 0 && position->valuedouble <= 100)
    level = (int)position->valuedouble;
  static const char *const active[] =
    {"on", "open", "opening", "playing", "cleaning", "locking", "unlocking",
     "heat", "cool", "auto", "home"};
  bool on = false;
  for (unsigned i = 0; i < sizeof(active) / sizeof(active[0]); i++)
    if (!strcmp(state->valuestring, active[i])) on = true;
  cJSON *card = cJSON_CreateObject();
  if (!card) return NULL;
  if (!cJSON_AddStringToObject(card, "entity", id->valuestring) ||
      !cJSON_AddStringToObject(card, "kind", home_kind(domain)) ||
      !cJSON_AddStringToObject(card, "name", name) ||
      !cJSON_AddStringToObject(card, "state", text) ||
      !cJSON_AddNumberToObject(card, "level", level) ||
      !cJSON_AddBoolToObject(card, "on", on)) { cJSON_Delete(card); return NULL; }
  return card;
}

/* Card for action-only domains (scene/script/button): the accepted call is the
 * visible outcome; the read only supplies the display name when available. */
static cJSON *home_action_card(const char *entity, const cJSON *body, const char *verb)
{
  char domain[24], name[64];
  home_domain(entity, domain, sizeof(domain));
  const char *friendly = "";
  if (body) {
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(body, "attributes");
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(attributes, "friendly_name");
    if (cJSON_IsString(value)) friendly = value->valuestring;
  }
  const char *fallback = strchr(entity, '.');
  home_text(name, sizeof(name), friendly[0] ? friendly : fallback ? fallback + 1 : entity);
  cJSON *card = cJSON_CreateObject();
  if (!card) return NULL;
  if (!cJSON_AddStringToObject(card, "entity", entity) ||
      !cJSON_AddStringToObject(card, "kind", home_kind(domain)) ||
      !cJSON_AddStringToObject(card, "name", name) ||
      !cJSON_AddStringToObject(card, "state", verb) ||
      !cJSON_AddNumberToObject(card, "level", -1) ||
      !cJSON_AddBoolToObject(card, "on", false)) { cJSON_Delete(card); return NULL; }
  return card;
}

static const char *home_error_code(int ret)
{
  switch (-ret) {
    case ENOTCONN: return "not_configured";
    case EBUSY: return "smart_home_busy";
    case EINVAL: return "invalid_arguments";
    case EACCES: return "entity_not_allowed";
    case ESTALE: return "configuration_changed";
    case ECANCELED: return "cancelled";
    case ETIMEDOUT: return "timeout";
    case ENOENT: return "not_found";
    default: return "smart_home_unavailable";
  }
}

static const char *home_http_code(int status)
{
  if (status == 404) return "not_found";
  if (status == 401 || status == 403) return "authentication_failed";
  if (status >= 400) return "request_rejected";
  return "request_failed";
}

static bool home_finish(int client, int request, struct hass_result_s *result,
                        unsigned timeout_ms, atomic_bool *cancel)
{
  uint64_t deadline = home_now() + timeout_ms;
  for (;;) {
    if (cancel && atomic_load(cancel)) return false;
    if (hass_poll((uint32_t)client, result)) return false;
    if (result->done && (int)result->request_id == request) return true;
    if (result->done) { hass_result_free(result); return false; }
    if (home_now() >= deadline) { hass_result_free(result); return false; }
    home_sleep_ms(50);
  }
}

/* Submits one request, waiting out a busy service, then waits for completion.
 * The configuration revision is pinned so a mid-flight reconfigure is refused
 * instead of applied to the wrong endpoint. */
static int home_submit_wait_until(int client, const char *resource, const char *entity, bool control,
                            bool on, int brightness, const char *action, const char *data,
                            struct hass_result_s *result, atomic_bool *cancel,
                            uint32_t revision, uint64_t deadline)
{
  memset(result, 0, sizeof(*result));
  if (control && !revision) return -ENOTCONN;
  uint64_t retry_until = home_now() + HOME_SUBMIT_RETRY_MS;
  int request;
  for (;;) {
    if (cancel && atomic_load(cancel)) return -ECANCELED;
    if (home_now() >= deadline) return -ETIMEDOUT;
    if (hass_configuration_revision() != revision) return -ESTALE;
    request = !control ? (entity ? hass_get_state((uint32_t)client, entity) : hass_get((uint32_t)client, resource))
              : action ? hass_call_service_checked((uint32_t)client, entity, action, data, revision)
                       : hass_control_checked((uint32_t)client, entity, on, brightness, revision);
    if (request > 0) break;
    if (request != -EBUSY || home_now() >= retry_until) return request;
    if (cancel && atomic_load(cancel)) return -ECANCELED;
    home_sleep_ms(50);
  }
  uint64_t now = home_now();
  if (now >= deadline || !home_finish(client, request, result, (unsigned)(deadline - now), cancel))
    return cancel && atomic_load(cancel) ? -ECANCELED : -ETIMEDOUT;
  if (hass_configuration_revision() != revision) { hass_result_free(result); return -ESTALE; }
  return 0;
}

static int home_submit_wait(int client, const char *resource, const char *entity, bool control,
                            bool on, int brightness, const char *action, const char *data,
                            struct hass_result_s *result, atomic_bool *cancel)
{
  return home_submit_wait_until(client, resource, entity, control, on, brightness, action, data,
    result, cancel, hass_configuration_revision(), home_now() + HOME_TOOL_WAIT_MS);
}

static bool home_matches(const char *body, const char *entity, bool on, int level)
{
  cJSON *parsed = body ? cJSON_Parse(body) : NULL;
  const cJSON *state = cJSON_GetObjectItemCaseSensitive(parsed, "state");
  bool ok = home_identity(parsed, entity) && cJSON_IsString(state) &&
            !strcmp(state->valuestring, on ? "on" : "off");
  if (ok && level >= 0) {
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(parsed, "attributes");
    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(attributes, "brightness");
    int percent = -1;
    if (cJSON_IsNumber(brightness) && brightness->valuedouble >= 0 && brightness->valuedouble <= 255)
      percent = (int)(brightness->valuedouble * 100 / 255 + .5);
    ok = percent >= level - 1 && percent <= level + 1;
  }
  cJSON_Delete(parsed);
  return ok;
}

/* The native panel's window: at most four reads inside eight seconds, and a
 * matching read is the only evidence a control succeeded. */
static char *home_verify(int client, const char *entity, bool on, int level, atomic_bool *cancel,
                         uint32_t revision)
{
  uint64_t deadline = home_now() + 8000;
  for (unsigned attempt = 0; attempt < HOME_VERIFY_ATTEMPTS; attempt++) {
    if (attempt) home_sleep_ms(HOME_VERIFY_INTERVAL_MS << (attempt-1));
    if (cancel && atomic_load(cancel)) return NULL;
    struct hass_result_s read;
    if (home_submit_wait_until(client, NULL, entity, false, false, -1, NULL, NULL, &read, cancel,
                              revision, deadline)) { hass_result_free(&read); break; }
    if (read.error || read.status != 200 || !read.body) { hass_result_free(&read); continue; }
    char *body = read.body;
    read.body = NULL;
    if (home_matches(body, entity, on, level)) return body;
    free(body);
  }
  return NULL;
}

static char *home_read_once(int client, const char *entity, atomic_bool *cancel, uint32_t revision,uint64_t deadline)
{
  struct hass_result_s read;
  if (home_submit_wait_until(client, NULL, entity, false, false, -1, NULL, NULL, &read, cancel,
                            revision, deadline)) { hass_result_free(&read); return NULL; }
  if (read.error || read.status != 200 || !read.body) { hass_result_free(&read); return NULL; }
  char *body = read.body;
  read.body = NULL;
  cJSON *parsed = body ? cJSON_Parse(body) : NULL;
  bool valid = home_identity(parsed, entity);
  cJSON_Delete(parsed);
  if (!valid) { free(body); return NULL; }
  return body;
}

static char *home_status_tool(void)
{
  bool configured = false, busy = false;
  unsigned clients = 0;
  hass_status(&configured, &busy, &clients);
  cJSON *root = cJSON_CreateObject();
  if (!root) return NULL;
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddBoolToObject(root, "configured", configured);
  cJSON_AddBoolToObject(root, "busy", busy);
  if (configured) {
    int client = hass_open(HASS_READ);
    if (client > 0) {
      bool fresh = false;
      uint32_t age = 0;
      if (!hass_cache_info((uint32_t)client, &fresh, &age)) {
        cJSON_AddBoolToObject(root, "states_cached", fresh);
        if (fresh) cJSON_AddNumberToObject(root, "states_cache_age_ms", age);
      }
      hass_close((uint32_t)client);
    }
  } else cJSON_AddStringToObject(root, "hint", "Home Assistant 尚未配置；请用户在设备设置中填写地址和令牌。");
  return home_result(root);
}

static char *home_list_tool(const cJSON *args, atomic_bool *cancel)
{
  if (!home_args_only(args, ",offset,,query,")) return home_error("invalid_arguments");
  const cJSON *query=cJSON_GetObjectItemCaseSensitive(args,"query");
  if(query&&(!cJSON_IsString(query)||strlen(query->valuestring)>96))return home_error("invalid_arguments");
  const char *needle=query?query->valuestring:"";
  int offset = 0;
  if (cJSON_GetObjectItemCaseSensitive(args, "offset") && !home_integer(args, "offset", 0, 100000, &offset))
    return home_error("invalid_arguments");
  int client = hass_open(HASS_READ);
  if (client <= 0) return home_error("smart_home_unavailable");
  struct hass_result_s result;
  int ret = home_submit_wait(client, "states", NULL, false, false, -1, NULL, NULL, &result, cancel);
  if (ret) { hass_close((uint32_t)client); return home_error(home_error_code(ret)); }
  if (result.error || result.status < 200 || result.status >= 300) {
    const char *code = result.error ? home_error_code(result.error) : home_http_code(result.status);
    hass_result_free(&result); hass_close((uint32_t)client);
    return home_error(code);
  }
  cJSON *states = cJSON_Parse(result.body);
  hass_result_free(&result);
  if (!cJSON_IsArray(states)) { cJSON_Delete(states); hass_close((uint32_t)client); return home_error("states_unavailable"); }
  cJSON *root = cJSON_CreateObject(), *items = root ? cJSON_AddArrayToObject(root, "items") : NULL;
  if (!root || !items) { cJSON_Delete(states); cJSON_Delete(root); hass_close((uint32_t)client); return NULL; }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddNumberToObject(root, "offset", offset);
  unsigned total = (unsigned)cJSON_GetArraySize(states);
  unsigned matches=0;
  for (unsigned i = 0; i < total; i++) {
    const cJSON *item = cJSON_GetArrayItem(states, (int)i);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "entity_id");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(item, "state");
    if (!cJSON_IsString(id) || !cJSON_IsString(state)) continue;
    const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(item, "attributes");
    const cJSON *friendly = cJSON_GetObjectItemCaseSensitive(attributes, "friendly_name");
    char name[64], value[48];
    home_text(name, sizeof(name), cJSON_IsString(friendly) ? friendly->valuestring : id->valuestring);
    home_text(value, sizeof(value), state->valuestring);
    if(needle[0]&&!strstr(name,needle)&&!strstr(id->valuestring,needle))continue;
    unsigned index=matches++;
    if(index<(unsigned)offset||index>=(unsigned)offset+HOME_PAGE)continue;
    cJSON *entry = cJSON_CreateObject();
    if (!entry || !cJSON_AddStringToObject(entry, "entity", id->valuestring) ||
        !cJSON_AddStringToObject(entry, "name", name) ||
        !cJSON_AddStringToObject(entry, "state", value)) { cJSON_Delete(entry); continue; }
    cJSON_AddItemToArray(items, entry);
  }
  cJSON_AddNumberToObject(root,"total",matches);
  if ((unsigned)offset + HOME_PAGE < matches) cJSON_AddBoolToObject(root, "more", true);
  cJSON_Delete(states); hass_close((uint32_t)client);
  return home_result(root);
}

#include "glass_home_capabilities.inc"

/* Export documented control/state fields; omit URLs and private metadata. */
static void home_observed(cJSON *root,const cJSON *body)
{
  if(!cJSON_IsObject(body))return;
  const char *state=home_string(body,"state");
  if(state[0]&&!cJSON_GetObjectItemCaseSensitive(root,"state"))cJSON_AddStringToObject(root,"state",state);
  const cJSON *source=cJSON_GetObjectItemCaseSensitive(body,"attributes");
  cJSON *out=cJSON_AddObjectToObject(root,"attributes");
  const char *keys[]={"brightness","color_mode","supported_color_modes","rgb_color","rgbw_color","rgbww_color","hs_color","xy_color","color_temp_kelvin","color_temp",
    "min_color_temp_kelvin","max_color_temp_kelvin","min_mireds","max_mireds","effect","effect_list","supported_features",
    "min","max","step","options","unit_of_measurement","temperature_unit","temperature","current_temperature","min_temp","max_temp","target_temp_step","target_temp_low","target_temp_high",
    "humidity","current_humidity","min_humidity","max_humidity","target_humidity_step","hvac_modes","hvac_action","fan_mode","fan_modes","preset_mode","preset_modes",
    "swing_mode","swing_modes","swing_horizontal_mode","swing_horizontal_modes","percentage","percentage_step","oscillating","direction",
    "current_position","current_tilt_position","mode","available_modes","operation_mode","operation_list","away_mode",
    "volume_level","is_volume_muted","source","source_list","fan_speed","fan_speed_list"};
  for(unsigned i=0;i<sizeof(keys)/sizeof(keys[0]);i++){
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(source,keys[i]);
    if(!value)continue;
    bool valid=cJSON_IsBool(value)||(cJSON_IsNumber(value)&&isfinite(value->valuedouble))||
      (cJSON_IsString(value)&&strlen(value->valuestring)<=512);
    if(cJSON_IsArray(value)&&cJSON_GetArraySize(value)<=64){valid=true;const cJSON *v;
      cJSON_ArrayForEach(v,value)if(!((cJSON_IsString(v)&&strlen(v->valuestring)<=512)||
        (cJSON_IsNumber(v)&&isfinite(v->valuedouble)))){valid=false;break;}}
    if(valid){cJSON *copy=cJSON_Duplicate(value,true);if(copy)cJSON_AddItemToObject(out,keys[i],copy);}
  }
}

static char *home_state_at(const cJSON *args, atomic_bool *cancel,uint32_t revision)
{
  const char *entity = home_string(args, "entity");
  if (!home_args_only(args, ",entity,") || !home_entity_valid(entity))
    return home_error("invalid_arguments");
  int client = hass_open(HASS_READ);
  if (client <= 0) return home_error("smart_home_unavailable");
  struct hass_result_s result;
  int ret = home_submit_wait_until(client, NULL, entity, false, false, -1, NULL, NULL, &result, cancel,
                                  revision,home_now()+HOME_TOOL_WAIT_MS);
  if (ret) { hass_close((uint32_t)client); return home_error(home_error_code(ret)); }
  if (result.error || result.status < 200 || result.status >= 300) {
    const char *code = result.error ? home_error_code(result.error) : home_http_code(result.status);
    hass_result_free(&result); hass_close((uint32_t)client);
    return home_error(code);
  }
  cJSON *body = cJSON_Parse(result.body);
  hass_result_free(&result);
  cJSON *card = home_identity(body, entity) ? home_card(body) : NULL;
  if (!card) { cJSON_Delete(body); hass_close((uint32_t)client); return home_error("unreadable_response"); }
  cJSON *root = cJSON_CreateObject();
  if (!root) { cJSON_Delete(card); cJSON_Delete(body); hass_close((uint32_t)client); return NULL; }
  const cJSON *name = cJSON_GetObjectItemCaseSensitive(card, "name");
  const cJSON *state = cJSON_GetObjectItemCaseSensitive(body, "state");
  char value[48];
  if (cJSON_IsString(state)) home_text(value, sizeof(value), state->valuestring); else value[0] = 0;
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "entity", entity);
  if (cJSON_IsString(name)) cJSON_AddStringToObject(root, "name", name->valuestring);
  if (value[0]) cJSON_AddStringToObject(root, "state", value);
  cJSON_AddBoolToObject(root, "available", strcmp(value, "unavailable") != 0);
  cJSON_AddItemToObject(root, "card", card);
  home_observed(root,body);
  cJSON_AddItemToObject(root,"actions",home_entity_actions(body,NULL));
  cJSON_AddBoolToObject(root,"registered_services_checked",false);
  cJSON_Delete(body); hass_close((uint32_t)client);
  return home_result(root);
}

static cJSON *home_resource(int client,const char *resource,atomic_bool *cancel,uint32_t revision)
{
  struct hass_result_s response;
  int ret=home_submit_wait_until(client,resource,NULL,false,false,-1,NULL,NULL,&response,cancel,revision,home_now()+HOME_TOOL_WAIT_MS);
  if(ret)return NULL;
  cJSON *out=!response.error&&response.status==200&&response.body?cJSON_Parse(response.body):NULL;
  hass_result_free(&response);return out;
}
static char *home_device_tool(const cJSON *args,atomic_bool *cancel)
{
  const char *entity=home_string(args,"entity"),*query=home_string(args,"query");int offset=0;
  const cJSON *filter=cJSON_GetObjectItemCaseSensitive(args,"query");
  if(!home_args_only(args,",entity,,offset,,query,")||!home_entity_valid(entity)||(filter&&(!cJSON_IsString(filter)||strlen(query)>96))||
    (cJSON_GetObjectItemCaseSensitive(args,"offset")&&!home_integer(args,"offset",0,100000,&offset)))return home_error("invalid_arguments");
  int client=hass_open(HASS_READ);if(client<=0)return home_error("smart_home_unavailable");
  uint32_t revision=hass_configuration_revision();
  cJSON *states=home_resource(client,"states",cancel,revision),*devices=NULL,*services=NULL,*out=NULL;
  if(!cJSON_IsArray(states)){cJSON_Delete(states);hass_close((uint32_t)client);return home_error("states_unavailable");}
  const cJSON *anchor=NULL,*item;cJSON_ArrayForEach(item,states)if(!strcmp(home_string(item,"entity_id"),entity)){
    if(anchor){cJSON_Delete(states);hass_close((uint32_t)client);return home_error("ambiguous_entity");}anchor=item;
  }
  if(!anchor){cJSON_Delete(states);hass_close((uint32_t)client);return home_error("not_found");}
  devices=home_resource(client,"devices",cancel,revision);
  services=home_resource(client,"services",cancel,revision);
  const cJSON *device=NULL,*members=NULL;
  if(cJSON_IsArray(devices))cJSON_ArrayForEach(item,devices){
    if(!cJSON_IsArray(item))continue;
    const cJSON *ids=cJSON_GetArrayItem(item,3),*id;if(!cJSON_IsArray(ids))continue;
    cJSON_ArrayForEach(id,ids)if(cJSON_IsString(id)&&!strcmp(id->valuestring,entity)){
      if(device&&device!=item){out=cJSON_Parse("{\"ok\":false,\"error\":\"ambiguous_device_registry\"}");goto done;}
      device=item;members=ids;break;
    }
  }
  out=cJSON_CreateObject();if(!out)goto done;
  cJSON_AddBoolToObject(out,"ok",true);cJSON_AddStringToObject(out,"entity",entity);
  cJSON_AddBoolToObject(out,"registry_available",cJSON_IsArray(devices));
  cJSON_AddBoolToObject(out,"registered_services_checked",cJSON_IsArray(services));
  if(device){const char *fields[]={"device_id","name","area"};for(unsigned i=0;i<3;i++){
      const cJSON *v=cJSON_GetArrayItem(device,i);char text[96];home_text(text,sizeof(text),cJSON_IsString(v)?v->valuestring:"");cJSON_AddStringToObject(out,fields[i],text);}}
  cJSON *functions=cJSON_AddArrayToObject(out,"functions");unsigned total=0,member_total=0,returned=0,bytes=0;bool full=false;
  if(!functions)goto done;
  cJSON_ArrayForEach(item,states){
    const char *id=home_string(item,"entity_id");bool member=!strcmp(id,entity);
    if(members){member=false;const cJSON *v;cJSON_ArrayForEach(v,members)if(cJSON_IsString(v)&&!strcmp(v->valuestring,id))member=true;}
    if(!member||!home_entity_valid(id)||!cJSON_IsString(cJSON_GetObjectItemCaseSensitive(item,"state")))continue;
    member_total++;
    const char *name=home_string(cJSON_GetObjectItemCaseSensitive(item,"attributes"),"friendly_name");
    if(query[0]&&!strstr(name,query)&&!strstr(id,query))continue;
    unsigned index=total++;if(index<(unsigned)offset||returned>=8||full)continue;
    cJSON *function=home_card(item);if(!function)continue;
    cJSON_AddStringToObject(function,"raw_state",home_string(item,"state"));
    home_observed(function,item);
    /* Missing service discovery is read-only, never inferred writable. */
    cJSON_AddItemToObject(function,"actions",cJSON_IsArray(services)?home_entity_actions(item,services):cJSON_CreateArray());
    char *encoded=cJSON_PrintUnformatted(function);size_t size=encoded?strlen(encoded):20000;free(encoded);
    if(bytes+size>12000){cJSON_Delete(function);if(!returned){cJSON_Delete(out);out=cJSON_Parse("{\"ok\":false,\"error\":\"entity_capabilities_too_large\"}");goto done;}full=true;continue;}
    bytes+=(unsigned)size;cJSON_AddItemToArray(functions,function);returned++;
  }
  cJSON_AddNumberToObject(out,"member_total",member_total);
  if(query[0])cJSON_AddStringToObject(out,"query",query);
  cJSON_AddNumberToObject(out,"offset",offset);cJSON_AddNumberToObject(out,"total",total);
  cJSON_AddNumberToObject(out,"next_offset",(unsigned)offset+returned);
  cJSON_AddBoolToObject(out,"more",(unsigned)offset+returned<total);
done:
  if(hass_configuration_revision()!=revision||(cancel&&atomic_load(cancel))){cJSON_Delete(out);out=cJSON_CreateObject();if(out){cJSON_AddBoolToObject(out,"ok",false);cJSON_AddStringToObject(out,"error",hass_configuration_revision()!=revision?"configuration_changed":"cancelled");}}
  cJSON_Delete(states);cJSON_Delete(devices);cJSON_Delete(services);hass_close((uint32_t)client);
  return home_result(out);
}

static char *home_control_tool(const cJSON *args, atomic_bool *cancel)
{
  const char *entity = home_string(args, "entity");
  const cJSON *on = cJSON_GetObjectItemCaseSensitive(args, "on");
  const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(args, "brightness");
  if (!home_args_only(args, ",entity,,on,,brightness,") || !home_entity_valid(entity) ||
      !cJSON_IsBool(on)) return home_error("invalid_arguments");
  int level = -1;
  if (brightness && (!home_integer(args, "brightness", 1, 100, &level) || !cJSON_IsTrue(on)))
    return home_error("invalid_arguments");
  int client = hass_open(HASS_READ | HASS_CONTROL);
  if (client <= 0) return home_error("smart_home_unavailable");
  struct hass_result_s result;
  uint32_t revision = hass_configuration_revision();
  int ret = home_submit_wait_until(client, NULL, entity, true, cJSON_IsTrue(on), level,
                             NULL, NULL, &result, cancel, revision, home_now() + HOME_TOOL_WAIT_MS);
  if (ret) { hass_close((uint32_t)client); return home_error(home_error_code(ret)); }
  if (result.error || result.status < 200 || result.status >= 300) {
    const char *code = result.error ? home_error_code(result.error) : home_http_code(result.status);
    hass_result_free(&result); hass_close((uint32_t)client);
    return home_error(code);
  }
  hass_result_free(&result);
  char *observed = home_verify(client, entity, cJSON_IsTrue(on), level, cancel, revision);
  cJSON *root = cJSON_CreateObject();
  if (!root) { free(observed); hass_close((uint32_t)client); return NULL; }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "entity", entity);
  cJSON_AddBoolToObject(root, "on", cJSON_IsTrue(on));
  if (level >= 0) cJSON_AddNumberToObject(root, "brightness", level);
  cJSON *body = observed ? cJSON_Parse(observed) : NULL;
  cJSON *card = cJSON_IsObject(body) ? home_card(body) : NULL;
  if (card) {
    cJSON_AddBoolToObject(root, "verified", true);
    cJSON_AddItemToObject(root, "card", card);
  } else {
    cJSON_AddBoolToObject(root, "verified", false);
    cJSON_AddStringToObject(root, "note", "已发送控制，但没有读到一致状态，结果未确认；不要自动重发。");
    cJSON_AddItemToObject(root, "card", home_action_card(entity, NULL, "结果未确认"));
  }
  cJSON_Delete(body); free(observed); hass_close((uint32_t)client);
  return home_result(root);
}

static bool home_service_name(const char *service)
{
  size_t size = strlen(service);
  if (!size || size > 31) return false;
  for (const char *p = service; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
  return true;
}

/* Only properties with an explicit state mapping can confirm completion. */
static bool home_service_matches(const cJSON *observed, const char *service, const cJSON *args)
{
  const char *state = home_string(observed, "state");
  if (!state[0] || !strcmp(state, "unknown") || !strcmp(state, "unavailable")) return false;
  if (!strcmp(service, "turn_off")) return !strcmp(state, "off");
  bool matched = false;
  if (!strcmp(service,"turn_on")){
    char domain[24];home_domain(home_string(observed,"entity_id"),domain,sizeof(domain));
    bool active=!strcmp(domain,"climate")||!strcmp(domain,"media_player")||!strcmp(domain,"water_heater")?strcmp(state,"off")!=0:!strcmp(state,"on");
    if(!active)return false;
    matched=true;
  }
  if(!strcmp(service,"open_cover")){
    const cJSON *position=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(observed,"attributes"),"current_position");
    return !strcmp(state,"open")&&(!cJSON_IsNumber(position)||position->valuedouble==100);
  }
  if(!strcmp(service,"close_cover"))return !strcmp(state,"closed");
  if(!strcmp(service,"stop_cover"))return strcmp(state,"opening")&&strcmp(state,"closing");
  if(!strcmp(service,"open_cover_tilt")||!strcmp(service,"close_cover_tilt")){
    const cJSON *position=cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(observed,"attributes"),"current_tilt_position");
    return cJSON_IsNumber(position)&&position->valuedouble==(!strcmp(service,"open_cover_tilt")?100:0);
  }
  if(!strcmp(service,"media_play"))return !strcmp(state,"playing");
  if(!strcmp(service,"media_pause")||!strcmp(service,"pause"))return !strcmp(state,"paused");
  if(!strcmp(service,"media_stop"))return !strcmp(state,"idle")||!strcmp(state,"off");
  if(!strcmp(service,"start"))return !strcmp(state,"cleaning");
  if(!strcmp(service,"stop"))return !strcmp(state,"idle")||!strcmp(state,"docked")||!strcmp(state,"paused");
  if(!strcmp(service,"return_to_base"))return !strcmp(state,"returning")||!strcmp(state,"docked");
  const cJSON *attributes = cJSON_GetObjectItemCaseSensitive(observed, "attributes");
  for (const cJSON *p = args ? args->child : NULL; p; p = p->next) {
    const char *key = p->string;
    if (!strcmp(key, "entity_id")) return false;
    const char *mapped = !strcmp(key, "position") ? "current_position" :
                         !strcmp(key,"tilt_position")?"current_tilt_position":
                         !strcmp(key, "brightness_pct") ? "brightness" : key;
    const cJSON *actual = (!strcmp(key, "value") || !strcmp(key, "option") || !strcmp(key, "hvac_mode"))
                         ? cJSON_GetObjectItemCaseSensitive(observed, "state")
                         : cJSON_GetObjectItemCaseSensitive(attributes, mapped);
    if (cJSON_IsNumber(p)) {
      double got;
      if (cJSON_IsNumber(actual)) got = actual->valuedouble;
      else if (cJSON_IsString(actual)) {
        char *end; got = strtod(actual->valuestring, &end);
        if (end == actual->valuestring || *end) return false;
      } else return false;
      if (!strcmp(key, "brightness_pct")) got = got * 100 / 255;
      double tolerance=!strcmp(key,"brightness_pct")?1.0:!strcmp(key,"color_temp_kelvin")?50.0:!strcmp(key,"volume_level")?0.02:0.01;
      if (!isfinite(got) || fabs(got - p->valuedouble) > tolerance) return false;
    } else if(cJSON_IsArray(p)) {
      int count=!strcmp(key,"rgb_color")?3:!strcmp(key,"rgbw_color")?4:!strcmp(key,"rgbww_color")?5:
        (!strcmp(key,"hs_color")||!strcmp(key,"xy_color"))?2:0;
      if(!count||!cJSON_IsArray(actual)||cJSON_GetArraySize(p)!=count||cJSON_GetArraySize(actual)!=count)return false;
      double tolerance=!strcmp(key,"xy_color")?.005:!strcmp(key,"hs_color")?1.0:2.0;
      for(int i=0;i<count;i++){
        const cJSON *want=cJSON_GetArrayItem(p,i),*got=cJSON_GetArrayItem(actual,i);
        if(!cJSON_IsNumber(want)||!cJSON_IsNumber(got)||!isfinite(got->valuedouble)||!isfinite(want->valuedouble)||
          fabs(want->valuedouble-got->valuedouble)>tolerance)return false;
      }
    } else if (cJSON_IsString(p)) {
      if (!cJSON_IsString(actual) || strcmp(p->valuestring, actual->valuestring)) return false;
    } else if (cJSON_IsBool(p)) {
      if (!cJSON_IsBool(actual) || cJSON_IsTrue(p) != cJSON_IsTrue(actual)) return false;
    } else return false;
    matched = true;
  }
  return matched;
}

/* Keep the result card/speech about the function that was actually verified. */
static void home_function_summary(cJSON *card,const char *service,const cJSON *args,const cJSON *observed)
{
  if(!card||!cJSON_IsObject(args))return;
  static const char *const names[][2]={{"direction","风向"},{"preset_mode","模式"},{"oscillating","摆风"},{"tilt_position","叶片角度"},
    {"temperature","目标温度"},{"target_temp_low","温度下限"},{"target_temp_high","温度上限"},{"humidity","目标湿度"},
    {"fan_mode","风速模式"},{"swing_mode","摆风"},{"swing_horizontal_mode","水平摆风"},{"away_mode","外出模式"},
    {"operation_mode","运行模式"},{"effect","灯光效果"},{"source","输入源"},{"fan_speed","清扫强度"}};
  const cJSON *attrs=cJSON_GetObjectItemCaseSensitive(observed,"attributes");
  char text[256]="";size_t used=0;
  for(unsigned i=0;i<sizeof(names)/sizeof(*names);i++){
    const char *key=names[i][0];if(!cJSON_GetObjectItemCaseSensitive(args,key))continue;
    const cJSON *actual=cJSON_GetObjectItemCaseSensitive(attrs,!strcmp(key,"tilt_position")?"current_tilt_position":key);
    char value[128]="";
    if(cJSON_IsNumber(actual))snprintf(value,sizeof(value),"%.6g%s",actual->valuedouble,!strcmp(key,"humidity")||!strcmp(key,"tilt_position")?"%":"");
    else if(cJSON_IsBool(actual))snprintf(value,sizeof(value),"%s",cJSON_IsTrue(actual)?"开启":"关闭");
    else if(cJSON_IsString(actual)){
      const char *word=!strcmp(actual->valuestring,"forward")?"正转":!strcmp(actual->valuestring,"reverse")?"反转":actual->valuestring;
      home_text(value,sizeof(value),word);
    }
    if(!value[0])continue;
    int n=snprintf(text+used,sizeof(text)-used,"%s%s：%s",used?" · ":"",names[i][1],value);
    if(n<0||(size_t)n>=sizeof(text)-used)break;
    used+=(unsigned)n;
  }
  if(text[0]){char safe[64];home_text(safe,sizeof(safe),text);cJSON_ReplaceItemInObjectCaseSensitive(card,"state",cJSON_CreateString(safe));}
  if(!strcmp(service,"set_cover_tilt_position"))cJSON_ReplaceItemInObjectCaseSensitive(card,"level",cJSON_CreateNumber(-1));
}

static char *home_service_at(const cJSON *args, atomic_bool *cancel,uint32_t revision)
{
  const char *entity = home_string(args, "entity");
  const char *service = home_string(args, "service");
  const cJSON *arguments = cJSON_GetObjectItemCaseSensitive(args, "arguments");
  if (!home_args_only(args, ",entity,,service,,arguments,") || !home_entity_valid(entity) ||
      !home_service_name(service) ||
      (arguments && !cJSON_IsObject(arguments))) return home_error("invalid_arguments");
  char domain[24];
  home_domain(entity, domain, sizeof(domain));
  if (!strcmp(domain, "lock") || !strcmp(domain, "alarm_control_panel")) return home_error("entity_not_allowed");
  /* Compose the exact body the native panel sends; the shared service validates
   * domain, service, entity and every parameter against its rule table. */
  char *body = NULL;
  {
    char *printed = cJSON_IsObject(arguments) ? cJSON_PrintUnformatted(arguments) : strdup("{}");
    if (!printed) return NULL;
    size_t capacity = strlen(printed) + strlen(entity) + 32;
    body = malloc(capacity);
    if (body) {
      if (printed[1] == '}') snprintf(body, capacity, "{\"entity_id\":\"%s\"}", entity);
      else snprintf(body, capacity, "{\"entity_id\":\"%s\",%.*s}", entity, (int)(strlen(printed) - 2), printed + 1);
    }
    free(printed);
    if (!body) return NULL;
  }
  if (strlen(body) > 1023) { free(body); return home_error("invalid_arguments"); }
  if(!hass_action_valid(domain,service,entity,body)){free(body);return home_error("invalid_arguments");}
  int client = hass_open(HASS_READ | HASS_CONTROL);
  if (client <= 0) { free(body); return home_error("smart_home_unavailable"); }
  bool action_only=!strcmp(domain,"scene")||!strcmp(domain,"script")||!strcmp(domain,"button")||!strcmp(domain,"input_button");
  if(!action_only&&strcmp(domain,"light")){
    char *fresh=home_read_once(client,entity,cancel,revision,home_now()+HOME_TOOL_WAIT_MS);
    cJSON *state=fresh?cJSON_Parse(fresh):NULL;free(fresh);
    cJSON *actions=state?home_entity_actions(state,NULL):NULL;
    bool supported=false;const cJSON *entry;
    cJSON_ArrayForEach(entry,actions)if(!strcmp(home_string(entry,"service"),service)&&home_parameters_match(entry,arguments))supported=true;
    cJSON_Delete(actions);cJSON_Delete(state);
    if(!supported){free(body);hass_close((uint32_t)client);return home_error("function_or_value_not_supported");}
  }
  struct hass_result_s result;
  int ret = home_submit_wait_until(client, NULL, entity, true, false, -1, service, body, &result, cancel,
                                  revision, home_now() + HOME_TOOL_WAIT_MS);
  free(body);
  if (ret) { hass_close((uint32_t)client); return home_error(home_error_code(ret)); }
  if (result.error || result.status < 200 || result.status >= 300) {
    const char *code = result.error ? home_error_code(result.error) : home_http_code(result.status);
    hass_result_free(&result); hass_close((uint32_t)client);
    return home_error(code);
  }
  hass_result_free(&result);
  action_only=action_only||!strcmp(service,"media_next_track")||!strcmp(service,"media_previous_track")||!strcmp(service,"stop_cover_tilt");
  char *observed = NULL;
  cJSON *readback = NULL;
  cJSON *card = NULL;
  bool verified=false;
  uint64_t deadline=home_now()+8000;
  for(unsigned attempt=0;attempt<HOME_VERIFY_ATTEMPTS;attempt++){
    if(attempt)home_sleep_ms(HOME_VERIFY_INTERVAL_MS << (attempt-1));
    if((cancel&&atomic_load(cancel))||home_now()>=deadline)break;
    free(observed);cJSON_Delete(readback);
    observed=home_read_once(client,entity,cancel,revision,deadline);
    readback=observed?cJSON_Parse(observed):NULL;
    verified=!action_only&&cJSON_IsObject(readback)&&home_service_matches(readback,service,arguments);
    if(verified||action_only)break;
  }
  if (verified){card=home_card(readback);home_function_summary(card,service,arguments,readback);}
  else card = home_action_card(entity, readback, action_only ? "已受理" : "结果未确认");
  cJSON *root = cJSON_CreateObject();
  if (!root) { cJSON_Delete(card); cJSON_Delete(readback); free(observed); hass_close((uint32_t)client); return NULL; }
  cJSON_AddBoolToObject(root, "ok", true);
  cJSON_AddStringToObject(root, "entity", entity);
  cJSON_AddStringToObject(root, "service", service);
  cJSON_AddBoolToObject(root, "accepted", true);
  cJSON_AddBoolToObject(root, "verified", verified);
  home_observed(root,readback);
  if (card) cJSON_AddItemToObject(root, "card", card);
  if (!verified) cJSON_AddStringToObject(root, "note", "服务已被受理，执行结果未确认；不要自动重发。");
  cJSON_Delete(readback); free(observed); hass_close((uint32_t)client);
  return home_result(root);
}

static char *home_light_tool(const cJSON *args,atomic_bool *cancel)
{
  const cJSON *query=cJSON_GetObjectItemCaseSensitive(args,"query");
  const cJSON *on=cJSON_GetObjectItemCaseSensitive(args,"on");
  const cJSON *brightness=cJSON_GetObjectItemCaseSensitive(args,"brightness");
  const cJSON *rgb=cJSON_GetObjectItemCaseSensitive(args,"rgb_color");
  int level=0;
  if(!home_args_only(args,",query,,on,,brightness,,rgb_color,")||
     !cJSON_IsString(query)||!query->valuestring[0]||strlen(query->valuestring)>96||
     (on&&!cJSON_IsBool(on))||(brightness&&!home_integer(args,"brightness",1,100,&level))||
     (rgb&&(!cJSON_IsArray(rgb)||cJSON_GetArraySize(rgb)!=3))||
     (cJSON_IsFalse(on)&&(brightness||rgb)))return home_error("invalid_arguments");
  if(rgb)for(const cJSON *channel=rgb->child;channel;channel=channel->next)
    if(!cJSON_IsNumber(channel)||!isfinite(channel->valuedouble)||channel->valuedouble<0||
       channel->valuedouble>255||channel->valuedouble!=(int)channel->valuedouble)
      return home_error("invalid_arguments");
  uint32_t revision=hass_configuration_revision();
  char entity[HOME_ENTITY_BYTES]={0};
  const char *needle=query->valuestring;
  if(home_entity_valid(needle)){
    if(strncmp(needle,"light.",6))return home_error("entity_not_allowed");
    strcpy(entity,needle);
  }else{
    int client=hass_open(HASS_READ);if(client<=0)return home_error("smart_home_unavailable");
    struct hass_result_s result;
    int ret=home_submit_wait_until(client,"states",NULL,false,false,-1,NULL,NULL,&result,cancel,
                                  revision,home_now()+HOME_TOOL_WAIT_MS);
    hass_close((uint32_t)client);
    if(ret)return home_error(home_error_code(ret));
    if(result.error||result.status!=200){
      const char *code=result.error?home_error_code(result.error):home_http_code(result.status);
      hass_result_free(&result);return home_error(code);
    }
    cJSON *states=cJSON_Parse(result.body);hass_result_free(&result);
    if(!cJSON_IsArray(states)){cJSON_Delete(states);return home_error("states_unavailable");}
    unsigned count=0;
    cJSON *candidates=cJSON_CreateArray();
    if(!candidates){cJSON_Delete(states);return NULL;}
    for(const cJSON *state=states->child;state;state=state->next){
      const char *id=home_string(state,"entity_id");
      const char *name=home_string(cJSON_GetObjectItemCaseSensitive(state,"attributes"),"friendly_name");
      if(strncmp(id,"light.",6)||!home_entity_valid(id)||(strcmp(name,needle)&&strcmp(id,needle)))continue;
      /* Exact names only. Duplicate names still require clarification. */
      count++;snprintf(entity,sizeof(entity),"%s",id);
      if(count<=6){cJSON *item=cJSON_CreateObject();char label[64];home_text(label,sizeof(label),name);
        if(item){cJSON_AddStringToObject(item,"entity",id);cJSON_AddStringToObject(item,"name",label);cJSON_AddItemToArray(candidates,item);}}
    }
    cJSON_Delete(states);
    if(count!=1){
      cJSON *out=cJSON_CreateObject();
      if(!out){cJSON_Delete(candidates);return NULL;}
      cJSON_AddBoolToObject(out,"ok",false);
      cJSON_AddStringToObject(out,"error",count?"ambiguous_light":"light_not_found");
      cJSON_AddNumberToObject(out,"matches",count);cJSON_AddItemToObject(out,"candidates",candidates);
      return home_result(out);
    }
    cJSON_Delete(candidates);
  }
  cJSON *resolved=cJSON_CreateObject();if(!resolved)return NULL;
  cJSON_AddStringToObject(resolved,"entity",entity);
  char *out;
  if(!on&&!brightness&&!rgb)out=home_state_at(resolved,cancel,revision);
  else{
    cJSON_AddStringToObject(resolved,"service",cJSON_IsFalse(on)?"turn_off":"turn_on");
    cJSON *parameters=cJSON_AddObjectToObject(resolved,"arguments");
    if(!parameters){cJSON_Delete(resolved);return NULL;}
    if(brightness)cJSON_AddNumberToObject(parameters,"brightness_pct",level);
    if(rgb){cJSON *copy=cJSON_Duplicate(rgb,true);if(!copy){cJSON_Delete(resolved);return NULL;}cJSON_AddItemToObject(parameters,"rgb_color",copy);}
    out=home_service_at(resolved,cancel,revision);
  }
  cJSON_Delete(resolved);return out;
}

static char *home_call_body(const char *tool, const cJSON *args, atomic_bool *cancel)
{
  bool light_write=!strcmp(tool,"home_light")&&(cJSON_GetObjectItemCaseSensitive(args,"on")||
    cJSON_GetObjectItemCaseSensitive(args,"brightness")||cJSON_GetObjectItemCaseSensitive(args,"rgb_color"));
  if (!strcmp(tool, "home_control") || !strcmp(tool, "home_service")||light_write) {
    cJSON *config = NULL;
    int loaded = portal_agent_load(&config);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(config, "home_control_enabled");
    bool allowed = !loaded && cJSON_IsNumber(enabled) && enabled->valuedouble == 1;
    cJSON_Delete(config);
    if (!allowed) return home_error("home_control_disabled");
  }
  if (!strcmp(tool, "home_status"))
    return cJSON_GetArraySize(args) == 0 ? home_status_tool() : home_error("invalid_arguments");
  if (!strcmp(tool, "home_list")) return home_list_tool(args, cancel);
  if (!strcmp(tool,"home_device"))return home_device_tool(args,cancel);
  if (!strcmp(tool, "home_light")) return home_light_tool(args,cancel);
  if (!strcmp(tool, "home_state")) return home_state_at(args, cancel,hass_configuration_revision());
  if (!strcmp(tool, "home_control")) return home_control_tool(args, cancel);
  if (!strcmp(tool, "home_service")) return home_service_at(args, cancel,hass_configuration_revision());
  return home_error("invalid_arguments");
}
static char *home_call(const char *tool,const cJSON *args,atomic_bool *cancel)
{
  uint64_t started=home_now();char *out=home_call_body(tool,args,cancel);
  printf("[chat-latency] tool=%s elapsed_ms=%lu\n",tool,(unsigned long)(home_now()-started));
  return out;
}

static const struct glass_board_home home_binding = { home_tool_catalog, home_call };
void glass_board_home_register(void) { glass_agent_set_board_home(&home_binding); }

/* Local offline model uses the very same typed service/capability/readback
 * path. The revision was captured BEFORE model generation, and reaches
 * checked transport submission; a late HA configuration swap cannot retarget
 * an accepted request. No new MCP permission or arbitrary-service API. */
uint32_t glass_board_home_revision(void *opaque)
{(void)opaque;return hass_configuration_revision();}
char *glass_board_home_local_checked(void *opaque,const char *method,const char *entity,bool on,uint32_t revision,atomic_bool *cancel)
{
 (void)opaque;
 if(!method||!entity||!home_entity_valid(entity)||strncmp(entity,"light.",6))return home_error("invalid_arguments");
 cJSON *args=cJSON_CreateObject();if(!args)return NULL;cJSON_AddStringToObject(args,"entity",entity);char *out=NULL;
 if(!strcmp(method,"home_state"))out=home_state_at(args,cancel,revision);
 else if(!strcmp(method,"home_power")){
  cJSON *config=NULL;int loaded=portal_agent_load(&config);const cJSON *enabled=cJSON_GetObjectItemCaseSensitive(config,"home_control_enabled");
  bool allowed=!loaded&&cJSON_IsNumber(enabled)&&enabled->valuedouble==1;cJSON_Delete(config);
  if(!allowed)out=home_error("home_control_disabled");
  else{
   cJSON_AddStringToObject(args,"service",on?"turn_on":"turn_off");cJSON_AddObjectToObject(args,"arguments");
   out=home_service_at(args,cancel,revision);
  }
 }else out=home_error("invalid_arguments");
 cJSON_Delete(args);return out;
}
