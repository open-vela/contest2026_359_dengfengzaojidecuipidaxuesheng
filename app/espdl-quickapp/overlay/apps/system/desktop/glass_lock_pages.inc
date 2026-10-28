/* SPDX-License-Identifier: Apache-2.0 */
static void lock_music_transport_draw(lv_event_t *e)
{
  lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
  int cx=(a.x1+a.x2)/2,cy=(a.y1+a.y2)/2,kind=(intptr_t)lv_event_get_user_data(e);
  lv_layer_t *layer=lv_event_get_layer(e);
  bool playing=g_lock_ui.shown && (g_lock_ui.shown->state==MUSIC_PLAYING || g_lock_ui.shown->state==MUSIC_LOADING) && !g_lock_ui.shown->pause_requested;
  if(!kind && playing) {
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(0xfff8f4);d.radius=4;
    lv_area_t left={cx-23,cy-29,cx-9,cy+29},right={cx+9,cy-29,cx+23,cy+29};
    lv_draw_rect(layer,&d,&left);lv_draw_rect(layer,&d,&right);return;
  }
  lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);d.bg_color=lv_color_hex(0xfff8f4);
  if(!kind) {
    d.p[0]=(lv_point_precise_t){cx-18,cy-29};d.p[1]=(lv_point_precise_t){cx-18,cy+29};d.p[2]=(lv_point_precise_t){cx+27,cy};
    lv_draw_triangle(layer,&d);
  } else for(int i=0;i<2;i++) {
    int x=cx+(i?11:-11);
    d.p[0]=(lv_point_precise_t){x-kind*12,cy-21};d.p[1]=(lv_point_precise_t){x-kind*12,cy+21};d.p[2]=(lv_point_precise_t){x+kind*12,cy};
    lv_draw_triangle(layer,&d);
  }
}

static void lock_music_page(lv_obj_t *root)
{
  lv_obj_set_style_bg_color(root,lv_color_hex(0x493832),0);
  lv_obj_t *art=glass_box(root,64,100,368,368,0x6c535d,28);
  lv_obj_remove_flag(art,LV_OBJ_FLAG_CLICKABLE);
  g_lock_ui.cover.host=art;g_lock_ui.cover.owner=GLASS_COVER_LOCK;
  lv_obj_t *disc=glass_box(art,62,54,244,244,0x3b303a,122);lv_obj_remove_flag(disc,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *ring=glass_box(disc,73,73,98,98,0x9b7b83,49);lv_obj_remove_flag(ring,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *note=glass_symbol(ring,LV_SYMBOL_AUDIO,0,0,0xf5f0f2);
  lv_obj_set_style_text_font(note,glass_icon_font(32),0);lv_obj_center(note);
  lv_obj_t *caption=lock_label(art,glass_tr("暂无专辑封面"),32,322,304,16,0xd6c3c8);
  lv_obj_set_style_text_align(caption,LV_TEXT_ALIGN_CENTER,0);
  g_lock_ui.status=lock_label(root,"",504,78,456,16,0xbba7a0);
  g_lock_ui.title=lock_label(root,"",504,129,456,28,0xfff8f4);
  if(g_home_font34)lv_obj_set_style_text_font(g_lock_ui.title,g_home_font34,0);
  g_lock_ui.artist=lock_label(root,"",504,182,456,20,0xc6afa8);
  g_lock_ui.prev=lock_button(root,LV_SYMBOL_PREV,552,254,64,lock_skip,(void *)-1);
  lv_obj_set_style_bg_opa(g_lock_ui.prev,LV_OPA_TRANSP,0);
  lv_obj_set_style_text_font(lv_obj_get_child(g_lock_ui.prev,0),glass_icon_font(32),0);
  lv_obj_t *play=lock_button(root,LV_SYMBOL_PLAY,680,238,96,lock_toggle,NULL);
  lv_obj_set_style_bg_opa(play,LV_OPA_TRANSP,0);g_lock_ui.play=lv_obj_get_child(play,0);
  lv_obj_set_style_text_font(g_lock_ui.play,glass_icon_font(32),0);
  g_lock_ui.next=lock_button(root,LV_SYMBOL_NEXT,840,254,64,lock_skip,(void *)1);
  lv_obj_set_style_bg_opa(g_lock_ui.next,LV_OPA_TRANSP,0);
  lv_obj_set_style_text_font(lv_obj_get_child(g_lock_ui.next,0),glass_icon_font(32),0);
  lv_obj_add_flag(lv_obj_get_child(g_lock_ui.prev,0),LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(g_lock_ui.play,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(lv_obj_get_child(g_lock_ui.next,0),LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(g_lock_ui.prev,lock_music_transport_draw,LV_EVENT_DRAW_MAIN,(void *)-1);
  lv_obj_add_event_cb(play,lock_music_transport_draw,LV_EVENT_DRAW_MAIN,NULL);
  lv_obj_add_event_cb(g_lock_ui.next,lock_music_transport_draw,LV_EVENT_DRAW_MAIN,(void *)1);
  g_lock_ui.lyric[1]=lock_label(root,"",504,370,456,28,0xfff4ed);
  g_lock_ui.lyric[2]=lock_label(root,"",504,420,456,20,0xbba7a0);
  g_lock_ui.progress=lv_bar_create(root);lv_obj_remove_style_all(g_lock_ui.progress);
  lv_obj_set_pos(g_lock_ui.progress,504,484);lv_obj_set_size(g_lock_ui.progress,456,6);
  lv_bar_set_range(g_lock_ui.progress,0,1000);lv_obj_remove_flag(g_lock_ui.progress,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(g_lock_ui.progress,lv_color_hex(0x897269),LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_lock_ui.progress,LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_lock_ui.progress,lv_color_hex(0xf8e9dd),LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(g_lock_ui.progress,LV_OPA_COVER,LV_PART_INDICATOR);
  lv_obj_set_style_radius(g_lock_ui.progress,3,LV_PART_MAIN);lv_obj_set_style_radius(g_lock_ui.progress,3,LV_PART_INDICATOR);
  g_lock_ui.elapsed=lock_label(root,"00:00",504,504,96,16,0xbba7a0);
  g_lock_ui.duration=lock_label(root,"--:--",864,504,96,16,0xbba7a0);
  lv_obj_set_style_text_align(g_lock_ui.duration,LV_TEXT_ALIGN_RIGHT,0);
}

static void lock_weather_update(void)
{
  struct glass_weather_snapshot weather;glass_weather_get(&weather);
  if(g_lock_ui.weather_valid && weather.revision==g_lock_ui.weather.revision &&
     weather.state==g_lock_ui.weather.state && weather.valid==g_lock_ui.weather.valid)return;
  g_lock_ui.weather=weather;g_lock_ui.weather_valid=true;
  const struct glass_weather_data *d=&weather.data;
  lv_label_set_text(g_lock_ui.weather_location,weather.valid&&d->location[0]?d->location:glass_tr("当地天气"));
  lv_label_set_text(g_lock_ui.weather_condition,weather.valid?d->condition:glass_tr("联网后自动获取天气"));
  lv_label_set_text(g_lock_ui.weather_icon,weather_detail_icon(weather.valid?d->condition:""));
  if(weather.valid && d->temperature!=-999)lv_label_set_text_fmt(g_lock_ui.weather_temp,"%d°",d->temperature);
  else lv_label_set_text(g_lock_ui.weather_temp,"--°");
  char humidity[24],air[24];
  if(weather.valid && d->humidity>=0)snprintf(humidity,sizeof(humidity),"%d%%",d->humidity);else strcpy(humidity,"--");
  if(weather.valid && d->aqi>=0)snprintf(air,sizeof(air),"%d",d->aqi);else strcpy(air,"--");
  lv_label_set_text_fmt(g_lock_ui.weather_meta,glass_tr("湿度 %s   ·   空气质量 %s"),humidity,air);
  lv_label_set_text(g_lock_ui.weather_report,weather.valid?d->report_time:glass_tr("天气数据尚未获取"));
  lv_label_set_text(g_lock_ui.status,weather.state==WEATHER_FETCHING?glass_tr("正在更新天气"):weather.state==WEATHER_WAIT_NET?
    glass_tr("等待网络连接"):weather.state==WEATHER_WAIT_TIME?glass_tr("等待系统校时"):weather.state==WEATHER_FAILED?
    (weather.valid?glass_tr("更新失败 · 保留上次数据"):glass_tr("暂时无法获取天气")):glass_tr("按网络位置定位 · 每小时自动更新"));
}
static void lock_weather_refresh(lv_event_t *e)
{LV_UNUSED(e);glass_weather_refresh();lock_weather_update();}
static void lock_weather_page(lv_obj_t *root)
{
  g_lock_ui.weather_icon=home_icon(root,GLASS_ICON_CLOUD_SUN,102,202,64,0xffd60a);
  g_lock_ui.weather_location=lock_label(root,glass_tr("当地天气"),238,173,448,28,0xf5f5f7);
  if(g_home_font34)lv_obj_set_style_text_font(g_lock_ui.weather_location,g_home_font34,0);
  g_lock_ui.weather_condition=lock_label(root,"",238,235,448,28,0x9b9ba2);
  g_lock_ui.weather_meta=lock_label(root,"",238,296,448,20,0x8a8a91);
  g_lock_ui.weather_report=lock_label(root,"",238,344,448,16,0x66666e);
  g_lock_ui.weather_temp=home_text(root,"--°",730,180,236,64,0xf5f5f7);
  lock_label(root,glass_tr("摄氏度"),740,278,216,20,0x8a8a91);
  g_lock_ui.status=lock_label(root,"",238,416,600,16,0x787880);
  lv_obj_t *refresh=glass_box(root,824,470,136,42,0x1c1c1e,21);
  glass_clickable(refresh,lock_weather_refresh,NULL);
  lv_obj_t *label=lock_label(refresh,glass_tr("刷新天气"),0,0,128,16,0xd4d4da);lv_obj_center(label);
}

static void lock_photo_canvas_deleted(lv_event_t *e)
{
  lv_image_cache_drop(lv_canvas_get_image(lv_event_get_target(e)));
  free(lv_event_get_user_data(e));
}
static void lock_photo_update(void)
{
  time_t now=time(NULL),shifted=now+GLASS_CLOCK_OFFSET_SECONDS;
  if(g_lock_ui.second!=now/60 || !g_lock_ui.valid) {
    g_lock_ui.second=now/60;struct tm t;
    if(gmtime_r(&shifted,&t) && t.tm_year>=120) {
      lv_label_set_text_fmt(g_lock_ui.clock,"%02d:%02d",t.tm_hour,t.tm_min);
      lv_label_set_text_fmt(g_lock_ui.photo_date,"%d.%02d.%02d",t.tm_year+1900,t.tm_mon+1,t.tm_mday);
    } else {lv_label_set_text(g_lock_ui.clock,"--:--");lv_label_set_text(g_lock_ui.photo_date,glass_tr("等待校时"));}
    g_lock_ui.valid=true;
  }
  if(g_lock_ui.photo_done)return;
  if(!g_lock_ui.photo_requested) {
    int ret=glass_lock_photo_request();
    if(ret==-EBUSY)return;
    if(ret){lv_label_set_text(g_lock_ui.photo_status,glass_tr("无法加载照片，请检查 SD 卡"));g_lock_ui.photo_done=true;return;}
    g_lock_ui.photo_requested=true;
  }
  struct glass_lock_photo photo;if(!glass_lock_photo_take(&photo))return;
  g_lock_ui.photo_done=true;
  if(photo.error || !photo.pixels) {
    free(photo.pixels);
    lv_label_set_text(g_lock_ui.photo_status,photo.error==-ENOENT?glass_tr("将照片放入 SD 卡\n/sdcard/lockscreen/photo.jpg"):glass_tr("照片无法显示\n请使用小于 512 KB 的普通 JPEG"));return;
  }
  lv_obj_t *canvas=lv_canvas_create(g_lock_ui.root);
  lv_canvas_set_buffer(canvas,photo.pixels,photo.width,photo.height,LV_COLOR_FORMAT_RGB565);
  unsigned scale=1024u*256/photo.width;if((uint64_t)photo.height*scale/256>600)scale=600u*256/photo.height;
  lv_image_set_pivot(canvas,0,0);lv_image_set_scale(canvas,scale);
  lv_obj_set_pos(canvas,(1024-(int)(photo.width*scale/256))/2,(600-(int)(photo.height*scale/256))/2);
  lv_obj_remove_flag(canvas,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(canvas,lock_photo_canvas_deleted,LV_EVENT_DELETE,photo.pixels);
  lv_obj_move_to_index(canvas,0);lv_obj_add_flag(g_lock_ui.photo_status,LV_OBJ_FLAG_HIDDEN);
}
static void lock_photo_page(lv_obj_t *root)
{
  g_lock_ui.photo_status=lock_label(root,glass_tr("正在读取 SD 卡照片…"),162,270,700,20,0x94949c);
  lv_label_set_long_mode(g_lock_ui.photo_status,LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(g_lock_ui.photo_status,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_t *shade=glass_box(root,644,54,340,142,0x000000,24);lv_obj_set_style_bg_opa(shade,100,0);
  lv_obj_remove_flag(shade,LV_OBJ_FLAG_CLICKABLE);
  g_lock_ui.clock=glass_number(root,"--:--",660,64,300,true,0xffffff);
  lv_obj_set_style_text_align(g_lock_ui.clock,LV_TEXT_ALIGN_RIGHT,0);
  g_lock_ui.photo_date=lock_label(root,"",660,156,300,20,0xe5e5ea);
  lv_obj_set_style_text_align(g_lock_ui.photo_date,LV_TEXT_ALIGN_RIGHT,0);
  shade=glass_box(root,0,556,1024,44,0x000000,0);lv_obj_set_style_bg_opa(shade,100,0);
  lv_obj_remove_flag(shade,LV_OBJ_FLAG_CLICKABLE);
}
