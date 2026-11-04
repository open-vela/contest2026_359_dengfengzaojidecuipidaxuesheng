/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <cJSON.h>
#define PORTAL_PORT 8080
#define PORTAL_CODE_LENGTH 6
#define PORTAL_TOKEN_LENGTH 32
struct portal_ai_config {
  char api_key[2049], base_url[1025], model[129], backend[33], system_prompt[1025];
  unsigned max_tokens, timeout_ms;
};
struct portal_voice_config {
  char app_id[33], secret_id[129], secret_key[129];
};
struct portal_jbox_config {char host[16],device_id[13],key[65];};
int portal_jbox_load(struct portal_jbox_config *out);
int portal_jbox_save(const struct portal_jbox_config *config);
int portal_ai_load(struct portal_ai_config *out);
int portal_voice_load(struct portal_voice_config *out);
int portal_music_key(char *out,size_t capacity);
int portal_ha_value(const char *key,char *out,size_t capacity);
int portal_ha_set(const char *key,const char *value);
int portal_ha_save(const char *url,const char *token);
int portal_ha_load(char *url,size_t url_size,char *token,size_t token_size);
int portal_ha_apply(void);
int portal_safe_path(const char *root,const char *relative,char *out,size_t cap,bool parents);
cJSON *portal_config_get(const char *section);
int portal_config_save(const char *section,const cJSON *patch);
/* Opening the page preserves its current code and all connected browsers. */
int portal_session_open(char *code,size_t capacity);
int portal_session_refresh(char *code,size_t capacity);
void portal_session_close(void);
unsigned portal_session_seconds(void);
int portal_service_start(void);
void portal_debug_status(void);
void portal_desktop_publish(const uint8_t values[4]);
bool portal_desktop_take(uint8_t values[4]);
bool portal_relative_path(const char *path);
int portal_public_path(const char *relative,char *out,size_t capacity);
int portal_public_write_path(const char *relative,char *out,size_t capacity);
int portal_remove_tree(const char *path);
int portal_install_begin(const cJSON *manifest,char *error,size_t capacity);
int portal_install_target(const char *relative,char *out,size_t capacity);
int portal_install_commit(void);
void portal_install_abort(void);
