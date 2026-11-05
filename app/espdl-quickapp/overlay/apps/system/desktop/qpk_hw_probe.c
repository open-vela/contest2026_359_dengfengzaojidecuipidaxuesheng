/* SPDX-License-Identifier: Apache-2.0 */
#include <nuttx/config.h>
#include <nuttx/kmalloc.h>
#include "qpk_hardware.h"
#include "qpk_storage.h"
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Explicit NSH diagnostics. Only the hardware-diagnostic package is changed. */
static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
static int number(const cJSON *object, const char *key)
{
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
  return cJSON_IsNumber(v) ? v->valueint : -1;
}
static int check_result(struct qpk_hw_session *session, const char *op,
                         const char *args, int action, cJSON **result)
{
  if (result) *result = NULL;
  int id = qpk_hw_submit(session, op, args);
  if (id < 0) { printf("hardware-test: %s submit=%d\n", op, id); return id; }
  bool controlled = false;
  for (unsigned n = 0; n < 600; n++) {
    char *text = qpk_hw_poll(session, id);
    cJSON *s = text ? cJSON_Parse(text) : NULL; free(text);
    if (!s) { qpk_hw_cancel(session, id, false); return -ENOMEM; }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "done"))) {
      int ret = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s, "ok")) ? 0 : number(s, "error");
      cJSON *out = cJSON_GetObjectItemCaseSensitive(s, "result");
      printf("hardware-test: %s code=%d bytes=%d file=%d rate=%d duration=%d value=%d peak=%d control=%d\n",
             op, ret, number(out, "bytes"), number(out, "fileBytes"), number(out, "sampleRate"),
             number(out, "durationMs"), number(out, "value"), number(s, "peak"), controlled);
      if (action && !controlled) ret = -EIO;
      if (result) *result = cJSON_DetachItemFromObjectCaseSensitive(s, "result");
      cJSON_Delete(s); fflush(stdout); usleep(20000); return ret;
    }
    if (action && !controlled && number(s, "bytes") >= 3200) {
      int ret = qpk_hw_cancel(session, id, action > 0);
      if (ret) { cJSON_Delete(s); return ret; }
      controlled = true;
    }
    cJSON_Delete(s); usleep(20000);
  }
  qpk_hw_cancel(session, id, false);
  printf("hardware-test: %s diagnostic deadline\n", op); return -ETIMEDOUT;
}
static int check(struct qpk_hw_session *s, const char *op, const char *args)
{ return check_result(s, op, args, 0, NULL); }
/* Explicit local-console fixture. Empty routes only; never infer wiring or
 * replace another circuit. The session is released on every exit. */
int qpk_jbox_dht11_probe(unsigned vcc,unsigned data,unsigned ground,unsigned io,unsigned power)
{
  if(vcc>=40||data>=40||ground>=40||io>=9||(power!=49&&power!=50)||
     vcc==data||vcc==ground||data==ground)return -EINVAL;
  if(pthread_mutex_trylock(&probe_lock))return -EBUSY;
  struct qpk_hw_session *s=qpk_hw_create("hardware-diagnostic");
  cJSON *out=NULL;char args[256];int ret=s?0:-ENOMEM;unsigned generation=0;
  if(!ret)ret=check_result(s,"jbox.connect","{}",0,&out);
  if(!ret){int value=number(out,"generation");if(value<1)ret=-EPROTO;else generation=(unsigned)value;}
  cJSON_Delete(out);out=NULL;
  snprintf(args,sizeof(args),"{\"generation\":%u}",generation);
  if(!ret)ret=check_result(s,"jbox.caps",args,0,&out);
  if(!ret){char *text=cJSON_PrintUnformatted(out);printf("JBOX_CAPS %s\n",text?text:"{}");free(text);}
  cJSON_Delete(out);out=NULL;
  if(!ret)ret=check_result(s,"jbox.status",args,0,&out);
  if(!ret){
    cJSON *nets=cJSON_GetObjectItemCaseSensitive(out,"nets");
    if(!cJSON_IsArray(nets)||cJSON_GetArraySize(nets)!=8)ret=-EPROTO;
    else for(const cJSON *net=nets->child;net;net=net->next)if(cJSON_GetArraySize(net))ret=-EBUSY;
  }
  cJSON_Delete(out);out=NULL;
  snprintf(args,sizeof(args),"{\"generation\":%u,\"program\":\"dht11\",\"slot\":0}",generation);
  if(!ret)ret=check(s,"jbox.functionCompile",args);
  snprintf(args,sizeof(args),"{\"generation\":%u,\"nets\":[[%u,%u],[%u,%u],[%u,51]]}",
    generation,vcc,power,data,40+io,ground);
  if(!ret)ret=check(s,"jbox.routeSet",args);
  for(unsigned sample=0;!ret&&sample<3;sample++){
    snprintf(args,sizeof(args),"{\"generation\":%u,\"slot\":0,\"ios\":[%u],\"delayMs\":%u}",
      generation,io,sample?2500:2000);
    ret=check_result(s,"jbox.functionCall",args,0,&out);
    if(!ret){
      cJSON *values=cJSON_GetObjectItemCaseSensitive(out,"values");
      if(!cJSON_IsArray(values)||cJSON_GetArraySize(values)!=2)ret=-EPROTO;
      else {
        cJSON *t=cJSON_GetArrayItem(values,0),*h=cJSON_GetArrayItem(values,1);
        if(!cJSON_IsNumber(t)||!cJSON_IsNumber(h)||t->valueint<0||t->valueint>509||h->valueint<0||h->valueint>1009)ret=-ERANGE;
        else printf("JBOX_DHT11 sample=%u temperature10=%d humidity10=%d checksum=passed\n",sample+1,t->valueint,h->valueint);
      }
    }
    cJSON_Delete(out);out=NULL;
  }
  if(s)qpk_hw_release_wait(s);
  printf("JBOX_DHT11 result=%d released=1 no_automatic_retry=1\n",ret);fflush(stdout);
  pthread_mutex_unlock(&probe_lock);return ret;
}
/* Each operation/JSON pair shares one owner. "wait" accepts milliseconds,
 * which permits a scan or stream to finish without losing its task group. */
int qpk_hw_call(int argc,char **argv)
{
  if(argc%2||pthread_mutex_trylock(&probe_lock))return -EINVAL;
  struct qpk_hw_session *s=qpk_hw_create("hardware-diagnostic");
  int ret=s?0:-ENOMEM;
  for(int i=0;s&&i<argc;i+=2){
    if(!strcmp(argv[i],"wait")){
      char *end;long ms=strtol(argv[i+1],&end,10);
      if(*end||ms<0||ms>15000){ret=-EINVAL;break;}
      usleep((useconds_t)ms*1000);continue;
    }
    cJSON *out=NULL;int code=check_result(s,argv[i],argv[i+1],0,&out);
    char *text=out?cJSON_PrintUnformatted(out):NULL;
    printf("hardware-call: %s code=%d %s\n",argv[i],code,text?text:"{}");
    free(text);cJSON_Delete(out);if(code&&!ret)ret=code;
  }
  if(s)qpk_hw_release_wait(s);
  pthread_mutex_unlock(&probe_lock);return ret;
}
static int value_is(struct qpk_hw_session *s, const char *op, const char *args,
                     const char *key, int expected)
{
  cJSON *result; int ret = check_result(s, op, args, 0, &result);
  if (!ret && number(result, key) != expected) ret = -EIO;
  cJSON_Delete(result); return ret;
}
static int wave_is(struct qpk_hw_session *s, const char *args, int bytes)
{
  cJSON *result; int ret = check_result(s, "audio.info", args, 0, &result);
  if (!ret && (number(result, "bytes") != bytes || number(result, "fileBytes") != bytes + 44 ||
      number(result, "sampleRate") != 16000 || number(result, "channels") != 1 ||
      number(result, "bitsPerSample") != 16 || number(result, "durationMs") != bytes / 32)) ret = -EIO;
  printf("hardware-test: wav-metadata=%s\n", ret ? "FAIL" : "PASS");
  cJSON_Delete(result); return ret;
}
static bool wait_capture(struct qpk_hw_session *s, unsigned id)
{
  for (unsigned i = 0; i < 200; i++) {
    char *text = qpk_hw_poll(s, id); cJSON *state = text ? cJSON_Parse(text) : NULL; free(text);
    if (!state) return false;
    bool ready = number(state, "bytes") >= 3200;
    bool done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(state, "done"));
    cJSON_Delete(state); if (done || ready) return ready && !done;
    usleep(20000);
  }
  return false;
}
static void heaps(const char *phase)
{
  struct mallinfo user = mallinfo(), kernel = kmm_mallinfo();
  printf("hardware-test: %s user-free=%lu kernel-free=%lu kernel-largest=%lu\n", phase,
         (unsigned long)user.fordblks, (unsigned long)kernel.fordblks, (unsigned long)kernel.mxordblk);
  fflush(stdout);
}
int qpk_hw_probe(bool audio_only)
{
  if (pthread_mutex_trylock(&probe_lock)) return 1;
  heaps("before"); int failures = 0;
  struct qpk_hw_session *s = qpk_hw_create("hardware-diagnostic");
  if (!s) { pthread_mutex_unlock(&probe_lock); return 1; }
  if (!audio_only) {
    failures += check(s, "gpio.open", "{\"pin\":6,\"mode\":\"output\",\"value\":1}") != 0;
    failures += value_is(s, "gpio.read", "{\"pin\":6}", "value", 1) != 0;
    failures += check(s, "gpio.write", "{\"pin\":6,\"value\":0}") != 0;
    failures += value_is(s, "gpio.read", "{\"pin\":6}", "value", 0) != 0;
    failures += check(s, "pwm.write", "{\"channel\":3,\"duty\":0.5}") != -EBUSY;
    failures += check(s, "gpio.close", "{\"pin\":6}") != 0;
    const char *opens[] = {"i2c.open", "spi.open", "uart.open"};
    const char *closes[] = {"i2c.close", "spi.close", "uart.close"};
    for (unsigned i = 0; i < 3; i++) {
      if (!check(s, opens[i], "{}")) {
        if (i == 1) failures += check(s, "uart.open", "{}") != -EBUSY;
        failures += check(s, closes[i], "{}") != 0;
      } else failures++;
    }
    failures += check(s, "pwm.write", "{\"channel\":0,\"frequency\":1000,\"duty\":0.25}") != 0;
    failures += check(s, "pwm.write", "{\"channel\":3,\"frequency\":1000,\"duty\":0.75}") != 0;
    failures += check(s, "pwm.stop", "{\"channel\":0}") != 0;
    failures += check(s, "pwm.write", "{\"channel\":3,\"frequency\":800,\"duty\":0.5}") != 0;
    failures += check(s, "pwm.stop", "{\"channel\":3}") != 0;
    failures += check(s, "servo.write", "{\"channel\":3,\"pulseUs\":1500}") != 0;
    failures += check(s, "servo.stop", "{\"channel\":3}") != 0;
    failures += check(s, "ble.start", "{}") != 0;
    bool ready = false;
    for (unsigned i = 0; i < 60 && !ready; i++) {
      cJSON *status; int ret = check_result(s, "ble.status", "{}", 0, &status);
      ready = !ret && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(status, "ready"));
      cJSON_Delete(status); if (!ready) usleep(100000);
    }
    if (ready) {
      failures += check(s, "ble.scan", "{}") != 0; usleep(500000);
      failures += check(s, "ble.status", "{}") != 0;
      failures += check(s, "ble.cancel", "{}") != 0;
    } else failures++;
  }
  failures += check(s, "audio.tone", "{\"durationMs\":120,\"volume\":10}") != 0;
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
  char args[160], early[160], indefinite[160];
  snprintf(args, sizeof(args), "{\"clip\":\"probe_%lx_%lx\",\"durationMs\":500,\"volume\":10}",
           (unsigned long)now.tv_sec, (unsigned long)now.tv_nsec);
  snprintf(indefinite, sizeof(indefinite), "{\"clip\":\"probe_%lx_%lx\",\"durationMs\":0}",
           (unsigned long)now.tv_sec, (unsigned long)now.tv_nsec);
  snprintf(early, sizeof(early), "{\"clip\":\"probe_%lx_%lx_early\",\"durationMs\":0}",
           (unsigned long)now.tv_sec, (unsigned long)now.tv_nsec);
  int recorded = check(s, "audio.record", args); failures += recorded != 0;
  if (!recorded) {
    failures += wave_is(s, args, 16000) != 0;
    failures += check(s, "audio.play", args) != 0;
    failures += check_result(s, "audio.record", indefinite, -1, NULL) != -ECANCELED;
    failures += wave_is(s, args, 16000) != 0;
    failures += check_result(s, "audio.record", early, 1, NULL) != 0;
    cJSON *metadata; int ret = check_result(s, "audio.info", early, 0, &metadata);
    failures += ret != 0 || number(metadata, "bytes") < 3200 || number(metadata, "bytes") >= 32000;
    cJSON_Delete(metadata);
    failures += check(s, "audio.list", "{}") != 0;
    failures += check(s, "audio.remove", args) != 0;
    failures += check(s, "audio.remove", early) != 0;
  }
  /* Exit with active resources, then verify a new session can acquire them. */
  if (!audio_only) failures += check(s, "pwm.write", "{\"channel\":3,\"duty\":0.5}") != 0;
  int active = qpk_hw_submit(s, "audio.record", indefinite);
  bool capturing = active > 0 && wait_capture(s, active);
  failures += !capturing;
  qpk_hw_release_wait(s);
  printf("hardware-test: active-exit capture-started=%d\n", capturing);
  s = qpk_hw_create("hardware-diagnostic");
  if (s) {
    failures += check(s, "audio.tone", "{\"durationMs\":80,\"volume\":10}") != 0;
    if (!audio_only) {
      failures += check(s, "gpio.open", "{\"pin\":6}") != 0;
      failures += check(s, "gpio.close", "{\"pin\":6}") != 0;
    }
    qpk_hw_release_wait(s);
  } else failures++;
  int cleanup = qpk_storage_remove_package(CONFIG_SYSTEM_DESKTOP_QPK_DIR "/.data", "hardware-diagnostic");
  failures += cleanup != 0; usleep(100000); heaps("after");
  printf("hardware-test: failures=%d cleanup=%d; external peripherals and audible quality not verified\n", failures, cleanup);
  fflush(stdout); pthread_mutex_unlock(&probe_lock); return failures ? 1 : 0;
}
