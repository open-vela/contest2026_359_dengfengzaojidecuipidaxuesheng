/* Real LVGL objects, pointer dispatch and synthetic offline media. */
#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
extern void review_album_fixture(int);
extern const char *review_album_decode_path(void);
static lv_obj_t *find_pager(lv_obj_t *root)
{
  if(!root||!lv_obj_is_visible(root))return NULL;
  if(lv_obj_get_user_data(root)&&lv_obj_get_height(root)==40&&lv_obj_get_width(root)<=136)return root;
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++){
    lv_obj_t *pager=find_pager(lv_obj_get_child(root,i));if(pager)return pager;
  }
  return NULL;
}
static void choose_page(unsigned ordinal)
{
  lv_obj_t *root=find_pager(g_desktop.panel);assert(root);
  struct glass_pager *pager=lv_obj_get_user_data(root);
  for(unsigned i=0;i<GLASS_PAGER_SLOTS;i++)
    if(pager->slots[i].hit&&pager->slots[i].page==ordinal){
      lv_obj_send_event(pager->slots[i].hit,LV_EVENT_CLICKED,NULL);settle();return;
    }
  assert(false);
}
static void check_exit(lv_display_t *display,lv_obj_t *layer,const char *capture)
{
  settle();lv_obj_t *exit=review_find(layer,"退出全屏");assert(exit);
  lv_area_t bounds;lv_obj_get_coords(exit,&bounds);
  assert(bounds.y1>=GLASS_STATUS_HEIGHT&&bounds.x2<1024&&bounds.y2<600);
  assert(lv_obj_is_visible(exit)&&lv_obj_get_width(exit)==132&&lv_obj_get_height(exit)==44);
  review_capture(display,capture,"相册全屏 · 常驻退出按钮");
  uint16_t pixel;memcpy(&pixel,review_fb+((bounds.y1+22)*1024+bounds.x1+8)*2,2);
  uint32_t color=0x252832;
  assert(pixel==(((color>>8)&0xf800)|((color>>5)&0x07e0)|((color>>3)&31)));
  int x=(bounds.x1+bounds.x2)/2,y=(bounds.y1+bounds.y2)/2;
  sample(x,y,true);sample(x,y,false);settle();
  assert(!review_find(layer,"退出全屏"));
}
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  static uint16_t frame[640*360];
  for(unsigned i=0;i<640*360;i++)frame[i]=0x07ff;
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();review_album_fixture(1);launch_local_video(NULL);settle();
    review_click("旅行");settle();review_click("照片");settle();
    assert(review_find(g_desktop.panel,"所有照片 · 包含子文件夹"));
    assert(!review_find(g_desktop.panel,"相机"));
    review_capture(display,"library-photos","所有照片 · 汇总子文件夹");
    review_click("旅行/子目录照片.jpg");settle();
    assert(!strcmp(review_album_decode_path(),"/sdcard/旅行/子目录照片.jpg"));
    review_click("全屏");settle();check_exit(display,lv_layer_top(),"library-photo-fullscreen");
    assert(review_find(g_desktop.panel,"子目录照片.jpg"));
    review_click("全屏");settle();assert(local_album_back(g_desktop.current_card));settle();
    assert(!review_find(lv_layer_top(),"退出全屏"));
    review_click("返回相册");settle();
    review_click("视频");settle();assert(review_find(g_desktop.panel,"所有视频 · 包含子文件夹"));
    review_capture(display,"library-videos","所有视频 · 汇总子文件夹");
    review_click("海边短片.mp4");settle();choose_page(1);
    assert(!strcmp(review_video_path,"/sdcard/旅行/子目录视频.mp4"));
    choose_page(0);assert(!strcmp(review_video_path,"/sdcard/海边短片.mp4"));
    memset(&picture,0,sizeof(picture));picture.header.magic=LV_IMAGE_HEADER_MAGIC;
    picture.header.cf=LV_COLOR_FORMAT_RGB565;picture.header.w=640;picture.header.h=360;
    picture.header.stride=1280;picture.data_size=sizeof(frame);picture.data=(const uint8_t *)frame;
    picture_layout();review_click("全屏");settle();
    /* Repeated frame invalidation must retain the exit above the video image. */
    for(unsigned i=0;i<4;i++){picture_layout();lv_obj_invalidate(image);lv_refr_now(display);}
    check_exit(display,lv_screen_active(),"library-video-fullscreen");assert(page&&!fullscreen);
    review_click("全屏");settle();assert(local_album_back(g_desktop.current_card));settle();assert(!fullscreen&&page);
    review_click("返回相册");settle();review_click("全部");settle();
    /* Returning to folders retains the directory used before the library. */
    review_click("上一级");settle();assert(review_find(g_desktop.panel,"旅行"));
    review_click("照片");settle();review_click("晨光.jpg");settle();review_click("全屏");settle();
    panel_hide(NULL);settle();assert(!review_find(lv_layer_top(),"退出全屏"));
    for(unsigned i=0;i<6;i++){
      launch_local_video(NULL);settle();review_click("视频");settle();review_click("海边短片.mp4");settle();
      review_click("全屏");settle();panel_hide(NULL);settle();assert(!fullscreen);
    }
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("ALBUM_LIBRARY_UI_OK: both themes; library from subfolder; nested photo decode/video paths; video paging; real pointer exits; bright-frame repaint keeps exit pixels; system back closes fullscreen first; directory restored; fullscreen teardown/reopen");
}
