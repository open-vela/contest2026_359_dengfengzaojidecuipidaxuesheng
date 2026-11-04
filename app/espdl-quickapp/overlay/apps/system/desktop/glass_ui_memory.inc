/* SPDX-License-Identifier: Apache-2.0 */
/* Worker publishes a policy request; only the GUI owner touches LVGL caches.
 * Acknowledgment follows eviction. Retention resumes lazily after the last
 * local inference phase ends, preserving the previous configured cache size. */
#include <stdatomic.h>
#include "glass_material.h"
#include <lvgl/src/core/lv_global.h>
int glass_ui_memory_begin(unsigned timeout_ms);
void glass_ui_memory_end(void);
extern void glass_material_font_pressure(bool);
void glass_weather_material_pressure(bool);
extern void glass_lyric_font_memory_pressure(bool) __attribute__((weak));
static atomic_uint ui_memory_users,ui_memory_request,ui_memory_ack;
static atomic_bool ui_memory_ready;
static pthread_t ui_memory_owner;
static bool ui_memory_trimmed;
static uint32_t ui_memory_saved_limit;
static void glass_ui_memory_poll(void)
{
 if(!atomic_load_explicit(&ui_memory_ready,memory_order_acquire)){
  ui_memory_owner=pthread_self();atomic_store_explicit(&ui_memory_ready,true,memory_order_release);
 }else if(!pthread_equal(ui_memory_owner,pthread_self()))return;
 unsigned ticket=atomic_load(&ui_memory_request);if(ticket==atomic_load(&ui_memory_ack))return;
 if(atomic_load(&ui_memory_users)){
  if(!ui_memory_trimmed){
   gmat_pressure(true);glass_material_font_pressure(true);glass_weather_material_pressure(true);
   lv_cache_t *cache=LV_GLOBAL_DEFAULT()->img_cache;
   ui_memory_saved_limit=cache?(uint32_t)lv_cache_get_max_size(cache,NULL):0;
   if(cache){lv_image_cache_resize(0,true);lv_image_cache_drop(NULL);}
   if(glass_lyric_font_memory_pressure)glass_lyric_font_memory_pressure(true);ui_memory_trimmed=true;
  }
 }else {
  gmat_pressure(false);if(ui_memory_trimmed){glass_material_font_pressure(false);glass_weather_material_pressure(false);
  if(LV_GLOBAL_DEFAULT()->img_cache)lv_image_cache_resize(ui_memory_saved_limit,false);
  if(glass_lyric_font_memory_pressure)glass_lyric_font_memory_pressure(false);ui_memory_trimmed=false;
 }}
 atomic_store(&ui_memory_ack,ticket);
}
int glass_ui_memory_begin(unsigned timeout_ms)
{
 atomic_fetch_add(&ui_memory_users,1);unsigned ticket=atomic_fetch_add(&ui_memory_request,1)+1;
 if(!atomic_load_explicit(&ui_memory_ready,memory_order_acquire))return -EAGAIN;
 if(pthread_equal(ui_memory_owner,pthread_self())){glass_ui_memory_poll();return 0;}
 if(timeout_ms>250)timeout_ms=250;
 struct timespec started;clock_gettime(CLOCK_MONOTONIC,&started);
 for(;;){
  if((int32_t)(atomic_load(&ui_memory_ack)-ticket)>=0)return 0;
  struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
  int64_t elapsed=(int64_t)(now.tv_sec-started.tv_sec)*1000+(now.tv_nsec-started.tv_nsec)/1000000;
  if(elapsed>=timeout_ms)return -ETIMEDOUT;
  usleep(1000);
 }
}
void glass_ui_memory_end(void)
{
 unsigned users=atomic_load(&ui_memory_users);
 while(users&&!atomic_compare_exchange_weak(&ui_memory_users,&users,users-1)){}
 if(users)atomic_fetch_add(&ui_memory_request,1);
}
