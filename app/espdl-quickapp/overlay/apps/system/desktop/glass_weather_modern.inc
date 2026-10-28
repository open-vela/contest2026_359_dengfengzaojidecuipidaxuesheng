/* SPDX-License-Identifier: Apache-2.0 */
#define WEATHER_FROST_X 28
#define WEATHER_FROST_Y 358
#define WEATHER_FROST_WIDTH 968
#define WEATHER_FROST_HEIGHT 150
#define WEATHER_FROST_COLS 160
#define WEATHER_FROST_ROWS 26
static struct {
  lv_obj_t *root,*location,*summary,*aqi,*category,*temperature,*humidity,*pm25;
  lv_obj_t *report,*status,*unit,*data_state,*state_dot,*humidity_hint,*metrics;
  struct weather_scene scene;
  lv_image_dsc_t frost;
  uint8_t *frost_pixels;
  unsigned frost_asset,frost_builds;
  unsigned revision;
  struct glass_weather_data *data;
  enum glass_weather_state state;
  bool stale,valid,data_valid,night;
} g_weather_detail;

/* A retained home stays ready for back gestures beneath any opaque full page. */
static bool glass_page_covers_home(void)
{
  lv_obj_t *root=g_desktop.current_card;
  return root&&lv_obj_is_visible(root)&&
    lv_obj_get_style_x(root,0)==0&&lv_obj_get_style_y(root,0)==0&&
    lv_obj_get_style_width(root,0)==1024&&lv_obj_get_style_height(root,0)==GLASS_CONTENT_HEIGHT&&
    lv_obj_get_style_translate_x(root,0)==0&&lv_obj_get_style_translate_y(root,0)==0&&
    lv_obj_get_style_opa(root,0)==LV_OPA_COVER&&lv_obj_get_style_bg_opa(root,0)==LV_OPA_COVER&&
    lv_obj_get_style_radius(root,0)==0;
}

static void weather_frost_release(void)
{
  if(g_weather_detail.frost_pixels)lv_image_cache_drop(&g_weather_detail.frost);
  free(g_weather_detail.frost_pixels);g_weather_detail.frost_pixels=NULL;
  memset(&g_weather_detail.frost,0,sizeof(g_weather_detail.frost));
}

/* Two small separable box passes approximate a soft backdrop blur. Keep RGB888
 * until LVGL's final RGB565 blend so averaging does not compound quantization. */
static void weather_frost_blur(uint8_t *pixels,bool horizontal)
{
  uint8_t line[WEATHER_FROST_COLS*3];
  int length=horizontal?WEATHER_FROST_COLS:WEATHER_FROST_ROWS;
  int lines=horizontal?WEATHER_FROST_ROWS:WEATHER_FROST_COLS;
  int stride=horizontal?3:WEATHER_FROST_COLS*3;
  int step=horizontal?WEATHER_FROST_COLS*3:3;
  for(int n=0;n<lines;n++){
    uint8_t *dst=pixels+n*step;
    for(int x=0;x<length;x++)memcpy(line+x*3,dst+x*stride,3);
    unsigned sum[3]={0};
    for(int k=-4;k<=4;k++){
      int x=k<0?0:k;
      for(unsigned c=0;c<3;c++)sum[c]+=line[x*3+c];
    }
    for(int x=0;x<length;x++){
      for(unsigned c=0;c<3;c++)dst[x*stride+c]=(sum[c]+4)/9;
      int leave=x<4?0:x-4,enter=x+5<length?x+5:length-1;
      for(unsigned c=0;c<3;c++)sum[c]=sum[c]+line[enter*3+c]-line[leave*3+c];
    }
  }
}

static void weather_frost_update(void)
{
  const struct weather_scene *s=&g_weather_detail.scene;
  if(!s->pixels){weather_frost_release();return;}
  if(g_weather_detail.frost_pixels&&g_weather_detail.frost_asset==s->asset)return;
  size_t count_pixels=WEATHER_FROST_COLS*WEATHER_FROST_ROWS,bytes=count_pixels*4;
  uint8_t *pixels=g_weather_detail.frost_pixels;
  if(pixels)lv_image_cache_drop(&g_weather_detail.frost);
  else pixels=malloc(bytes);
  if(!pixels)return;
  int iw=s->image.header.w,ih=s->image.header.h;
  /* Average the matching native sky crop, before the page tint, at 1/6 scale. */
  for(int y=0;y<WEATHER_FROST_ROWS;y++)for(int x=0;x<WEATHER_FROST_COLS;x++){
    int x0=(WEATHER_FROST_X*WEATHER_FROST_COLS+x*WEATHER_FROST_WIDTH)*iw/(WEATHER_FROST_COLS*1024);
    int x1=(WEATHER_FROST_X*WEATHER_FROST_COLS+(x+1)*WEATHER_FROST_WIDTH)*iw/(WEATHER_FROST_COLS*1024);
    int y0=(WEATHER_FROST_Y*WEATHER_FROST_ROWS+y*WEATHER_FROST_HEIGHT)*ih/(WEATHER_FROST_ROWS*GLASS_CONTENT_HEIGHT);
    int y1=(WEATHER_FROST_Y*WEATHER_FROST_ROWS+(y+1)*WEATHER_FROST_HEIGHT)*ih/(WEATHER_FROST_ROWS*GLASS_CONTENT_HEIGHT);
    unsigned sum[3]={0},count=0;
    for(int sy=y0;sy<y1;sy++)for(int sx=x0;sx<x1;sx++){
      uint16_t p=s->pixels[sy*iw+sx];
      /* LV_COLOR_FORMAT_RGB888 stores bytes in B,G,R order. */
      sum[0]+=(p&31)*255/31;sum[1]+=((p>>5)&63)*255/63;sum[2]+=((p>>11)&31)*255/31;count++;
    }
    uint8_t *p=pixels+(y*WEATHER_FROST_COLS+x)*3;
    for(unsigned c=0;c<3;c++)p[c]=sum[c]/count;
  }
  for(unsigned pass=0;pass<2;pass++){
    weather_frost_blur(pixels,true);weather_frost_blur(pixels,false);
  }
  /* The software image renderer does not implement clip_radius. An alpha
   * contour in the tiny cache gives soft rounded corners without a full-size
   * clipping layer. Expand backwards so the RGB blur can share this buffer. */
  for(int i=(int)count_pixels-1;i>=0;i--){
    int x=i%WEATHER_FROST_COLS,y=i/WEATHER_FROST_COLS;
    uint8_t b=pixels[i*3],g=pixels[i*3+1],r=pixels[i*3+2];
    bool inner_x=x*WEATHER_FROST_WIDTH>=22*WEATHER_FROST_COLS&&
      (x+1)*WEATHER_FROST_WIDTH<=(WEATHER_FROST_WIDTH-23)*WEATHER_FROST_COLS;
    bool inner_y=y*WEATHER_FROST_HEIGHT>=22*WEATHER_FROST_ROWS&&
      (y+1)*WEATHER_FROST_HEIGHT<=(WEATHER_FROST_HEIGHT-23)*WEATHER_FROST_ROWS;
    unsigned inside=inner_x||inner_y?16:0;
    if(!inside)for(int sy=0;sy<4;sy++)for(int sx=0;sx<4;sx++){
      int px=(x*8+sx*2+1)*WEATHER_FROST_WIDTH/(WEATHER_FROST_COLS*8);
      int py=(y*8+sy*2+1)*WEATHER_FROST_HEIGHT/(WEATHER_FROST_ROWS*8);
      int dx=px<22?22-px:px>WEATHER_FROST_WIDTH-23?px-(WEATHER_FROST_WIDTH-23):0;
      int dy=py<22?22-py:py>WEATHER_FROST_HEIGHT-23?py-(WEATHER_FROST_HEIGHT-23):0;
      if(dx*dx+dy*dy<=22*22)inside++;
    }
    pixels[i*4]=b;pixels[i*4+1]=g;pixels[i*4+2]=r;pixels[i*4+3]=(inside*255+8)/16;
  }
  g_weather_detail.frost_pixels=pixels;g_weather_detail.frost_asset=s->asset;g_weather_detail.frost_builds++;
  lv_image_dsc_t *image=&g_weather_detail.frost;
  image->header.magic=LV_IMAGE_HEADER_MAGIC;image->header.cf=LV_COLOR_FORMAT_ARGB8888;
  image->header.w=WEATHER_FROST_COLS;image->header.h=WEATHER_FROST_ROWS;
  image->header.stride=WEATHER_FROST_COLS*4;image->data_size=bytes;image->data=pixels;
  if(g_weather_detail.metrics)lv_obj_invalidate(g_weather_detail.metrics);
}

static void weather_frost_draw(lv_event_t *e)
{
  lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
  lv_layer_t *layer=lv_event_get_layer(e);
  if(g_weather_detail.frost_pixels){
    lv_draw_image_dsc_t image;lv_draw_image_dsc_init(&image);image.src=&g_weather_detail.frost;
    if(weather_scene_night_tint(&g_weather_detail.scene)){
      image.recolor=lv_color_hex(0x102b54);image.recolor_opa=75;
    }
    image.scale_x=(WEATHER_FROST_WIDTH*256+WEATHER_FROST_COLS-1)/WEATHER_FROST_COLS;
    image.scale_y=(WEATHER_FROST_HEIGHT*256+WEATHER_FROST_ROWS-1)/WEATHER_FROST_ROWS;
    image.pivot=(lv_point_t){0,0};image.antialias=1;
    lv_area_t source=a;source.x2=source.x1+WEATHER_FROST_COLS-1;source.y2=source.y1+WEATHER_FROST_ROWS-1;
    lv_draw_image(layer,&image,&source);
  }
  lv_draw_rect_dsc_t glass;lv_draw_rect_dsc_init(&glass);
  glass.radius=22;glass.bg_color=lv_color_hex(0x163044);
  glass.bg_opa=weather_scene_night_tint(&g_weather_detail.scene)?135:100;
  glass.border_width=1;glass.border_color=lv_color_hex(0xf1f8ff);glass.border_opa=38;
  lv_draw_rect(layer,&glass,&a);
}

static void glass_weather_detail_deleted(lv_event_t *e)
{
  LV_UNUSED(e);weather_frost_release();weather_scene_release(&g_weather_detail.scene);
  free(g_weather_detail.data);
  memset(&g_weather_detail,0,sizeof(g_weather_detail));
}

/* Lock-screen weather retains the shared compact outline glyph. */
static const char *weather_detail_icon(const char *condition)
{
  if(strstr(condition,"雪"))return GLASS_ICON_CLOUD_SNOW;
  if(strstr(condition,"雨")||strstr(condition,"雷")||strstr(condition,"雹"))return GLASS_ICON_CLOUD_RAIN;
  if(strstr(condition,"阴")||strstr(condition,"雾")||strstr(condition,"霾")||
     strstr(condition,"沙")||strstr(condition,"尘"))return GLASS_ICON_CLOUD;
  if(strstr(condition,"晴")&&!strstr(condition,"云"))return GLASS_ICON_SUN;
  return GLASS_ICON_CLOUD_SUN;
}

static const uint32_t weather_air_colors[]={
  0xd7e6f1,0xa7e2c1,0xe9dd9c,0xf5c18d,0xf0a3a8,0xd7afe8,0xe6a0bd
};

static void glass_weather_detail_update(const struct glass_weather_snapshot *weather,bool stale)
{
  if(!glass_ui_can_paint(g_weather_detail.location))return;
  bool night=weather_is_night();
  bool first=!g_weather_detail.valid;
  bool same_data=!weather->valid||(g_weather_detail.data&&
    weather_values_equal(g_weather_detail.data,&weather->data)&&
    !strcmp(g_weather_detail.data->report_time,weather->data.report_time));
  if(g_weather_detail.valid&&(g_weather_detail.revision==weather->revision||same_data)&&
     g_weather_detail.stale==stale&&g_weather_detail.state==weather->state&&
     g_weather_detail.data_valid==weather->valid&&g_weather_detail.night==night){
    g_weather_detail.revision=weather->revision;return;
  }
  bool air_changed=!g_weather_detail.valid||!g_weather_detail.data||
    g_weather_detail.data_valid!=weather->valid||
    g_weather_detail.data->level!=weather->data.level||g_weather_detail.data->aqi!=weather->data.aqi;
  bool dot_changed=!g_weather_detail.valid||g_weather_detail.data_valid!=weather->valid||
    g_weather_detail.stale!=stale||g_weather_detail.state!=weather->state;
  g_weather_detail.revision=weather->revision;g_weather_detail.state=weather->state;
  g_weather_detail.stale=stale;g_weather_detail.valid=true;g_weather_detail.data_valid=weather->valid;
  g_weather_detail.night=night;
  weather_scene_update(&g_weather_detail.scene,weather,night);
  weather_frost_update();
  const struct glass_weather_data *data=&weather->data;
  weather_text(g_weather_detail.location,weather->valid&&data->location[0]?data->location:glass_tr("当地天气"));
  weather_text(g_weather_detail.summary,weather->valid&&data->condition[0]?glass_tr(data->condition):glass_tr("天气暂不可用"));
  char temperature[12];
  if(weather->valid&&data->temperature!=-999)snprintf(temperature,sizeof(temperature),"%d",data->temperature);
  else strcpy(temperature,"--");
  if(weather_text(g_weather_detail.temperature,temperature)||first){
    const lv_font_t *font=lv_obj_get_style_text_font(g_weather_detail.temperature,0);
    int width=lv_text_get_width(temperature,strlen(temperature),font,0);
    lv_obj_set_x(g_weather_detail.unit,32+width+8);
  }

  bool air_valid=weather->valid&&data->aqi>=0;
  unsigned level=air_valid&&data->level>=1&&data->level<=6?(unsigned)data->level:0;
  char buffer[96];
  if(air_valid)snprintf(buffer,sizeof(buffer),"%d",data->aqi);else strcpy(buffer,"--");
  weather_text(g_weather_detail.aqi,buffer);
  weather_text(g_weather_detail.category,air_valid&&data->category[0]?glass_tr(data->category):glass_tr("暂无数据"));
  if(air_changed)lv_obj_set_style_text_color(g_weather_detail.category,lv_color_hex(weather_air_colors[level]),0);
  if(weather->valid&&data->humidity>=0)snprintf(buffer,sizeof(buffer),"%d%%",data->humidity);else strcpy(buffer,"--%");
  weather_text(g_weather_detail.humidity,buffer);
  weather_text(g_weather_detail.humidity_hint,
    !weather->valid||data->humidity<0?glass_tr("暂无湿度数据"):data->humidity<30?glass_tr("偏干燥"):data->humidity>60?glass_tr("偏潮湿"):glass_tr("舒适"));
  if(weather->valid&&data->pm25>=0)snprintf(buffer,sizeof(buffer),"%d",data->pm25);else strcpy(buffer,"--");
  weather_text(g_weather_detail.pm25,buffer);
  if(weather->valid)snprintf(buffer,sizeof(buffer),glass_tr("报告时间 · %s"),data->report_time[0]?data->report_time:glass_tr("未提供"));
  else strcpy(buffer,glass_tr("等待首次天气报告"));
  weather_text(g_weather_detail.report,buffer);
  weather_text(g_weather_detail.status,weather->state==WEATHER_READY&&!stale&&weather->valid?
    glass_tr("按网络位置定位 · 每小时自动更新"):weather_status(weather,stale));
  const char *state=weather->state==WEATHER_FETCHING?glass_tr("更新中"):weather->state==WEATHER_WAIT_NET?(weather->valid?glass_tr("离线缓存"):glass_tr("等待联网")):
    weather->state==WEATHER_WAIT_TIME?glass_tr("等待校时"):weather->state==WEATHER_FAILED?glass_tr("更新失败"):
    stale?glass_tr("待更新"):weather->valid?glass_tr("已更新"):glass_tr("暂无数据");
  weather_text(g_weather_detail.data_state,state);
  if(dot_changed)lv_obj_set_style_bg_color(g_weather_detail.state_dot,
    lv_color_hex(weather->valid&&weather->state==WEATHER_READY&&!stale?0xa7e2c1:0xf1d29f),0);
  if(g_weather_detail.data)*g_weather_detail.data=weather->data;
}

static void glass_weather_refresh_clicked(lv_event_t *e)
{
  LV_UNUSED(e);glass_weather_refresh();
  if(g_weather_detail.status)lv_label_set_text(g_weather_detail.status,glass_tr("已请求刷新 · 距上次请求至少一分钟"));
}

static lv_obj_t *weather_page_button(lv_obj_t *parent,const char *text,
                                    int x,int y,int w,lv_event_cb_t callback)
{
  lv_obj_t *button=detail_button(parent,text,x,y,w,callback,NULL);
  lv_obj_set_style_radius(button,22,0);
  lv_obj_set_style_bg_color(button,lv_color_hex(0x102b40),0);
  lv_obj_set_style_bg_opa(button,150,0);
  lv_obj_set_style_bg_opa(button,210,LV_STATE_PRESSED);
  lv_obj_set_style_border_width(button,1,0);
  lv_obj_set_style_border_color(button,lv_color_hex(0xffffff),0);
  lv_obj_set_style_border_opa(button,45,0);
  lv_obj_set_style_text_color(lv_obj_get_child(button,0),lv_color_hex(0xffffff),0);
  return button;
}

static void glass_weather_open(lv_event_t *e)
{
  LV_UNUSED(e);lv_obj_t *card=panel_card(glass_tr("当地天气"));if(!card)return;
  detail_page(card);memset(&g_weather_detail,0,sizeof(g_weather_detail));
  g_weather_detail.root=card;
  glass_navigation_register(glass_weather_open,false,0);
  g_weather_detail.data=calloc(1,sizeof(*g_weather_detail.data));
  g_weather_home.deferred=true;
  lv_obj_add_event_cb(card,glass_weather_detail_deleted,LV_EVENT_DELETE,NULL);
  weather_scene_attach_page(&g_weather_detail.scene,card,1024,GLASS_CONTENT_HEIGHT);
  detail_text(card,glass_tr("天气"),32,25,120,20,0xffffff);
  g_weather_detail.state_dot=glass_box(card,584,38,6,6,0xa7e2c1,3);
  lv_obj_remove_flag(g_weather_detail.state_dot,LV_OBJ_FLAG_CLICKABLE);
  g_weather_detail.data_state=detail_text(card,"",600,31,100,14,0xe1edf5);
  weather_page_button(card,glass_tr("刷新天气"),716,18,132,glass_weather_refresh_clicked);
  weather_page_button(card,glass_tr("返回桌面"),864,18,128,panel_hide);

  g_weather_detail.location=detail_text(card,glass_tr("当地天气"),32,86,560,34,0xffffff);
  /* Production already owns a 128px number face under the legacy font72 name. */
  g_weather_detail.temperature=glass_number(card,"--",24,123,342,true,0xffffff);
  if(!g_glass.font72)lv_obj_set_style_text_font(g_weather_detail.temperature,home_font(64),0);
  g_weather_detail.unit=detail_text(card,"°",240,135,64,64,0xffffff);
  g_weather_detail.summary=detail_text(card,"",32,278,560,20,0xffffff);
  g_weather_detail.status=detail_text(card,"",32,313,720,14,0xd4e2ed);

  /* Frost samples this exact sky region once per asset change, never per tick. */
  lv_obj_t *metrics=glass_box(card,WEATHER_FROST_X,WEATHER_FROST_Y,
                            WEATHER_FROST_WIDTH,WEATHER_FROST_HEIGHT,0x0b2133,22);
  g_weather_detail.metrics=metrics;
  lv_obj_set_style_bg_opa(metrics,LV_OPA_TRANSP,0);
  lv_obj_remove_flag(metrics,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(metrics,weather_frost_draw,LV_EVENT_DRAW_MAIN,NULL);
  detail_text(metrics,glass_tr("空气质量 · AQI"),24,24,270,14,0xe1edf5);
  g_weather_detail.aqi=detail_text(metrics,"--",24,58,270,34,0xffffff);
  g_weather_detail.category=detail_text(metrics,"",24,113,270,14,0xa7e2c1);
  detail_text(metrics,glass_tr("相对湿度"),346,24,270,14,0xe1edf5);
  g_weather_detail.humidity=detail_text(metrics,"--%",346,58,270,34,0xffffff);
  g_weather_detail.humidity_hint=detail_text(metrics,"",346,113,270,14,0xe1edf5);
  detail_text(metrics,"PM2.5",668,24,270,14,0xe1edf5);
  g_weather_detail.pm25=detail_text(metrics,"--",668,58,270,34,0xffffff);
  detail_text(metrics,glass_tr("微克 / 立方米"),668,113,270,14,0xe1edf5);
  g_weather_detail.report=detail_text(card,"",32,531,760,14,0xc2d4e2);
  lv_obj_t *unit=detail_text(card,glass_tr("温度单位 · 摄氏度"),800,531,192,14,0xc2d4e2);
  lv_obj_set_style_text_align(unit,LV_TEXT_ALIGN_RIGHT,0);
  struct glass_weather_snapshot weather;glass_weather_get(&weather);
  struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
  uint64_t ms=(uint64_t)now.tv_sec*1000+now.tv_nsec/1000000;
  glass_weather_detail_update(&weather,weather.valid&&ms>=weather.received_ms&&ms-weather.received_ms>=7200000);
}
