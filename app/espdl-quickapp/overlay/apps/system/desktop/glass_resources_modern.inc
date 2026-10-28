/* SPDX-License-Identifier: Apache-2.0 */
#include "glass_system_monitor.h"
#define GLASS_RESOURCE_HISTORY 60
static struct {
  lv_obj_t *card,*labels[10],*tiles[2],*mini[2],*chart,*heading,*axis,*empty;
  lv_chart_series_t *mini_series[2],*series;
  lv_timer_t *timer;struct glass_system_monitor *monitor;
  struct glass_system_sample latest;
  char displayed[10][96];int32_t history[2][GLASS_RESOURCE_HISTORY];
  unsigned count[2],selected;uint32_t sample_tick;
  bool sampled,presented,plot_dirty[2];uint8_t presented_metric;
} g_resources;
static uint32_t resources_color(unsigned metric)
{return metric?(g_desktop.light_theme?0x7b68a5:0xb8a0df):(g_desktop.light_theme?0x4b84c9:0x80b0ed);}
static void glass_resources_text(unsigned field,const char *text)
{if(strcmp(g_resources.displayed[field],text)){strlcpy(g_resources.displayed[field],text,sizeof(g_resources.displayed[field]));lv_label_set_text(g_resources.labels[field],text);}}
static void resources_mib(char *out,size_t size,uint64_t bytes)
{snprintf(out,size,"%llu.%u MiB",(unsigned long long)(bytes/(1024*1024)),(unsigned)((bytes%(1024*1024))*10/(1024*1024)));}
static unsigned resources_memory_pct(const struct glass_system_sample *s)
{
  if(!s->heap_total||s->heap_free>s->heap_total)return 0;
  uint64_t used=s->heap_total-s->heap_free;
  return used<=UINT64_MAX/1000?(unsigned)(used*1000/s->heap_total):(unsigned)((used/1024)*1000/(s->heap_total/1024));
}
static lv_obj_t *resources_chart(lv_obj_t *parent,int x,int y,int w,int h,unsigned metric,lv_chart_series_t **series)
{
  lv_obj_t *chart=lv_chart_create(parent);lv_obj_set_pos(chart,x,y);lv_obj_set_size(chart,w,h);
  lv_obj_set_style_bg_color(chart,lv_color_hex(g_desktop.light_theme?0xf9fafc:0x252934),0);
  lv_obj_set_style_bg_opa(chart,LV_OPA_COVER,0);lv_obj_set_style_border_width(chart,1,0);
  lv_obj_set_style_border_color(chart,lv_color_hex(detail_line()),0);lv_obj_set_style_radius(chart,0,0);
  lv_obj_set_style_pad_all(chart,0,0);lv_obj_set_style_line_color(chart,lv_color_hex(detail_line()),LV_PART_MAIN);
  lv_obj_set_style_line_width(chart,1,LV_PART_MAIN);lv_obj_set_style_line_width(chart,2,LV_PART_ITEMS);
  lv_obj_set_style_size(chart,0,0,LV_PART_INDICATOR);lv_obj_remove_flag(chart,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  lv_chart_set_type(chart,LV_CHART_TYPE_LINE);lv_chart_set_range(chart,LV_CHART_AXIS_PRIMARY_Y,0,1000);
  lv_chart_set_point_count(chart,2);lv_chart_set_div_line_count(chart,h>100?6:2,h>100?11:3);
  *series=lv_chart_add_series(chart,lv_color_hex(resources_color(metric)),LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_all_value(chart,*series,0);lv_obj_add_flag(chart,LV_OBJ_FLAG_HIDDEN);return chart;
}
static void resources_plot(lv_obj_t *chart,lv_chart_series_t *series,unsigned metric)
{
  unsigned n=g_resources.count[metric];if(!n){lv_obj_add_flag(chart,LV_OBJ_FLAG_HIDDEN);return;}
  lv_obj_remove_flag(chart,LV_OBJ_FLAG_HIDDEN);lv_chart_set_point_count(chart,n<2?2:n);
  /* A single reading becomes a flat segment; past history is never invented. */
  for(unsigned i=0;i<(n<2?2:n);i++)lv_chart_set_value_by_id(chart,series,i,g_resources.history[metric][i<n?i:0]);
  lv_chart_refresh(chart);
}
static void resources_present(void)
{
  unsigned chosen=g_resources.selected;const struct glass_system_sample *s=&g_resources.latest;char text[96];
  bool selection_changed=!g_resources.presented||chosen!=g_resources.presented_metric;
  if(selection_changed)lv_label_set_text(g_resources.heading,chosen?glass_tr("内存"):"CPU");
  if(chosen&&(s->heap_error||!s->heap_total||s->heap_free>s->heap_total))strcpy(text,glass_tr("暂不可用"));
  else if(!chosen&&s->cpu_error)strcpy(text,s->cpu_error==ENOTSUP?glass_tr("未启用"):glass_tr("暂不可用"));
  else if(!g_resources.sampled)strcpy(text,"--");
  else {unsigned pct=chosen?resources_memory_pct(s):s->cpu_tenths;snprintf(text,sizeof(text),"%u.%u%%",pct/10,pct%10);}
  glass_resources_text(6,text);
  if(selection_changed)lv_chart_set_series_color(g_resources.chart,g_resources.series,lv_color_hex(resources_color(chosen)));
  if(!chosen)snprintf(text,sizeof(text),glass_tr("整体负载 · 最近 %u 次有效采样"),g_resources.count[0]);
  else if(!s->heap_error&&s->heap_total&&s->heap_free<=s->heap_total){char total[48];resources_mib(total,sizeof(total),s->heap_total);snprintf(text,sizeof(text),glass_tr("系统堆总量 %s"),total);}
  else strcpy(text,glass_tr("系统堆数据暂不可用"));
  glass_resources_text(4,text);
  if(selection_changed||g_resources.plot_dirty[chosen])resources_plot(g_resources.chart,g_resources.series,chosen);
  for(unsigned i=0;i<2;i++){
    if(selection_changed){
      lv_obj_set_style_border_color(g_resources.tiles[i],lv_color_hex(i==chosen?resources_color(i):detail_line()),0);
      lv_obj_set_style_border_width(g_resources.tiles[i],i==chosen?2:1,0);
    }
    if(g_resources.plot_dirty[i])resources_plot(g_resources.mini[i],g_resources.mini_series[i],i);
  }
  if(!g_resources.count[chosen])lv_obj_remove_flag(g_resources.empty,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(g_resources.empty,LV_OBJ_FLAG_HIDDEN);
  unsigned n=g_resources.count[chosen];
  if(n>1)snprintf(text,sizeof(text),glass_tr("%u 秒前"),n-1);else strcpy(text,glass_tr("开始采样"));
  if(strcmp(lv_label_get_text(g_resources.axis),text))lv_label_set_text(g_resources.axis,text);
  g_resources.plot_dirty[0]=g_resources.plot_dirty[1]=false;
  g_resources.presented=true;g_resources.presented_metric=chosen;
}
static void resources_select(lv_event_t *e)
{g_resources.selected=(unsigned)(uintptr_t)lv_event_get_user_data(e);resources_present();}
static void glass_resources_closed(lv_event_t *e)
{LV_UNUSED(e);if(g_resources.timer)lv_timer_delete(g_resources.timer);glass_system_release(g_resources.monitor);memset(&g_resources,0,sizeof(g_resources));}
static void resources_append(unsigned metric,int32_t value)
{
  unsigned n=g_resources.count[metric];
  bool changed=n<GLASS_RESOURCE_HISTORY||g_resources.history[metric][n-1]!=value;
  for(unsigned i=1;!changed&&i<n;i++)changed=g_resources.history[metric][i]!=g_resources.history[metric][i-1];
  if(n==GLASS_RESOURCE_HISTORY){memmove(g_resources.history[metric],g_resources.history[metric]+1,(n-1)*sizeof(int32_t));n--;}
  g_resources.history[metric][n]=value;g_resources.count[metric]=n+1;
  g_resources.plot_dirty[metric]|=changed;
}
static void glass_resources_show(const struct glass_system_sample *s)
{
  g_resources.latest=*s;char text[96];
  bool heap_ok=!s->heap_error&&s->heap_total&&s->heap_free<=s->heap_total;
  if(!g_resources.sampled||lv_tick_elaps(g_resources.sample_tick)>=900){
    if(!s->cpu_error)resources_append(0,s->cpu_tenths>1000?1000:s->cpu_tenths);
    if(heap_ok)resources_append(1,resources_memory_pct(s));
    g_resources.sample_tick=lv_tick_get();g_resources.sampled=true;
  }
  /* Keep collecting history while covered; defer labels/plots, not sampling. */
  if(!glass_ui_can_paint(g_resources.card))return;
  if(!s->cpu_error)snprintf(text,sizeof(text),"%u.%u%%",s->cpu_tenths/10,s->cpu_tenths%10);
  else strcpy(text,s->cpu_error==ENOTSUP?glass_tr("未启用"):glass_tr("暂不可用"));
  glass_resources_text(0,text);
  if(heap_ok){unsigned pct=resources_memory_pct(s);snprintf(text,sizeof(text),"%u.%u%%",pct/10,pct%10);}else strcpy(text,glass_tr("暂不可用"));glass_resources_text(1,text);
  const uint64_t values[]={s->heap_largest,s->heap_peak,heap_ok?s->heap_total-s->heap_free:0,s->heap_free};const unsigned fields[]={2,3,7,8};
  for(unsigned i=0;i<4;i++){if(heap_ok)resources_mib(text,sizeof(text),values[i]);else strcpy(text,"--");glass_resources_text(fields[i],text);}
  glass_resources_text(5,s->cpu_error==ENOTSUP?glass_tr("CPU 统计未启用 · 内存范围为系统堆"):s->cpu_error||!heap_ok?glass_tr("部分数据暂不可用 · 每秒重试"):glass_tr("每秒采样 · 最多保留 60 次 · 内存范围为系统堆"));
  resources_present();
}
static void glass_resources_update(lv_timer_t *timer)
{
  LV_UNUSED(timer);
  if(!g_resources.monitor){int error=glass_system_start(&g_resources.monitor);
    if(error==EBUSY){glass_resources_text(5,glass_tr("等待上一次采样结束"));return;}
    if(error){char text[96];snprintf(text,sizeof(text),glass_tr("采样启动失败 (%d)"),error);glass_resources_text(5,text);if(g_resources.timer)lv_timer_delete(g_resources.timer);g_resources.timer=NULL;return;}}
  struct glass_system_status status;int error=glass_system_read(g_resources.monitor,&status);
  if(error||status.stopped){char text[96];snprintf(text,sizeof(text),glass_tr("采样已停止 (%d)"),error?error:status.error);glass_resources_text(5,text);if(g_resources.timer)lv_timer_delete(g_resources.timer);g_resources.timer=NULL;glass_system_release(g_resources.monitor);g_resources.monitor=NULL;return;}
  if(status.ready)glass_resources_show(&status.sample);
}
static void glass_resources_page(void *unused)
{
  LV_UNUSED(unused);lv_obj_t *card=panel_card(glass_tr("系统性能"));if(!card)return;
  detail_page(card);g_resources.card=card;lv_obj_add_event_cb(card,glass_resources_closed,LV_EVENT_DELETE,NULL);
  glass_settings_sidebar(card,SETTINGS_RESOURCES);
  detail_text(card,glass_tr("系统性能"),216,26,400,28,detail_ink());detail_text(card,glass_tr("资源占用与运行趋势"),216,66,500,14,detail_muted());
  for(unsigned i=0;i<2;i++){
    lv_obj_t *tile=detail_tile(card,216+i*400,104,384,84);g_resources.tiles[i]=tile;glass_clickable(tile,resources_select,(void *)(uintptr_t)i);
    detail_text(tile,i?glass_tr("内存"):"CPU",16,12,160,14,detail_muted());g_resources.labels[i]=detail_text(tile,"--",16,38,160,28,detail_ink());
    g_resources.mini[i]=resources_chart(tile,204,24,160,40,i,&g_resources.mini_series[i]);
  }
  lv_obj_t *graph=detail_tile(card,216,204,784,220);g_resources.heading=detail_text(graph,"CPU",24,16,300,28,detail_ink());
  g_resources.labels[6]=detail_text(graph,"--",520,20,236,34,detail_ink());lv_obj_set_style_text_align(g_resources.labels[6],LV_TEXT_ALIGN_RIGHT,0);
  g_resources.labels[4]=detail_text(graph,"",24,60,650,12,detail_muted());
  g_resources.chart=resources_chart(graph,24,88,736,96,0,&g_resources.series);
  g_resources.empty=detail_text(graph,glass_tr("等待有效采样"),268,128,250,16,detail_muted());detail_text(graph,"100%",704,68,56,12,detail_muted());
  g_resources.axis=detail_text(graph,glass_tr("开始采样"),24,192,260,12,detail_muted());detail_text(graph,glass_tr("现在"),716,192,44,12,detail_muted());
  lv_obj_t *stats=detail_tile(card,216,440,784,102);const char *names[]={glass_tr("已用内存"),glass_tr("可用内存"),glass_tr("最大连续空闲"),glass_tr("堆使用峰值")};const unsigned fields[]={7,8,2,3};
  for(unsigned i=0;i<4;i++){detail_text(stats,names[i],24+i*194,18,174,12,detail_muted());g_resources.labels[fields[i]]=detail_text(stats,"--",24+i*194,48,174,20,detail_ink());if(i)glass_box(stats,12+i*194,24,1,54,detail_line(),0);}
  g_resources.labels[5]=detail_text(card,"",216,GLASS_CONTENT_HEIGHT-18,784,12,detail_muted());glass_resources_text(5,glass_tr("正在采样"));resources_present();
  g_resources.timer=lv_timer_create(glass_resources_update,1000,NULL);if(!g_resources.timer)glass_resources_text(5,glass_tr("采样启动失败：内存不足"));else glass_resources_update(NULL);
}
