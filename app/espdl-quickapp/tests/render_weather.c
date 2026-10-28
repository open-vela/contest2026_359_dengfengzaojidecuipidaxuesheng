/* SPDX-License-Identifier: Apache-2.0
 * Actual LVGL weather cards and production updater, offline service snapshots.
 */
#define UNIFIED_DEVICE_NO_MAIN
#include "render_unified_devices.c"
extern void review_weather_snapshot_fixture(const struct glass_weather_snapshot *);
static unsigned scene_draws;
static void count_draw(lv_event_t *e){(void)e;scene_draws++;}
static lv_point_t weather_touch_point;
static lv_indev_state_t weather_touch_state;
static void weather_touch_read(lv_indev_t *input,lv_indev_data_t *data)
{(void)input;data->point=weather_touch_point;data->state=weather_touch_state;}
static void weather_touch(lv_indev_t *input,int x,int y,bool pressed)
{
  weather_touch_point=(lv_point_t){x,y};
  weather_touch_state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
  lv_tick_inc(20);lv_indev_read(input);
}
static unsigned objects(lv_obj_t *o)
{unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(o);i++)n+=objects(lv_obj_get_child(o,i));return n;}
static void check_text(lv_obj_t *o)
{
  if(!lv_obj_is_visible(o))return;
  if(lv_obj_has_class(o,&lv_label_class)){
    lv_area_t a,b;lv_obj_get_coords(o,&a);lv_obj_get_coords(lv_obj_get_parent(o),&b);
    assert(a.x1>=b.x1&&a.x2<=b.x2&&a.y1>=b.y1&&a.y2<=b.y2);
  }
  for(unsigned i=0;i<lv_obj_get_child_count(o);i++)check_text(lv_obj_get_child(o,i));
}
static uint64_t weather_perf_clock(void)
{struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t weather_perf_median(uint64_t *values)
{
  for(unsigned i=0;i<5;i++)for(unsigned j=i+1;j<5;j++)if(values[j]<values[i]){
    uint64_t tmp=values[i];values[i]=values[j];values[j]=tmp;
  }
  return values[2];
}
static void weather_perf_run(lv_display_t *display,struct glass_weather_snapshot *w)
{
  glass_weather_open(NULL);lv_refr_now(display);
  uint64_t frost[5],sky[5],covered[5];
  for(unsigned sample=0;sample<5;sample++){
    uint64_t start=weather_perf_clock();
    for(unsigned i=0;i<20;i++){weather_frost_release();weather_frost_update();}
    frost[sample]=(weather_perf_clock()-start)/20;
    start=weather_perf_clock();
    for(unsigned i=0;i<20;i++){
      strcpy(w->data.condition,i%2?"多云":"晴");weather_scene_update(&g_weather_detail.scene,w,false);
    }
    sky[sample]=(weather_perf_clock()-start)/20;
  }
  unsigned home_decodes=g_weather_home.scene.decodes;
  for(unsigned sample=0;sample<5;sample++){
    uint64_t start=weather_perf_clock();
    for(unsigned i=0;i<20;i++){
      strcpy(w->data.condition,i%2?"多云":"晴");w->revision++;
      review_weather_snapshot_fixture(w);glass_update(NULL);
    }
    covered[sample]=(weather_perf_clock()-start)/20;
  }
  unsigned covered_home_decodes=g_weather_home.scene.decodes-home_decodes;
  assert(covered_home_decodes==0);
  weather_scene_update(&g_weather_detail.scene,w,weather_is_night());weather_frost_update();
  w->revision++;review_weather_snapshot_fixture(w);glass_update(NULL);lv_refr_now(display);
  scene_draws=0;lv_obj_add_event_cb(g_weather_detail.scene.object,count_draw,LV_EVENT_DRAW_MAIN,NULL);
  for(unsigned i=0;i<5;i++){
    w->revision++;review_weather_snapshot_fixture(w);glass_update(NULL);lv_refr_now(display);
  }
  assert(scene_draws==0);
  char path[1024];snprintf(path,sizeof(path),"%s/perf.json",review_out);
  FILE *f=fopen(path,"w");assert(f);
  fprintf(f,"{\"environment\":\"native host with ASan/UBSan; not hardware timing\","
    "\"frost_rebuild_ns_median\":%llu,\"scene_switch_ns_median\":%llu,"
    "\"covered_update_ns_median\":%llu,\"covered_home_decodes\":%u,"
    "\"equal_payload_new_revisions\":5,\"equal_payload_scene_draws\":%u}\n",
    (unsigned long long)weather_perf_median(frost),(unsigned long long)weather_perf_median(sky),
    (unsigned long long)weather_perf_median(covered),covered_home_decodes,scene_draws);
  fclose(f);printf("WEATHER_PERF_OK unchanged-payload scene draws=%u\n",scene_draws);
  panel_hide(NULL);
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
  /* Match the actual production font hidden behind this legacy field name. */
  g_glass.font72=lv_tiny_ttf_create_data_ex(font,size,128,LV_FONT_KERNING_NORMAL,24);
  assert(g_glass.font72);
  g_glass.reduced_motion=true;
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  lv_indev_t *input=lv_indev_create();lv_indev_set_type(input,LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(input,weather_touch_read);lv_indev_set_display(input,display);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
  struct glass_weather_snapshot w={.valid=true,.state=WEATHER_READY,.revision=10,
    .received_ms=(uint64_t)ts.tv_sec*1000+ts.tv_nsec/1000000,
    .data={.temperature=24,.humidity=48,.aqi=32,.level=1,.pm25=12}};
  strcpy(w.data.location,"北京市");strcpy(w.data.condition,"晴");strcpy(w.data.category,"优");
  strcpy(w.data.report_time,"2026-10-18 14:00 · 离线渲染样本");
  static const struct {
    const char *condition,*id;
    unsigned asset;
    int temperature,humidity;
  } cases[]={
    {"晴","sunny",WEATHER_ART_CLEAR,24,48},
    {"多云","cloudy",WEATHER_ART_CLOUDY,23,52},
    {"阴","overcast",WEATHER_ART_OVERCAST,20,63},
    {"小雨","rain",WEATHER_ART_RAIN,18,82},
    {"暴雨","rain-heavy",WEATHER_ART_RAIN_HEAVY,17,93},
    {"雷阵雨","thunder",WEATHER_ART_THUNDER,19,86},
    {"小雪","snow",WEATHER_ART_SNOW,-2,72},
    {"暴雪","snow-heavy",WEATHER_ART_SNOW_HEAVY,-6,81},
    {"雨夹雪","sleet",WEATHER_ART_SLEET,1,87},
    {"雾","fog",WEATHER_ART_FOG,12,96},
    {"霾","haze",WEATHER_ART_HAZE,16,55},
    {"沙尘暴","dust",WEATHER_ART_DUST,22,21},
    {"毛毛雨","drizzle",WEATHER_ART_DRIZZLE,19,78},
    {"中雨","rain-moderate",WEATHER_ART_RAIN_MODERATE,18,86},
    {"大雨","rain-large",WEATHER_ART_RAIN_LARGE,17,91},
    {"特大暴雨","rain-extreme",WEATHER_ART_RAIN_EXTREME,16,98},
    {"阵雨","showers",WEATHER_ART_SHOWERS,22,73},
    {"冰雹","hail",WEATHER_ART_HAIL,7,79},
    {"冻雨","freezing-rain",WEATHER_ART_FREEZING_RAIN,-1,91},
    {"中雪","snow-moderate",WEATHER_ART_SNOW_MODERATE,-3,78},
    {"大雪","snow-large",WEATHER_ART_SNOW_LARGE,-4,84},
    {"阵雪","snow-showers",WEATHER_ART_SNOW_SHOWERS,-1,72},
    {"浮尘","dust-floating",WEATHER_ART_DUST_FLOATING,24,32},
    {"扬沙","dust-blowing",WEATHER_ART_DUST_BLOWING,23,24},
    {"大风","wind",WEATHER_ART_WIND,16,42},
    {"雷阵雨伴冰雹","thunder-hail",WEATHER_ART_THUNDER_HAIL,9,85},
    {"龙卷风","tornado",WEATHER_ART_TORNADO,20,77},
    {"台风","tropical-storm",WEATHER_ART_TROPICAL_STORM,25,96},
    {"未知天气","unknown",WEATHER_ART_NONE,24,48}
  };
  /* Priority matters for mixed, intense and transitioning descriptions. */
  static const struct {const char *text;enum weather_sky_kind kind;} aliases[]={
    {"雷阵雨伴冰雹",SKY_THUNDER_HAIL},{"雷暴",SKY_THUNDER},
    {"阵雨",SKY_SHOWERS},{"中雨",SKY_RAIN_MODERATE},{"冻雨",SKY_FREEZING_RAIN},
    {"中到大雨",SKY_RAIN_LARGE},{"大到暴雨",SKY_RAIN_HEAVY},{"强降雨",SKY_RAIN_HEAVY},
    {"中到大雪",SKY_SNOW_LARGE},{"大暴雪",SKY_SNOW_HEAVY},{"雪暴",SKY_SNOW_HEAVY},
    {"雨夹雪",SKY_SLEET},{"小雨转雪",SKY_SLEET},
    {"浓雾",SKY_FOG},{"轻度霾",SKY_HAZE},{"雾霾",SKY_HAZE},
    {"扬沙",SKY_DUST_BLOWING},{"浮尘",SKY_DUST_FLOATING},{"强沙尘暴",SKY_DUST},
    {"冰雹",SKY_HAIL},{"",SKY_UNKNOWN},
    {"小到中雨",SKY_RAIN_MODERATE},{"小到中雪",SKY_SNOW_MODERATE},
    {"大暴雨",SKY_RAIN_EXTREME},{"大到特大暴雨",SKY_RAIN_EXTREME},
    {"特大暴雨",SKY_RAIN_EXTREME},{"暴雨到大暴雨",SKY_RAIN_EXTREME},
    {"毛毛雨",SKY_DRIZZLE},{"细雨",SKY_DRIZZLE},{"冻结雨",SKY_FREEZING_RAIN},
    {"雨伴冰雹",SKY_HAIL},{"雷雨伴雹",SKY_THUNDER_HAIL},
    {"阵雪",SKY_SNOW_SHOWERS},{"雨雪天气",SKY_SLEET},
    {"重度霾",SKY_HAZE},{"大雾",SKY_FOG},{"特强浓雾",SKY_FOG},
    {"扬尘",SKY_DUST_BLOWING},{"大风",SKY_WIND},{"强风",SKY_WIND},
    {"热带风暴",SKY_TROPICAL_STORM},{"飓风",SKY_TROPICAL_STORM},
    {"台风暴雨",SKY_TROPICAL_STORM},{"龙卷风",SKY_TORNADO},
    {"晴间多云",SKY_CLOUDY},{"少云",SKY_CLOUDY},{"晴朗",SKY_CLEAR},
    {"阴转小雨",SKY_RAIN},{"天气不详",SKY_UNKNOWN}
  };
  for(unsigned i=0;i<sizeof(aliases)/sizeof(aliases[0]);i++){
    strcpy(w.data.condition,aliases[i].text);assert(weather_sky(&w)==aliases[i].kind);
  }
  w.valid=false;strcpy(w.data.condition,"暴雨");assert(weather_sky(&w)==SKY_UNKNOWN);w.valid=true;
  for(unsigned theme=0;theme<2;theme++){
    panel_hide(NULL);g_desktop.light_theme=theme!=0;g_glass.widget=0;
    review_weather_snapshot_fixture(&w);glass_rebuild();
    const void *retained_home=NULL;
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
      strcpy(w.data.condition,cases[i].condition);w.revision++;
      w.data.temperature=cases[i].temperature;w.data.humidity=cases[i].humidity;
      /* Illustrative air readings keep haze/dust previews consistent with
       * their visual atmosphere. Production values still come from service. */
      if(cases[i].asset==WEATHER_ART_HAZE){
        w.data.aqi=186;w.data.level=4;w.data.pm25=128;strcpy(w.data.category,"中度污染");
      }else if(cases[i].asset==WEATHER_ART_DUST||cases[i].asset==WEATHER_ART_DUST_BLOWING||cases[i].asset==WEATHER_ART_DUST_FLOATING){
        w.data.aqi=245;w.data.level=5;w.data.pm25=176;strcpy(w.data.category,"重度污染");
      }else{
        w.data.aqi=32;w.data.level=1;w.data.pm25=12;strcpy(w.data.category,"优");
      }
      review_weather_snapshot_fixture(&w);glass_update(NULL);
      weather_home_update(&w,false,false);
      assert(g_weather_home.scene.asset==cases[i].asset);
      if(cases[i].asset!=WEATHER_ART_NONE){
        assert(g_weather_home.scene.pixels&&g_weather_home.scene.image.data_size==307*272*2);
        if(retained_home)assert(retained_home==g_weather_home.scene.pixels);
        retained_home=g_weather_home.scene.pixels;
        if(i>=3){
          unsigned decodes=g_weather_home.scene.decodes;
          weather_scene_update(&g_weather_home.scene,&w,true);
          weather_scene_update(&g_weather_home.scene,&w,false);
          assert(g_weather_home.scene.decodes==decodes);
        }
      }else assert(!g_weather_home.scene.pixels);
      review_capture(display,cases[i].id,"天气卡片 · 离线天气样本");
      check_text(g_glass.home_cards[3]);
      glass_weather_open(NULL);weather_scene_update(&g_weather_detail.scene,&w,false);weather_frost_update();
      assert(g_weather_detail.scene.asset==cases[i].asset);
      assert(g_weather_detail.scene.full_page);
      if(cases[i].asset!=WEATHER_ART_NONE){
        assert(g_weather_detail.scene.pixels&&g_weather_detail.scene.image.data_size==588*326*2);
        assert(g_weather_detail.frost_pixels&&g_weather_detail.frost.data_size==16640);
      }else assert(!g_weather_detail.frost_pixels);
      char id[80];snprintf(id,sizeof(id),"detail-%s",cases[i].id);
      review_capture(display,id,"天气详情 · 离线天气样本");check_text(g_desktop.current_card);
      lv_area_t bounds;lv_obj_get_coords(g_weather_detail.scene.object,&bounds);
      assert(bounds.x1==0&&bounds.x2==1023&&bounds.y1==32&&bounds.y2==599);
      unsigned before_decodes=g_weather_detail.scene.decodes,before_blurs=g_weather_detail.frost_builds;
      unsigned before_asset=g_weather_detail.scene.asset;
      const void *before_pixels=g_weather_detail.scene.pixels,*before_frost=g_weather_detail.frost_pixels;
      weather_scene_update(&g_weather_detail.scene,&w,true);weather_frost_update();
      assert(before_pixels==g_weather_detail.scene.pixels&&before_frost==g_weather_detail.frost_pixels);
      if(g_weather_detail.scene.asset==before_asset){
        assert(g_weather_detail.scene.decodes==before_decodes&&g_weather_detail.frost_builds==before_blurs);
      }
      snprintf(id,sizeof(id),"detail-%s-night",cases[i].id);
      review_capture(display,id,"天气详情 · 夜间离线样本");check_text(g_desktop.current_card);
      panel_hide(NULL);assert(!g_weather_detail.scene.pixels&&!g_weather_detail.frost_pixels);
      weather_home_update(&w,false,true);
      snprintf(id,sizeof(id),"%s-night",cases[i].id);
      review_capture(display,id,"天气卡片 · 夜间离线样本");check_text(g_glass.home_cards[3]);
    }
    strcpy(w.data.condition,"晴");w.revision++;
    review_weather_snapshot_fixture(&w);glass_update(NULL);weather_home_update(&w,false,true);
    assert(g_weather_home.scene.asset==WEATHER_ART_CLEAR_NIGHT);
    review_capture(display,"clear-night","晴夜 · 无云月光");
    glass_weather_open(NULL);weather_scene_update(&g_weather_detail.scene,&w,true);weather_frost_update();
    review_capture(display,"detail-clear-night","天气单页 · 晴夜");check_text(g_desktop.current_card);
    panel_hide(NULL);
    strcpy(w.data.condition,"多云");w.revision++;
    review_weather_snapshot_fixture(&w);glass_update(NULL);weather_home_update(&w,false,true);
    assert(g_weather_home.scene.asset==WEATHER_ART_CLOUDY_NIGHT);
    review_capture(display,"night","天气卡片 · 夜间");
    glass_weather_open(NULL);weather_scene_update(&g_weather_detail.scene,&w,true);weather_frost_update();
    review_capture(display,"detail-night","天气详情 · 夜间");check_text(g_desktop.current_card);
    panel_hide(NULL);glass_weather_open(NULL);weather_scene_update(&g_weather_detail.scene,&w,false);weather_frost_update();
    review_capture(display,"detail-day","天气详情 · 白天");
    panel_hide(NULL);
    /* Same revision may change state/validity: stale readings stay labelled. */
    w.state=WEATHER_WAIT_NET;review_weather_snapshot_fixture(&w);glass_update(NULL);
    assert(strstr(lv_label_get_text(g_weather_home.status),"离线"));
    review_capture(display,"offline-cache","离线 · 保留最近天气");
    glass_weather_open(NULL);
    lv_obj_update_layout(g_desktop.screen);
    assert(!strcmp(lv_label_get_text(g_weather_detail.data_state),"离线缓存"));
    review_capture(display,"detail-offline-cache","天气单页 · 离线缓存");
    w.valid=false;review_weather_snapshot_fixture(&w);glass_update(NULL);
    assert(!g_weather_detail.scene.pixels&&!g_weather_detail.frost_pixels&&!strcmp(lv_label_get_text(g_weather_detail.temperature),"--"));
    assert(!strcmp(lv_label_get_text(g_weather_detail.aqi),"--"));
    review_capture(display,"detail-empty","天气单页 · 无数据");check_text(g_desktop.current_card);
    panel_hide(NULL);
    assert(!strcmp(lv_label_get_text(g_glass.temperature),"--°"));
    assert(g_weather_home.scene.kind==SKY_UNKNOWN);
    review_capture(display,"empty","无数据 · 不虚构天气");
    w.state=WEATHER_FETCHING;review_weather_snapshot_fixture(&w);glass_update(NULL);
    assert(!strcmp(lv_label_get_text(g_weather_home.status),"正在更新天气"));
    glass_weather_open(NULL);
    lv_obj_update_layout(g_desktop.screen);
    assert(!strcmp(lv_label_get_text(g_weather_detail.data_state),"更新中"));
    w.state=WEATHER_FAILED;review_weather_snapshot_fixture(&w);glass_update(NULL);
    assert(!strcmp(lv_label_get_text(g_weather_detail.data_state),"更新失败"));
    review_capture(display,"detail-failed","天气单页 · 更新失败");panel_hide(NULL);
    assert(strstr(lv_label_get_text(g_weather_home.status),"无法获取"));
    w.valid=true;w.state=WEATHER_READY;w.data.temperature=-12;w.data.humidity=-1;w.data.aqi=-1;w.revision++;
    review_weather_snapshot_fixture(&w);glass_update(NULL);review_capture(display,"negative","负温度与缺失环境指标");
    assert(!strcmp(lv_label_get_text(g_glass.humidity),"--%"));
    check_text(g_glass.home_cards[3]);
    for(unsigned widget=0;widget<3;widget++){
      g_glass.widget=widget;glass_update(NULL);lv_refr_now(display);check_text(g_glass.home_cards[3]);
    }
    g_glass.widget=0;w.data.temperature=24;w.data.humidity=48;w.data.aqi=32;w.revision++;
    review_weather_snapshot_fixture(&w);glass_update(NULL);lv_refr_now(display);
    unsigned n=objects(g_glass.home_cards[3]),decodes=g_weather_home.scene.decodes;
    assert(g_weather_home.scene.pixels&&decodes>0);
    lv_obj_add_event_cb(g_weather_home.scene.object,count_draw,LV_EVENT_DRAW_MAIN,NULL);
    scene_draws=0;
    for(unsigned i=0;i<120;i++){glass_update(NULL);lv_refr_now(display);}
    assert(scene_draws==0&&objects(g_glass.home_cards[3])==n&&g_weather_home.scene.decodes==decodes);
    printf("WEATHER_IDLE_OK theme=%u objects=%u scene_draws=%u ticks=120\n",theme,n,scene_draws);
    lv_obj_t *arrow=review_callback_find(g_glass.home_cards[3],glass_weather_open);assert(arrow);
    lv_obj_send_event(arrow,LV_EVENT_CLICKED,NULL);assert(g_weather_detail.location);
    lv_obj_update_layout(g_desktop.screen);lv_refr_now(display);
    unsigned detail_count=objects(g_weather_detail.root),detail_decodes=g_weather_detail.scene.decodes;
    unsigned frost_builds=g_weather_detail.frost_builds;
    lv_obj_add_event_cb(g_weather_detail.scene.object,count_draw,LV_EVENT_DRAW_MAIN,NULL);scene_draws=0;
    lv_obj_add_event_cb(g_weather_detail.metrics,count_draw,LV_EVENT_DRAW_MAIN,NULL);
    for(unsigned i=0;i<120;i++){glass_update(NULL);lv_refr_now(display);}
    assert(scene_draws==0&&objects(g_weather_detail.root)==detail_count&&g_weather_detail.scene.decodes==detail_decodes);
    assert(g_weather_detail.frost_builds==frost_builds);
    printf("WEATHER_PAGE_IDLE_OK theme=%u objects=%u pixel_bytes=%lu frost_bytes=%lu scene_draws=%u ticks=120\n",
      theme,detail_count,(unsigned long)g_weather_detail.scene.image.data_size,(unsigned long)g_weather_detail.frost.data_size,scene_draws);
    weather_touch(input,782,72,true);weather_touch(input,782,72,false);
    assert(strstr(lv_label_get_text(g_weather_detail.status),"已请求刷新"));
    weather_touch(input,928,72,true);weather_touch(input,928,72,false);
    assert(!g_weather_detail.root&&!g_weather_detail.scene.pixels&&!g_weather_detail.frost_pixels);
    for(unsigned i=0;i<30;i++){
      glass_rebuild();glass_weather_open(NULL);panel_hide(NULL);
      assert(!g_weather_detail.scene.object);
    }
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  weather_perf_run(display,&w);
  printf("WEATHER_NATIVE_OK: both themes; %lu conditions; %lu classification aliases; %u artwork assets; all day/night; offline; missing values; real navigation; 60 reopen cycles\n",
    (unsigned long)(sizeof(cases)/sizeof(cases[0])),(unsigned long)(sizeof(aliases)/sizeof(aliases[0])),(unsigned)WEATHER_ART_NONE);
  return 0;
}
