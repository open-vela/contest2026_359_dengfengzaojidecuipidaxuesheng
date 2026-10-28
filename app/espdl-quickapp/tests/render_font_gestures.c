/* Real streamed fonts and LVGL input: cold parsing, warm text and lost samples. */
#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
#include <time.h>
#include <unistd.h>

static void choose_font(unsigned family)
{
  global_font_restore_checked=true;
  assert(!glass_lyric_font_request(family));global_font_requested=family;
  struct glass_font_cache_status initial;glass_lyric_font_cache_status(family,&initial);
  unsigned previous=initial.ready_sizes;
  for(unsigned n=0;n<15000;n++){
    glass_global_fonts_poll();
    struct glass_font_cache_status cache;glass_lyric_font_cache_status(family,&cache);
    /* Parsing one font size at a time leaves an event-loop turn for touch. */
    assert(cache.ready_sizes<=previous+1);previous=cache.ready_sizes;
    if(global_font_selected==family){assert(cache.ready_sizes==12);return;}
    usleep(1000);
  }
  fprintf(stderr,"FONT_LOAD_FAILURE family=%u state=%d selected=%u requested=%u sizes=%u\n",family,glass_lyric_font_state(family),global_font_selected,global_font_requested,previous);
  assert(!"global font did not become ready");
}
static void release_swipe(int x,int y,int dx,int dy)
{
  sample(x,y,true);
  /* A busy display can deliver only down and the final released point. */
  sample(x+dx,y+dy,false);
  assert(g_edge_back_pending);settle();assert(!g_edge_back_pending);
}
static uint64_t microseconds(void)
{
  struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
  return (uint64_t)now.tv_sec*1000000+now.tv_nsec/1000;
}
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  glass_global_fonts_init();glass_sd_font_start();choose_font(4);
  const lv_font_t *published=zh_font(20);
  glass_settings(NULL);settle();
  lv_obj_t *text=lv_label_create(g_desktop.current_card);
  char content[1024];unsigned length=0;
  for(unsigned c=33;c<127;c++)content[length++]=(char)c;
  snprintf(content+length,sizeof(content)-length,"\n全局字体桌面应用歌词音乐照片视频相册设置语言切换返回滑动性能缓存资源加载连接设备控制扬声器亮度网络天气蓝牙录音拍摄文件管理显示存储页面内容文字消息系统用户选择恢复取消确认关闭启动暂停播放进度时间状态结果刷新中文英文默认普通预览清晰流畅稳定测试验证成功错误处理完成当前可用服务信息详情帮助安全保存查询搜索收藏传输路径容量更新大小方向边缘触摸按钮识别窗口位置尺寸列表图像颜色模型温度湿度空气质量今日明天星期年月日小时分钟秒");
  lv_label_set_text(text,content);lv_obj_set_width(text,780);lv_obj_set_pos(text,220,80);
  lv_obj_set_style_text_font(text,published,0);lv_obj_set_style_text_color(text,lv_color_hex(0xffffff),0);
  for(unsigned n=0;n<3;n++){lv_obj_invalidate(g_desktop.current_card);lv_refr_now(display);}
  struct glass_font_cache_status before,after;glass_lyric_font_cache_status(4,&before);
  uint64_t begin=microseconds();
  for(unsigned n=0;n<12;n++){lv_obj_invalidate(g_desktop.current_card);lv_refr_now(display);}
  uint64_t elapsed=microseconds()-begin;glass_lyric_font_cache_status(4,&after);
  assert(before.preloaded_bytes==1444224&&after.reads==before.reads&&after.page_misses==before.page_misses);
  assert(after.glyph_cached>96&&after.glyph_cached<=after.glyph_capacity);
  printf("FONT_WARM_PAGE_OK glyphs=%u capacity=%u preload=%u reads=%llu misses=%llu redraws=12 elapsed_us=%llu\n",
    after.glyph_cached,after.glyph_capacity,after.preloaded_bytes,(unsigned long long)after.reads,
    (unsigned long long)after.page_misses,(unsigned long long)elapsed);
  fflush(stdout);choose_font(1);glass_lyric_font_cache_status(4,&after);
  assert(after.glyph_capacity==64&&after.glyph_cached<=64&&zh_font(20)==published);
  choose_font(4);assert(zh_font(20)==published);
  g_motion_reduced=false;g_glass.reduced_motion=false;
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();glass_settings(NULL);settle();
    about_clicked(NULL);settle();release_swipe(8,300,64,0);
    assert(g_desktop.current_card==g_control_card);
    about_clicked(NULL);settle();release_swipe(24,300,60,25);
    assert(g_desktop.current_card==g_control_card);
    about_clicked(NULL);settle();release_swipe(1000,300,-60,25);
    assert(g_desktop.current_card==g_control_card);
    about_clicked(NULL);settle();lv_obj_t *owner=g_desktop.current_card;
    int original=lv_obj_get_style_translate_x(owner,0);
    sample(24,300,true);sample(55,308,true);
    assert(!g_edge_back_pending&&lv_obj_get_style_translate_x(owner,0)==original);
    sample(55,308,false);settle();assert(g_desktop.current_card==owner);
    sample(32,300,true);sample(95,300,false);settle();assert(g_desktop.current_card==owner);
    release_swipe(24,300,60,0);assert(g_desktop.current_card==g_control_card);
    /* A released-only commit must not consume the next pointer sequence. */
    release_swipe(24,300,60,0);assert(!g_desktop.current_card);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("FONT_EDGE_OK: real selected SD face; one-size parsing; bounded inactive caches; both themes; 28px edges; 60px diagonal/released-only swipes; no page translation; short/interior rejection; consecutive back");
  return 0;
}
