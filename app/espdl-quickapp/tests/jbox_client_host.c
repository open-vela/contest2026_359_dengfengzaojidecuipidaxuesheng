/* SPDX-License-Identifier: Apache-2.0 */
/* Production C client talks only to the loopback test peer. */
#include "qpk_jbox.h"
#include "glass_portal.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static struct portal_jbox_config saved={.host="127.0.0.1",.device_id="aabbccddeeff",
  .key="000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
int portal_jbox_load(struct portal_jbox_config *out){*out=saved;return 0;}
int portal_jbox_save(const struct portal_jbox_config *config){saved=*config;return 0;}
struct cancel_job {struct qpk_hw_progress *progress;unsigned delay;};
static void *cancel_later(void *opaque)
{struct cancel_job *job=opaque;usleep(job->delay*1000);atomic_store(&job->progress->cancel,true);return NULL;}
int main(void)
{
  void *state=NULL;char *line=NULL;size_t capacity=0;setvbuf(stdout,NULL,_IOLBF,0);
  while(getline(&line,&capacity,stdin)>0){
    cJSON *input=cJSON_Parse(line),*out=cJSON_CreateObject();
    if(!input||!out)return 2;
    const cJSON *operation=cJSON_GetObjectItemCaseSensitive(input,"operation");
    const cJSON *arguments=cJSON_GetObjectItemCaseSensitive(input,"arguments");
    struct qpk_hw_progress progress={0};pthread_t thread;
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(input,"cancelBefore")))atomic_store(&progress.cancel,true);
    const cJSON *delay=cJSON_GetObjectItemCaseSensitive(input,"cancelAfterMs");
    struct cancel_job cancel={.progress=&progress,.delay=cJSON_IsNumber(delay)?(unsigned)delay->valueint:0};
    if(cancel.delay&&pthread_create(&thread,NULL,cancel_later,&cancel))return 3;
    int ret=cJSON_IsString(operation)?qpk_jbox_execute(&state,operation->valuestring,arguments,out,&progress):-EINVAL;
    if(cancel.delay)pthread_join(thread,NULL);
    cJSON_AddNumberToObject(out,"error",ret);
    char *encoded=cJSON_PrintUnformatted(out);if(!encoded)return 4;
    puts(encoded);free(encoded);cJSON_Delete(out);cJSON_Delete(input);
  }
  qpk_jbox_destroy(state);free(line);return 0;
}
