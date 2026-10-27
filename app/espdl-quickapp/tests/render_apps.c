/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main

static unsigned count_objects(lv_obj_t *o)
{unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(o);i++)n+=count_objects(lv_obj_get_child(o,i));return n;}
static void check_tiles(unsigned visible)
{
  lv_obj_update_layout(g_apps_card);
  lv_area_t grid;lv_obj_get_coords(g_apps_grid,&grid);
  for(unsigned i=0;i<9;i++){
    struct glass_app_slot *s=&g_apps_slots[i];
    if(i>=visible){assert(!s->tile||lv_obj_has_flag(s->tile,LV_OBJ_FLAG_HIDDEN));continue;}
    assert(s->tile&&lv_obj_is_visible(s->tile));
    lv_area_t tile,name,source;lv_obj_get_coords(s->tile,&tile);
    lv_obj_get_coords(s->name,&name);lv_obj_get_coords(s->source,&source);
    assert(tile.x1>=grid.x1&&tile.x2<=grid.x2&&tile.y1>=grid.y1&&tile.y2<=grid.y2);
    assert(name.x1>=tile.x1&&name.x2<tile.x2&&name.y2<source.y1);
    assert(lv_obj_get_x(s->tile)==(int)(i%3)*316&&lv_obj_get_y(s->tile)==(int)(i/3)*116);
    if(lv_obj_is_visible(s->remove)){
      lv_area_t remove;lv_obj_get_coords(s->remove,&remove);
      assert(source.x2<remove.x1&&remove.x2<tile.x2&&remove.y2<tile.y2);
      assert(lv_obj_get_height(s->remove)>=44);
    }
  }
}
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void pointer_read(lv_indev_t *input,lv_indev_data_t *data)
{LV_UNUSED(input);data->point=pointer_point;data->state=pointer_state;}
static void pointer_move(lv_indev_t *input,int x,int y,bool pressed)
{
  pointer_point=(lv_point_t){x,y};pointer_state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
  lv_obj_update_layout(lv_layer_top());lv_indev_read(input);
}
static void swipe(lv_indev_t *input,lv_obj_t *from,int dx,int dy)
{
  lv_obj_update_layout(g_desktop.screen);
  lv_area_t a;lv_obj_get_coords(from,&a);int x=(a.x1+a.x2)/2,y=(a.y1+a.y2)/2;
  pointer_move(input,x,y,true);
  for(int i=1;i<=4;i++)pointer_move(input,x+dx*i/4,y+dy*i/4,true);
  pointer_move(input,x+dx,y+dy,false);
}
static void check_swipes(lv_indev_t *input)
{
  glass_apps_page(NULL,0);lv_obj_t *card=g_apps_card;
  swipe(input,g_apps_slots[0].badge,-120,0);assert(g_apps_page_index==1&&g_apps_card==card);
  swipe(input,g_apps_slots[0].tile,-120,0);assert(g_apps_page_index==1&&g_apps_card==card);
  swipe(input,g_apps_slots[0].name,120,0);assert(g_apps_page_index==0&&g_apps_card==card);
  swipe(input,g_apps_slots[0].tile,120,0);assert(g_apps_page_index==0&&g_apps_card==card);
  swipe(input,g_apps_slots[0].tile,0,120);assert(g_apps_page_index==0&&g_apps_card==card);
  /* Empty space and pager dots use the same gesture owner. */
  pointer_move(input,800,130,true);pointer_move(input,740,130,true);
  pointer_move(input,400,130,true);pointer_move(input,400,130,false);
  assert(g_apps_page_index==1&&g_apps_card==card);
  swipe(input,g_apps_pager,120,0);assert(g_apps_page_index==0&&g_apps_card==card);
  /* A new short tap still launches the selected application after a swipe. */
  swipe(input,g_apps_slots[4].tile,0,0);assert(g_music_ui.root==g_desktop.current_card);
  panel_hide(NULL);glass_apps(NULL);assert(g_apps_page_index==0);
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font);assert(fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_status_create();
  glass_edge_back_create();
  lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(input,display);lv_indev_set_read_cb(input,pointer_read);

  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme==0;glass_rebuild();glass_apps(NULL);
    check_tiles(9);review_capture(display,"apps-first","全部应用 · 首页");
    glass_apps_page(NULL,1);check_tiles(4);review_capture(display,"apps-last","全部应用 · 末页");
    lv_obj_t *card=g_apps_card,*pager=g_apps_pager;unsigned warm=count_objects(card);
    for(unsigned page=0;page<100;page++){
      glass_apps_page(NULL,page%2);assert(g_apps_card==card&&g_apps_pager==pager);
      assert(count_objects(card)==warm);
    }
    check_swipes(input);
    g_desktop.qpk=realloc(g_desktop.qpk,6*sizeof(*g_desktop.qpk));assert(g_desktop.qpk);
    g_desktop.qpk_capacity=6;memset(g_desktop.qpk,0,6*sizeof(*g_desktop.qpk));
    for(unsigned i=0;i<6;i++){
      strcpy(g_desktop.qpk[i].package,"review.fixture");
      strcpy(g_desktop.qpk[i].dir,"/offline-fixtures/qpk/review.fixture");
      snprintf(g_desktop.qpk[i].name,sizeof(g_desktop.qpk[i].name),"应用名称超长显示测试 ABCDEFGHIJKLMNOPQRSTUVWXYZ");
      g_desktop.qpk[i].volume=i%2?QPK_VOLUME_SD:QPK_VOLUME_INTERNAL;
    }
    g_desktop.nqpk=6;glass_apps_page(NULL,2);check_tiles(1);
    review_capture(display,"apps-one","全部应用 · 单个长名称应用");
    glass_apps_page(NULL,1);check_tiles(9);
    review_capture(display,"apps-installed","全部应用 · 安装应用与删除按钮");
    swipe(input,g_apps_slots[4].remove,-120,0);
    assert(g_apps_page_index==2&&!g_desktop.dialog&&g_desktop.apps_page);
    glass_apps_page(NULL,1);
    assert(lv_obj_is_visible(g_apps_slots[4].remove));
    lv_obj_send_event(g_apps_slots[4].remove,LV_EVENT_CLICKED,NULL);
    assert(g_desktop.dialog);review_capture(display,"apps-delete","删除应用确认");dialog_cancel();
    g_apps_remove_timer=lv_timer_create(glass_apps_remove_poll,60000,NULL);assert(g_apps_remove_timer);
    swipe(input,g_apps_slots[0].tile,-120,0);assert(g_apps_page_index==1);
    lv_timer_delete(g_apps_remove_timer);g_apps_remove_timer=NULL;
    /* A held remove button must not act after rebinding to a different app. */
    lv_obj_send_event(g_apps_slots[4].remove,LV_EVENT_PRESSED,NULL);
    glass_apps_page(NULL,0);
    lv_obj_send_event(g_apps_slots[4].remove,LV_EVENT_CLICKED,NULL);assert(!g_desktop.dialog);
    glass_apps_page(NULL,2);g_desktop.nqpk=0;glass_apps_render();
    assert(g_apps_page_index==1);check_tiles(4);
    /* Edge strips still close the page instead of changing its selection. */
    pointer_move(input,5,300,true);pointer_move(input,100,300,true);pointer_move(input,100,300,false);
    lv_timer_handler();assert(!g_apps_card&&!g_apps_pager);
  }
  lv_indev_delete(input);
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("APPS_UI_OK: both themes; fixed grid; 1/4/9 items; long names; remove bounds/confirmation; stale press; page clamp; 200 pooled updates");
  puts("APPS_SWIPE_OK: real pointer; badges/names/tiles/background/pager/delete; first/last bounds; one page per stroke; vertical ignored; tap launch; removal busy; edge back; both themes");
  return 0;
}
