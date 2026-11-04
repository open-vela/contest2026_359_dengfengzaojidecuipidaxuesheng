/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdbool.h>
/* Exercise the production filesystem implementation; intercept only the
 * transaction boundary to count writes and inject a failed medium. */
#define qpk_storage_write fixture_storage_write
#include "../overlay/apps/system/desktop/qpk_storage.c"
#undef qpk_storage_write
int qpk_storage_write(const char *,const char *,const char *,const char *,size_t);
#include "../overlay/apps/system/desktop/glass_portal_config.c"
static unsigned flash_writes;
static bool fail_write;
int qpk_storage_write(const char *root,const char *package,const char *key,const char *data,size_t length)
{
  assert(!strcmp(root,PORTAL_CONFIG_ROOT)&&!strcmp(package,"portal"));
  if(fail_write)return -ENOSPC;
  int ret=fixture_storage_write(root,package,key,data,length);
  if(!ret)flash_writes++;
  return ret;
}
static int patch(const char *text)
{cJSON *o=cJSON_Parse(text);assert(o);int ret=portal_config_save("ai",o);cJSON_Delete(o);return ret;}
static void check(const char *model)
{
  struct portal_ai_config config={0};assert(!portal_ai_load(&config));
  assert(!strcmp(config.model,model)&&!strcmp(config.api_key,"fixture-secret"));
}
int main(int argc,char **argv)
{
  if(argc==2&&!strcmp(argv[1],"jbox")){
    struct portal_jbox_config jbox;assert(!portal_jbox_load(&jbox));
    assert(!strcmp(jbox.host,"127.0.0.1")&&!strcmp(jbox.device_id,"aabbccddeeff")&&strlen(jbox.key)==64);return 0;
  }
  if(argc==2){check(argv[1]);return 0;}
  assert(!patch("{\"model\":\"offline-model\",\"api_key\":\"fixture-secret\",\"base_url\":\"https://model.invalid/v1\"}"));
  assert(flash_writes==1);check("offline-model");
  assert(!patch("{}")&&!patch("{\"model\":\"offline-model\"}")&&flash_writes==1);
  /* A stale or absent SD configuration must never select a different model.
   * No SD volume API is linked into this test. */
  assert(!mkdir(PORTAL_TEST_SD_ROOT,0700));
  FILE *file=fopen(PORTAL_TEST_SD_ROOT "/ai0.json","w");assert(file);
  assert(fputs("{\"model\":\"stale-sd-model\"}",file)>=0&&!fclose(file));
  assert(!patch("{\"model\":\"latest-model\"}")&&flash_writes==2);check("latest-model");
  assert(!unlink(PORTAL_TEST_SD_ROOT "/ai0.json")&&!rmdir(PORTAL_TEST_SD_ROOT));
  check("latest-model");
  pid_t child=fork();assert(child>=0);
  if(!child){execl(argv[0],argv[0],"latest-model",(char *)NULL);_exit(127);}
  int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
  cJSON *redacted=portal_config_get("ai");char *wire=cJSON_PrintUnformatted(redacted);
  assert(wire&&!strstr(wire,"fixture-secret")&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(redacted,"api_key_set")));
  free(wire);cJSON_Delete(redacted);
  fail_write=true;assert(patch("{\"model\":\"must-fail\"}")==-ENOSPC);fail_write=false;
  check("latest-model");assert(flash_writes==2);
  assert(patch("{\"unknown\":1}")==-EINVAL&&patch("{\"model\":\"a\",\"model\":\"b\"}")==-EINVAL);
  assert(portal_config_get("../../data/private")==NULL&&portal_config_get(NULL)==NULL);
  assert(!patch("{}")&&flash_writes==2);
  struct portal_jbox_config jbox={.host="127.0.0.1",.device_id="aabbccddeeff",
    .key="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"},loaded;
  assert(portal_jbox_load(&loaded)==-ENOENT);
  assert(!portal_jbox_save(&jbox)&&flash_writes==3);
  assert(!portal_jbox_save(&jbox)&&flash_writes==3);
  assert(!portal_jbox_load(&loaded)&&!strcmp(jbox.key,loaded.key));
  redacted=portal_config_get("jbox");wire=cJSON_PrintUnformatted(redacted);
  assert(wire&&!strstr(wire,jbox.key)&&!cJSON_GetObjectItemCaseSensitive(redacted,"key")&&
    cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(redacted,"key_set")));free(wire);cJSON_Delete(redacted);
  child=fork();assert(child>=0);
  if(!child){execl(argv[0],argv[0],"jbox",(char *)NULL);_exit(127);}
  assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
  strcpy(jbox.host,"127.0.0.2");fail_write=true;assert(portal_jbox_save(&jbox)==-ENOSPC);fail_write=false;
  assert(!portal_jbox_load(&loaded)&&!strcmp(loaded.host,"127.0.0.1")&&flash_writes==3);
  strcpy(jbox.host,"255.255.255.255");assert(portal_jbox_save(&jbox)==-EINVAL&&flash_writes==3);
  strcpy(jbox.host,"127.0.0.1");jbox.key[0]='z';assert(portal_jbox_save(&jbox)==-EINVAL&&flash_writes==3);
  puts("JBOX_STORAGE_OK: redacted persistent binding; no-op writes skipped; failed writes preserve old key; IPv4/key validation");
  puts("PORTAL_STORAGE_OK: Flash authoritative with/without SD; fresh-process persistence; identical saves skip writes; failed save retains old config; secrets and validation preserved");
  return 0;
}
