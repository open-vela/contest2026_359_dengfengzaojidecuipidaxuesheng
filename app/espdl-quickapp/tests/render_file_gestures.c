/* Real LVGL pointer regression: a grid drag must never become a file tap. */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
static lv_indev_t *input;
static lv_point_t point;
static bool down;
static void pointer_read(lv_indev_t *i,lv_indev_data_t *data)
{(void)i;data->point=point;data->state=down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;}
static void sample(int x,int y,bool pressed)
{point=(lv_point_t){x,y};down=pressed;lv_tick_inc(20);lv_indev_read(input);}
static void settle(void)
{for(unsigned i=0;i<8;i++){lv_tick_inc(101);lv_timer_handler();}lv_obj_update_layout(g_desktop.screen);}
static void stroke_at(int x,int y,int dx,int dy,unsigned steps)
{
  sample(x,y,true);
  for(unsigned i=1;i<=steps;i++)sample(x+dx*(int)i/(int)steps,y+dy*(int)i/(int)steps,true);
  sample(x+dx,y+dy,false);settle();
}
static void stroke(int dx,int dy,unsigned steps)
{
  lv_obj_update_layout(g_desktop.screen);lv_area_t bounds;
  lv_obj_get_coords(g_files_browser.tiles[0].button,&bounds);
  stroke_at((bounds.x1+bounds.x2)/2,(bounds.y1+bounds.y2)/2,dx,dy,steps);
}
static void check_grid(unsigned page)
{
  assert(g_files_shell&&g_files_browser.card&&!g_files_pending);
  assert(!strcmp(g_files_path,GLASS_FILES_ROOT)&&g_files_page==page);
  assert(!g_files_auxiliary||lv_obj_has_flag(g_files_auxiliary,LV_OBJ_FLAG_HIDDEN));
}
static lv_display_t *review_pointer_init(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);size_t size=ftell(f);rewind(f);
  void *font=malloc(size);assert(font);assert(fread(font,1,size,f)==size);fclose(f);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_status_create();glass_edge_back_create();
  input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(input,pointer_read);
  return display;
}
#ifndef FILE_GESTURES_NO_MAIN
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  extern void review_files_pages_fixture(unsigned);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    review_files_pages_fixture(3);g_files_root=GLASS_FILES_ROOT;strcpy(g_files_path,GLASS_FILES_ROOT);g_files_page=0;
    glass_files_open(NULL);settle();check_grid(0);
    // Smaller drags and diagonal movement cancel tapping without paging.
    stroke(24,0,3);check_grid(0);stroke(0,24,3);check_grid(0);stroke(40,40,4);check_grid(0);
    stroke(-47,0,8);check_grid(0);stroke(-48,0,8);check_grid(1);stroke(48,0,8);check_grid(0);
    stroke(-100,0,2);check_grid(1);stroke(100,0,2);check_grid(0);
    stroke(0,-100,4);check_grid(1);stroke(0,100,4);check_grid(0);
    // A two-second drag also pages, instead of opening long-press details.
    stroke(-100,0,100);check_grid(1);stroke(100,0,100);check_grid(0);
    stroke(100,0,3);check_grid(0);
    stroke(-100,0,3);check_grid(1);stroke(-100,0,3);check_grid(2);stroke(-100,0,3);check_grid(2);
    stroke(100,0,3);check_grid(1);stroke(100,0,3);check_grid(0);
    // A drag starting in the gap between tiles remains owned by the grid.
    lv_area_t bounds;lv_obj_get_coords(g_files_browser.tiles[0].button,&bounds);
    stroke_at(bounds.x2+8,(bounds.y1+bounds.y2)/2,-100,0,8);check_grid(1);stroke(100,0,8);check_grid(0);
    for(unsigned n=0;n<20;n++){stroke(-100,0,5);check_grid(1);stroke(100,0,5);check_grid(0);}
    review_capture(display,"file-swipe-grid","滑动翻页，不打开文件");
    // Tap still enters the folder; the system edge returns to its parent.
    stroke(4,3,1);assert(!strcmp(g_files_path,GLASS_FILES_ROOT "/music"));
    stroke_at(8,300,100,0,3);check_grid(0);
    // Stationary long press retains explicit file management.
    lv_obj_get_coords(g_files_browser.tiles[0].button,&bounds);
    int x=(bounds.x1+bounds.x2)/2,y=(bounds.y1+bounds.y2)/2;
    sample(x,y,true);for(unsigned n=0;n<40;n++)sample(x,y,true);sample(x,y,false);settle();
    assert(g_files_auxiliary&&!lv_obj_has_flag(g_files_auxiliary,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(g_files_path,GLASS_FILES_ROOT));glass_files_cancel(NULL);settle();check_grid(0);
    stroke_at(512,592,0,-110,4);assert(!g_files_shell&&!g_desktop.current_card);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("FILE_GESTURES_OK: real pointer input; both themes; small/diagonal drags cancel taps; fast/slow four-direction paging; boundaries and gaps; 80 repeated page turns; jitter tap, stationary long press, parent edge and bottom home");
}
#endif
