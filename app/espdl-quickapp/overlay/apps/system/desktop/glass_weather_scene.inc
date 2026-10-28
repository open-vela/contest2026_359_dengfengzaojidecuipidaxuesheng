/* SPDX-License-Identifier: Apache-2.0
 * Native text over original, baseline-JPEG sky artwork. Reuse the existing
 * decoder once per asset change; no network, timer or per-frame decompression.
 */
#include "qpk_mjpeg.h"
#include "glass_weather_art.inc"

enum weather_sky_kind
{
  SKY_UNKNOWN, SKY_CLEAR, SKY_CLOUDY, SKY_OVERCAST, SKY_RAIN,
  SKY_SNOW, SKY_FOG, SKY_THUNDER, SKY_RAIN_HEAVY, SKY_SNOW_HEAVY,
  SKY_SLEET, SKY_HAZE, SKY_DUST, SKY_DRIZZLE, SKY_RAIN_MODERATE,
  SKY_RAIN_LARGE, SKY_RAIN_EXTREME, SKY_SHOWERS, SKY_HAIL, SKY_FREEZING_RAIN,
  SKY_SNOW_MODERATE, SKY_SNOW_LARGE, SKY_SNOW_SHOWERS, SKY_DUST_FLOATING,
  SKY_DUST_BLOWING, SKY_WIND, SKY_THUNDER_HAIL, SKY_TORNADO, SKY_TROPICAL_STORM
};

struct weather_scene
{
  lv_obj_t *object;
  lv_image_dsc_t image;
  uint16_t *pixels;
  enum weather_sky_kind kind;
  unsigned asset, decodes;
  size_t pixel_capacity;
  bool night, compact, painted, full_page;
};

static struct
{
  struct weather_scene scene;
  lv_obj_t *city, *status;
  unsigned revision, widget;
  enum glass_weather_state state;
  /* Snapshot caches belong in the runtime PSRAM heap, not boot-time BSS. */
  struct glass_weather_data *data;
  bool valid, stale, night, painted, deferred;
} g_weather_home;

static bool weather_text(lv_obj_t *label,const char *text)
{
  if(!strcmp(lv_label_get_text(label),text))return false;
  lv_label_set_text(label,text);return true;
}

/* Report timestamps affect only the detail footer, not the home card. */
static bool weather_values_equal(const struct glass_weather_data *a,const struct glass_weather_data *b)
{
  return a->temperature==b->temperature&&a->humidity==b->humidity&&a->aqi==b->aqi&&
    a->level==b->level&&a->pm25==b->pm25&&!strcmp(a->location,b->location)&&
    !strcmp(a->condition,b->condition)&&!strcmp(a->category,b->category);
}

static bool weather_home_data_changed(const struct glass_weather_snapshot *w)
{
  return !g_weather_home.painted||g_weather_home.valid!=w->valid||
    (w->valid&&g_weather_home.revision!=w->revision&&
     (!g_weather_home.data||!weather_values_equal(g_weather_home.data,&w->data)));
}

static void weather_home_deleted(lv_event_t *e)
{
  LV_UNUSED(e);free(g_weather_home.data);g_weather_home.data=NULL;
  g_weather_home.city=NULL;g_weather_home.status=NULL;
}

static enum weather_sky_kind weather_sky(const struct glass_weather_snapshot *w)
{
  if (!w->valid || !w->data.condition[0]) return SKY_UNKNOWN;
  const char *s=w->data.condition;
  /* Compound descriptions retain their strongest/current precipitation cue.
   * Mixed rain/snow must be recognized before the generic rain/snow tests. */
  if(strstr(s,"雷"))return strstr(s,"雹")?SKY_THUNDER_HAIL:SKY_THUNDER;
  if(strstr(s,"雹"))return SKY_HAIL;
  if(strstr(s,"冻雨")||strstr(s,"冻结雨"))return SKY_FREEZING_RAIN;
  if(strstr(s,"雨")&&strstr(s,"雪"))return SKY_SLEET;
  if(strstr(s,"雪")){
    if(strstr(s,"暴雪")||strstr(s,"雪暴"))return SKY_SNOW_HEAVY;
    if(strstr(s,"大雪"))return SKY_SNOW_LARGE;
    if(strstr(s,"中雪"))return SKY_SNOW_MODERATE;
    return strstr(s,"阵雪")?SKY_SNOW_SHOWERS:SKY_SNOW;
  }
  if(strstr(s,"龙卷"))return SKY_TORNADO;
  if(strstr(s,"台风")||strstr(s,"飓风")||strstr(s,"热带风暴"))return SKY_TROPICAL_STORM;
  if(strstr(s,"雨")){
    if(strstr(s,"大暴雨"))return SKY_RAIN_EXTREME;
    if(strstr(s,"暴雨")||strstr(s,"强降雨"))return SKY_RAIN_HEAVY;
    if(strstr(s,"大雨"))return SKY_RAIN_LARGE;
    if(strstr(s,"中雨"))return SKY_RAIN_MODERATE;
    if(strstr(s,"毛毛")||strstr(s,"细雨"))return SKY_DRIZZLE;
    return strstr(s,"阵雨")?SKY_SHOWERS:SKY_RAIN;
  }
  if(strstr(s,"浮尘"))return SKY_DUST_FLOATING;
  if(strstr(s,"扬沙")||strstr(s,"扬尘"))return SKY_DUST_BLOWING;
  if(strstr(s,"沙")||strstr(s,"尘"))return SKY_DUST;
  if(strstr(s,"霾"))return SKY_HAZE;
  if(strstr(s,"雾"))return SKY_FOG;
  if(strstr(s,"风"))return SKY_WIND;
  if(strstr(s,"阴"))return SKY_OVERCAST;
  if(strstr(s,"云"))return SKY_CLOUDY;
  if(strstr(s,"晴"))return SKY_CLEAR;
  return SKY_UNKNOWN;
}

static unsigned weather_art_for_sky(enum weather_sky_kind kind,bool night)
{
  switch(kind){
    case SKY_CLEAR:return night?WEATHER_ART_CLEAR_NIGHT:WEATHER_ART_CLEAR;
    case SKY_CLOUDY:return night?WEATHER_ART_CLOUDY_NIGHT:WEATHER_ART_CLOUDY;
    case SKY_OVERCAST:return WEATHER_ART_OVERCAST;
    case SKY_RAIN:return WEATHER_ART_RAIN;
    case SKY_RAIN_HEAVY:return WEATHER_ART_RAIN_HEAVY;
    case SKY_THUNDER:return WEATHER_ART_THUNDER;
    case SKY_SNOW:return WEATHER_ART_SNOW;
    case SKY_SNOW_HEAVY:return WEATHER_ART_SNOW_HEAVY;
    case SKY_SLEET:return WEATHER_ART_SLEET;
    case SKY_FOG:return WEATHER_ART_FOG;
    case SKY_HAZE:return WEATHER_ART_HAZE;
    case SKY_DUST:return WEATHER_ART_DUST;
    case SKY_DRIZZLE:return WEATHER_ART_DRIZZLE;
    case SKY_RAIN_MODERATE:return WEATHER_ART_RAIN_MODERATE;
    case SKY_RAIN_LARGE:return WEATHER_ART_RAIN_LARGE;
    case SKY_RAIN_EXTREME:return WEATHER_ART_RAIN_EXTREME;
    case SKY_SHOWERS:return WEATHER_ART_SHOWERS;
    case SKY_HAIL:return WEATHER_ART_HAIL;
    case SKY_FREEZING_RAIN:return WEATHER_ART_FREEZING_RAIN;
    case SKY_SNOW_MODERATE:return WEATHER_ART_SNOW_MODERATE;
    case SKY_SNOW_LARGE:return WEATHER_ART_SNOW_LARGE;
    case SKY_SNOW_SHOWERS:return WEATHER_ART_SNOW_SHOWERS;
    case SKY_DUST_FLOATING:return WEATHER_ART_DUST_FLOATING;
    case SKY_DUST_BLOWING:return WEATHER_ART_DUST_BLOWING;
    case SKY_WIND:return WEATHER_ART_WIND;
    case SKY_THUNDER_HAIL:return WEATHER_ART_THUNDER_HAIL;
    case SKY_TORNADO:return WEATHER_ART_TORNADO;
    case SKY_TROPICAL_STORM:return WEATHER_ART_TROPICAL_STORM;
    default:return WEATHER_ART_NONE;
  }
}

static bool weather_scene_night_tint(const struct weather_scene *s)
{
  return s->night&&s->asset!=WEATHER_ART_CLEAR_NIGHT&&s->asset!=WEATHER_ART_CLOUDY_NIGHT;
}

/* Clock-based atmosphere, without claiming an observed sunrise or moon phase. */
static bool weather_is_night(void)
{
  time_t now=time(NULL);struct tm local={0};
#ifndef CONFIG_LIBC_LOCALTIME
  now+=GLASS_CLOCK_OFFSET_SECONDS;
#endif
  return localtime_r(&now,&local)&&local.tm_year>=120&&
         (local.tm_hour<6||local.tm_hour>=18);
}

static void weather_scene_release(struct weather_scene *s)
{
  if(s->pixels)lv_image_cache_drop(&s->image);
  free(s->pixels);s->pixels=NULL;
  s->pixel_capacity=0;
  memset(&s->image,0,sizeof(s->image));
}

static void weather_scrim(lv_layer_t *layer,const lv_area_t *a,
                          lv_grad_dir_t direction,lv_opa_t first,lv_opa_t last)
{
  lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);
  d.bg_color=lv_color_hex(0x071a2d);d.bg_opa=255;
  d.bg_grad.dir=direction;d.bg_grad.stops_count=2;
  d.bg_grad.stops[0].color=d.bg_grad.stops[1].color=d.bg_color;
  d.bg_grad.stops[0].opa=first;d.bg_grad.stops[0].frac=0;
  d.bg_grad.stops[1].opa=last;d.bg_grad.stops[1].frac=255;
  lv_draw_rect(layer,&d,a);
}

static void weather_scene_draw(lv_event_t *e)
{
  struct weather_scene *s=lv_event_get_user_data(e);
  lv_layer_t *layer=lv_event_get_layer(e);lv_area_t a;
  lv_obj_get_coords(s->object,&a);
  if(s->pixels){
    lv_draw_image_dsc_t d;lv_draw_image_dsc_init(&d);d.src=&s->image;
    if(weather_scene_night_tint(s)){
      d.recolor=lv_color_hex(0x102b54);d.recolor_opa=75;
    }
    lv_area_t image_area=a;
    if(s->full_page){
      /* Reuse the 588x326 detail decode for a full-page sky. LVGL transforms
       * directly into the invalidated framebuffer region, no 1024x568 cache. */
      d.scale_x=(lv_area_get_width(&a)*256+s->image.header.w-1)/s->image.header.w;
      d.scale_y=(lv_area_get_height(&a)*256+s->image.header.h-1)/s->image.header.h;
      d.pivot=(lv_point_t){0,0};d.antialias=1;
      image_area.x2=image_area.x1+s->image.header.w-1;
      image_area.y2=image_area.y1+s->image.header.h-1;
    }
    lv_draw_image(layer,&d,&image_area);
  }
  if(s->full_page){
    /* The artwork already reserves dark text areas. A restrained constant
     * veil avoids coarse RGB565 opacity-gradient bands over a large sky. */
    lv_draw_rect_dsc_t veil;lv_draw_rect_dsc_init(&veil);
    veil.bg_color=lv_color_hex(0x071a2d);veil.bg_opa=weather_scene_night_tint(s)?95:60;
    lv_draw_rect(layer,&veil,&a);
  }else{
    weather_scrim(layer,&a,LV_GRAD_DIR_HOR,125,8);
    lv_area_t foot=a;foot.y1+=s->compact?126:180;
    weather_scrim(layer,&foot,LV_GRAD_DIR_VER,0,s->night?190:215);
  }
  if(s->compact){
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);
    d.bg_color=lv_color_hex(0xffffff);d.bg_opa=55;
    lv_area_t rule={a.x1+22,a.y1+184,a.x2-22,a.y1+184};lv_draw_rect(layer,&d,&rule);
  }
}

static void weather_scene_deleted(lv_event_t *e)
{
  struct weather_scene *s=lv_event_get_user_data(e);
  weather_scene_release(s);s->object=NULL;
}

static void weather_scene_attach(struct weather_scene *s,lv_obj_t *parent,
                                 int w,int h,bool compact)
{
  memset(s,0,sizeof(*s));s->compact=compact;s->asset=WEATHER_ART_NONE;
  s->object=glass_box(parent,0,0,w,h,0x35516b,20);
  lv_obj_remove_flag(s->object,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s->object,weather_scene_draw,LV_EVENT_DRAW_MAIN,s);
  lv_obj_add_event_cb(s->object,weather_scene_deleted,LV_EVENT_DELETE,s);
}

static void weather_scene_attach_page(struct weather_scene *s,lv_obj_t *parent,int w,int h)
{
  weather_scene_attach(s,parent,w,h,false);
  s->full_page=true;
  lv_obj_set_style_radius(s->object,0,0);
}

static void weather_scene_update(struct weather_scene *s,
                                 const struct glass_weather_snapshot *w,bool night)
{
  enum weather_sky_kind kind=weather_sky(w);
  if(!s->object||(s->painted&&s->kind==kind&&s->night==night))return;
  unsigned asset=weather_art_for_sky(kind,night);
  bool replace=asset!=s->asset||(!s->painted&&asset<WEATHER_ART_NONE);
  if(replace){
    if(s->pixels)lv_image_cache_drop(&s->image);
    memset(&s->image,0,sizeof(s->image));s->asset=asset;
    if(asset<WEATHER_ART_NONE){
      const struct weather_art *p=&weather_artworks[s->compact?1:0][asset];
      /* Reuse the current native-size allocation, and read complete JPEGs
       * directly from flash. Only decoder workspace is temporary. */
      size_t bytes=(size_t)p->w*p->h*2;
      if(s->pixel_capacity!=bytes){
        weather_scene_release(s);s->pixels=malloc(bytes);
        if(s->pixels)s->pixel_capacity=bytes;
      }
      void *decoder=qpk_mjpeg_create();
      if(s->pixels&&decoder){
        int ret=qpk_mjpeg_decode_still(decoder,p->data,p->size,s->pixels,p->w,p->h);
        if(ret==0){
          s->decodes++;
          s->image.header.magic=LV_IMAGE_HEADER_MAGIC;
          s->image.header.cf=LV_COLOR_FORMAT_RGB565;
          s->image.header.w=p->w;s->image.header.h=p->h;s->image.header.stride=p->w*2;
          s->image.data_size=bytes;s->image.data=(const uint8_t *)s->pixels;
        }
      }
      qpk_mjpeg_destroy(decoder);
      if(!s->image.data)weather_scene_release(s);
    }else weather_scene_release(s);
  }
  s->kind=kind;s->night=night;s->painted=true;
  lv_obj_invalidate(s->object);
}

static const char *weather_status(const struct glass_weather_snapshot *w,bool stale)
{
  if(w->state==WEATHER_WAIT_NET)return w->valid?glass_tr("离线 · 保留上次天气"):glass_tr("连接网络后更新");
  if(w->state==WEATHER_WAIT_TIME)return glass_tr("等待系统校时");
  if(w->state==WEATHER_FETCHING)return glass_tr("正在更新天气");
  if(w->state==WEATHER_FAILED)return w->valid?"更新失败 · 保留上次天气":glass_tr("暂时无法获取天气");
  if(stale)return glass_tr("天气数据待更新");
  return w->valid?glass_tr("按网络位置定位"):glass_tr("暂无天气数据");
}

static void weather_home_update(const struct glass_weather_snapshot *w,bool stale,bool night)
{
  if(!g_weather_home.city)return;
  bool changed=weather_home_data_changed(w)||
    g_weather_home.state!=w->state||g_weather_home.valid!=w->valid||
    g_weather_home.stale!=stale||g_weather_home.night!=night;
  if(changed){
    weather_text(g_weather_home.city,w->valid&&w->data.location[0]?w->data.location:glass_tr("当地天气"));
    weather_text(g_weather_home.status,weather_status(w,stale));
    weather_scene_update(&g_weather_home.scene,w,night);
  }
  if(changed||g_weather_home.widget!=g_glass.widget){
    static const uint32_t colors[]={0xe2edf6,0xc0edce,0xf4e2a7,0xffd2a8,0xffb9b8,0xe6c4ef,0xf4b7ce};
    unsigned level=w->valid&&w->data.aqi>=0&&w->data.level>=1&&w->data.level<=6?w->data.level:0;
    uint32_t color=g_glass.widget==0?colors[level]:0xffffff;
    lv_obj_set_style_text_color(g_glass.widget_value,lv_color_hex(color),0);
    lv_obj_set_style_text_color(g_glass.widget_category,lv_color_hex(color),0);
  }
  if(changed&&g_weather_home.data)*g_weather_home.data=w->data;
  g_weather_home.revision=w->revision;g_weather_home.state=w->state;
  g_weather_home.valid=w->valid;g_weather_home.stale=stale;g_weather_home.night=night;
  g_weather_home.widget=g_glass.widget;g_weather_home.painted=true;
}
