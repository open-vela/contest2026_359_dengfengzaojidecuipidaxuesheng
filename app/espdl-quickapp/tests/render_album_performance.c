/* Cache behavior verified through real sidebar callbacks and LVGL lifetimes. */
#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
extern void review_album_fixture(int);
extern unsigned review_album_scan_calls(void);
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();review_album_fixture(1);launch_local_video(NULL);settle();
    unsigned count=review_album_scan_calls();
    review_click("照片");settle();assert(review_album_scan_calls()==++count);
    review_click("视频");settle();assert(review_album_scan_calls()==++count);
    for(unsigned i=0;i<20;i++){
      review_click("照片");settle();review_click("照片");settle();
      review_click("晨光.jpg");settle();review_click("返回相册");settle();
      review_click("视频");settle();review_click("视频");settle();
      assert(review_album_scan_calls()==count);
    }
    review_capture(display,"album-cache","照片与视频索引缓存 · 切换不重扫");
    review_click("刷新");settle();assert(review_album_scan_calls()==++count);
    review_click("照片");settle();assert(review_album_scan_calls()==++count);
    review_click("本机");settle();assert(review_album_scan_calls()==++count);
    review_click("视频");settle();assert(review_album_scan_calls()==++count);
    review_click("照片");settle();assert(review_album_scan_calls()==count);
    review_click("SD 卡");settle();assert(review_album_scan_calls()==++count);
    review_click("视频");settle();assert(review_album_scan_calls()==++count);
    review_album_fixture(3);review_click("刷新");settle();assert(review_album_scan_calls()==++count);
    review_click("视频");settle();assert(review_album_scan_calls()==++count);
    review_album_fixture(1);review_click("视频");settle();assert(review_album_scan_calls()==++count);
    review_click("照片");settle();assert(review_album_scan_calls()==++count);
    panel_hide(NULL);settle();launch_local_video(NULL);settle();assert(review_album_scan_calls()==++count);
    review_click("照片");settle();assert(review_album_scan_calls()==++count);
    panel_hide(NULL);settle();
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("ALBUM_CACHE_UI_OK: two themes; 40 photo/video roundtrips and repeated same-tab taps cause zero rescans; preview return reuses index; refresh/medium switch/error/reopen invalidate caches; sanitizers check shared ownership teardown");
}
