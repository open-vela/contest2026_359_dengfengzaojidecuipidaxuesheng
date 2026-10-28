#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
static unsigned images;
static void inspect_images(lv_obj_t *root)
{
  if(lv_obj_check_type(root,&lv_image_class)){
    const lv_image_dsc_t *src=lv_image_get_src(root);lv_obj_t *host=lv_obj_get_parent(root);
    lv_area_t a,b;lv_obj_get_coords(root,&a);lv_obj_get_coords(host,&b);
    assert(src->header.w&&src->header.h);
    assert(a.x1>=b.x1&&a.y1>=b.y1&&a.x2<=b.x2&&a.y2<=b.y2);
    assert(lv_obj_get_scrollbar_mode(host)==LV_SCROLLBAR_MODE_OFF);
    assert(!lv_obj_has_flag(host,LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    lv_obj_t *pill=NULL;
    for(unsigned i=0;i<lv_obj_get_child_count(host);i++){
      lv_obj_t *child=lv_obj_get_child(host,i);
      if(lv_obj_get_style_radius(child,0)==13&&lv_obj_get_child_count(child)==1)pill=child;
    }
    assert(pill&&lv_obj_get_x(pill)==8&&lv_obj_get_y(pill)==8&&lv_obj_get_height(pill)==26);
    assert(lv_obj_get_width(pill)<lv_obj_get_width(host)-8&&lv_obj_get_style_shadow_width(pill,0)==0);
    assert(lv_obj_get_style_border_width(pill,0)==0&&lv_obj_get_style_bg_opa(pill,0)==LV_OPA_60);
    lv_area_t badge;lv_obj_get_coords(pill,&badge);
    int x=(a.x1+a.x2)/2,y=b.y1+18;assert(x>badge.x2+6);
    uint32_t color=glass_app_palette(g_desktop.light_theme).soft;
    uint16_t expected=((color>>8)&0xf800)|((color>>5)&0x07e0)|((color>>3)&31),pixel;
    if(y>=a.y1&&y<=a.y2)memcpy(&expected,src->data+(y-a.y1)*src->header.stride+(x-a.x1)*2,2);
    memcpy(&pixel,review_fb+(y*1024+x)*2,2);
    assert(pixel==expected);images++;
  }
  for(unsigned i=0;i<lv_obj_get_child_count(root);i++)inspect_images(lv_obj_get_child(root,i));
}
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);extern void review_album_fixture(int);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();review_album_fixture(1);launch_local_video(NULL);settle();settle();
    review_capture(display,"before-invalidate","相册半透明圆角格式徽标");images=0;inspect_images(g_desktop.panel);assert(images==5);
    lv_obj_invalidate(lv_screen_active());review_capture(display,"full-invalidate","完整重绘图像边界");
    images=0;inspect_images(g_desktop.panel);assert(images==5);panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("ALBUM_BOUNDS_OK: both themes; landscape/portrait bounds; translucent pills without border/shadow; original image area; pixels outside badges match source/background before/after full repaint; no scrollbars or overflow");
}
