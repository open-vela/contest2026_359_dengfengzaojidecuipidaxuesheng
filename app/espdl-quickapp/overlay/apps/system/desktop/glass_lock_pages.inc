/* SPDX-License-Identifier: Apache-2.0 */
static void lock_music_transport_draw(lv_event_t *e)
{
  lv_area_t a;lv_obj_get_coords(lv_event_get_target(e),&a);
  int cx=(a.x1+a.x2)/2,cy=(a.y1+a.y2)/2,kind=(intptr_t)lv_event_get_user_data(e);
  lv_layer_t *layer=lv_event_get_layer(e);
  bool playing=g_lock_ui.shown && (g_lock_ui.shown->state==MUSIC_PLAYING || g_lock_ui.shown->state==MUSIC_LOADING) && !g_lock_ui.shown->pause_requested;
  if(!kind && playing) {
    lv_draw_rect_dsc_t d;lv_draw_rect_dsc_init(&d);d.bg_color=lv_color_hex(0xfff8f4);d.radius=4;
    lv_area_t left={cx-24,cy-29,cx-8,cy+29},right={cx+8,cy-29,cx+24,cy+29};
    lv_draw_rect(layer,&d,&left);lv_draw_rect(layer,&d,&right);return;
  }
  lv_draw_triangle_dsc_t d;lv_draw_triangle_dsc_init(&d);d.bg_color=lv_color_hex(0xfff8f4);
  if(!kind) {
    d.p[0]=(lv_point_precise_t){cx-18,cy-29};d.p[1]=(lv_point_precise_t){cx-18,cy+29};d.p[2]=(lv_point_precise_t){cx+27,cy};
    lv_draw_triangle(layer,&d);
  } else for(int i=0;i<2;i++) {
    int x=cx+(i?14:-14);
    d.p[0]=(lv_point_precise_t){x-kind*13,cy-20};d.p[1]=(lv_point_precise_t){x-kind*13,cy+20};d.p[2]=(lv_point_precise_t){x+kind*13,cy};
    if(kind<0){lv_point_precise_t point=d.p[0];d.p[0]=d.p[1];d.p[1]=point;}
    lv_draw_triangle(layer,&d);
  }
}

/* Album art is the one visual focus; controls share a quiet baseline. */
static void lock_music_page(lv_obj_t *root)
{
  lv_obj_t *art=glass_box(root,96,94,368,368,0x4f3b42,24);
  lv_obj_remove_flag(art,LV_OBJ_FLAG_CLICKABLE);
  g_lock_ui.cover.host=art;g_lock_ui.cover.owner=GLASS_COVER_LOCK;
  lv_obj_t *disc=glass_box(art,52,52,264,264,0x2b262e,132);
  lv_obj_remove_flag(disc,LV_OBJ_FLAG_CLICKABLE);
  for(int radius=100;radius>=68;radius-=16){
    lv_obj_t *groove=glass_box(disc,132-radius,132-radius,radius*2,radius*2,0x000000,radius);
    lv_obj_set_style_bg_opa(groove,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(groove,1,0);
    lv_obj_set_style_border_color(groove,lv_color_hex(0x59464f),0);
    lv_obj_remove_flag(groove,LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_t *ring=glass_box(disc,104,104,56,56,0xc9a57d,28);
  lv_obj_remove_flag(ring,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *note=glass_symbol(ring,LV_SYMBOL_AUDIO,0,0,0x3d2e30);lv_obj_center(note);
  g_lock_ui.status=lock_label(root,"",520,94,424,16,0x7d8792);
  g_lock_ui.title=lock_label(root,"",520,154,424,28,0xfff6f0);
  if(g_home_font34)lv_obj_set_style_text_font(g_lock_ui.title,lock_font(g_home_font34),0);
  lv_obj_set_height(g_lock_ui.title,48);
  lv_obj_set_style_text_align(g_lock_ui.title,LV_TEXT_ALIGN_CENTER,0);
  g_lock_ui.artist=lock_label(root,"",520,208,424,20,0xc7b6b0);
  lv_obj_set_style_text_align(g_lock_ui.artist,LV_TEXT_ALIGN_CENTER,0);
  g_lock_ui.lyric[1]=lock_label(root,"",520,264,424,20,0xc7b6b0);
  lv_obj_set_style_text_align(g_lock_ui.lyric[1],LV_TEXT_ALIGN_CENTER,0);
  g_lock_ui.prev=lock_button(root,LV_SYMBOL_PREV,544,310,64,lock_skip,(void *)-1);
  lv_obj_t *play=lock_button(root,LV_SYMBOL_PLAY,692,300,80,lock_toggle,NULL);
  g_lock_ui.play=lv_obj_get_child(play,0);
  g_lock_ui.next=lock_button(root,LV_SYMBOL_NEXT,856,310,64,lock_skip,(void *)1);
  lv_obj_t *buttons[]={g_lock_ui.prev,play,g_lock_ui.next};
  for(unsigned i=0;i<3;i++){
    lv_obj_set_style_bg_opa(buttons[i],LV_OPA_TRANSP,0);
    lv_obj_add_flag(lv_obj_get_child(buttons[i],0),LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(buttons[i],lock_music_transport_draw,LV_EVENT_DRAW_MAIN,(void *)(intptr_t)(i==0?-1:i==2?1:0));
  }
  g_lock_ui.progress=lv_bar_create(root);lv_obj_remove_style_all(g_lock_ui.progress);
  lv_obj_set_pos(g_lock_ui.progress,520,444);lv_obj_set_size(g_lock_ui.progress,424,8);
  lv_bar_set_range(g_lock_ui.progress,0,1000);lv_obj_remove_flag(g_lock_ui.progress,LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(g_lock_ui.progress,lv_color_hex(0x8b7970),LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_lock_ui.progress,LV_OPA_COVER,LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_lock_ui.progress,lv_color_hex(0xf3e7db),LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(g_lock_ui.progress,LV_OPA_COVER,LV_PART_INDICATOR);
  lv_obj_set_style_radius(g_lock_ui.progress,4,LV_PART_MAIN);
  lv_obj_set_style_radius(g_lock_ui.progress,4,LV_PART_INDICATOR);
  g_lock_ui.elapsed=lock_label(root,"00:00",520,464,96,16,0xc2aaa0);
  g_lock_ui.duration=lock_label(root,"--:--",848,464,96,16,0xc2aaa0);
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
  if(weather.valid && d->temperature!=-999)lv_label_set_text_fmt(g_lock_ui.weather_temp,"%d",d->temperature);
  else lv_label_set_text(g_lock_ui.weather_temp,"--");
  const char *temperature=lv_label_get_text(g_lock_ui.weather_temp);
  int width=lv_text_get_width(temperature,strlen(temperature),lv_obj_get_style_text_font(g_lock_ui.weather_temp,0),0);
  lv_obj_set_x(g_lock_ui.weather_degree,112+width+12);
  char humidity[24],air[24];
  if(weather.valid && d->humidity>=0)snprintf(humidity,sizeof(humidity),"%d%%",d->humidity);else strcpy(humidity,"--");
  if(weather.valid && d->aqi>=0)snprintf(air,sizeof(air),"%d",d->aqi);else strcpy(air,"--");
  lv_label_set_text_fmt(g_lock_ui.weather_meta,glass_tr("湿度 %s   ·   空气质量 %s"),humidity,air);
  lv_label_set_text(g_lock_ui.weather_report,weather.valid?d->report_time:glass_tr("天气数据尚未获取"));
  lv_label_set_text(g_lock_ui.status,weather.state==WEATHER_FETCHING?glass_tr("正在更新天气"):weather.state==WEATHER_WAIT_NET?
    glass_tr("等待网络连接"):weather.state==WEATHER_WAIT_TIME?glass_tr("等待系统校时"):weather.state==WEATHER_FAILED?
    (weather.valid?glass_tr("更新失败 · 保留上次数据"):glass_tr("暂时无法获取天气")):"");
}
static void lock_weather_refresh(lv_event_t *e)
{LV_UNUSED(e);glass_weather_refresh();lock_weather_update();}
/* Oversized temperature balances a single weather mark and aligned text. */
static void lock_weather_page(lv_obj_t *root)
{
  g_lock_ui.weather_icon=home_icon(root,GLASS_ICON_CLOUD_SUN,786,205,64,0xffdfa0);
  lv_obj_set_style_transform_scale_x(g_lock_ui.weather_icon,384,0);
  lv_obj_set_style_transform_scale_y(g_lock_ui.weather_icon,384,0);
  g_lock_ui.weather_location=lock_label(root,glass_tr("当地天气"),112,102,700,28,0xe5e9ee);
  g_lock_ui.weather_condition=lock_label(root,"",112,378,700,20,0xc1d4df);
  g_lock_ui.weather_temp=glass_number(root,"--",112,148,600,true,0xf0f2f5);
  lv_obj_set_style_text_font(g_lock_ui.weather_temp,lock_display(),0);
  /* Draw the degree mark so every selected numeral font remains usable. */
  lv_obj_t *degree=glass_box(root,430,212,18,18,0x000000,9);g_lock_ui.weather_degree=degree;
  lv_obj_set_style_bg_opa(degree,LV_OPA_TRANSP,0);
  lv_obj_set_style_border_width(degree,2,0);
  lv_obj_set_style_border_color(degree,lv_color_hex(0xf0f2f5),0);
  lv_obj_remove_flag(degree,LV_OBJ_FLAG_CLICKABLE);
  g_lock_ui.weather_meta=lock_label(root,"",112,430,700,20,0xc1d4df);
  g_lock_ui.weather_report=lock_label(root,"",112,466,650,16,0x9fb9ca);
  g_lock_ui.status=lock_label(root,"",112,490,650,16,0x9fb9ca);
  lv_obj_t *refresh=glass_box(root,808,440,136,48,0x000000,0);
  lv_obj_set_style_bg_opa(refresh,LV_OPA_TRANSP,0);glass_clickable(refresh,lock_weather_refresh,NULL);
  lv_obj_t *label=lock_label(refresh,glass_tr("刷新天气"),0,0,136,16,0xc1d4df);
  lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);lv_obj_center(label);
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
      if(!g_lock_ui.time_valid||t.tm_yday!=g_lock_ui.local.tm_yday||t.tm_year!=g_lock_ui.local.tm_year)
        lock_date_set(g_lock_ui.photo_date,&t);
      g_lock_ui.local=t;g_lock_ui.time_valid=true;
    } else {g_lock_ui.time_valid=false;lv_label_set_text(g_lock_ui.clock,"--:--");lv_label_set_text(g_lock_ui.photo_date,glass_tr("等待校时"));}
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
    lv_label_set_text(g_lock_ui.photo_status,photo.error==-ENOENT?glass_tr("添加照片"):glass_tr("照片无法显示\n请使用小于 512 KB 的普通 JPEG"));return;
  }
  lv_obj_t *canvas=lv_canvas_create(g_lock_ui.root);
  if(!canvas){free(photo.pixels);lv_label_set_text(g_lock_ui.photo_status,glass_tr("内存不足，稍后再试"));return;}
  lv_canvas_set_buffer(canvas,photo.pixels,photo.width,photo.height,LV_COLOR_FORMAT_RGB565);
  unsigned scale=1024u*256/photo.width;if((uint64_t)photo.height*scale/256>600)scale=600u*256/photo.height;
  lv_image_set_pivot(canvas,0,0);lv_image_set_scale(canvas,scale);
  lv_obj_set_pos(canvas,(1024-(int)(photo.width*scale/256))/2,(600-(int)(photo.height*scale/256))/2);
  lv_obj_remove_flag(canvas,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(canvas,lock_photo_canvas_deleted,LV_EVENT_DELETE,photo.pixels);
  lv_obj_move_to_index(canvas,1);lv_obj_add_flag(g_lock_ui.photo_status,LV_OBJ_FLAG_HIDDEN);
}
static void lock_photo_help(lv_event_t *event)
{LV_UNUSED(event);show_toast(glass_tr("将照片放入 SD 卡\n/sdcard/lockscreen/photo.jpg"));}
static void lock_photo_page(lv_obj_t *root)
{
  g_lock_ui.photo_status=lock_label(root,glass_tr("正在读取 SD 卡照片…"),64,448,800,16,0x94949c);
  lv_label_set_long_mode(g_lock_ui.photo_status,LV_LABEL_LONG_WRAP);
  lv_obj_set_height(g_lock_ui.photo_status,LV_SIZE_CONTENT);
  lv_obj_set_style_text_align(g_lock_ui.photo_status,LV_TEXT_ALIGN_LEFT,0);
  glass_clickable(g_lock_ui.photo_status,lock_photo_help,NULL);
  /* A soft edge shade keeps text readable without a floating card. */
  lv_obj_t *shade=glass_box(root,0,0,1024,190,0x000000,0);
  lv_obj_set_style_bg_opa(shade,80,0);
  lv_obj_set_style_bg_grad_color(shade,lv_color_black(),0);
  lv_obj_set_style_bg_grad_dir(shade,LV_GRAD_DIR_VER,0);
  lv_obj_set_style_bg_grad_opa(shade,LV_OPA_TRANSP,0);
  lv_obj_remove_flag(shade,LV_OBJ_FLAG_CLICKABLE);
  g_lock_ui.clock=glass_number(root,"--:--",64,74,600,true,0xf3f0e4);
  lv_obj_set_style_text_font(g_lock_ui.clock,lock_font(g_glass.font72),0);
  g_lock_ui.photo_date=lock_label(root,"",64,48,600,20,0xe5e6d8);
  shade=glass_box(root,0,506,1024,94,0x000000,0);
  lv_obj_set_style_bg_opa(shade,LV_OPA_COVER,0);
  lv_obj_set_style_bg_main_opa(shade,LV_OPA_TRANSP,0);
  lv_obj_set_style_bg_grad_color(shade,lv_color_black(),0);
  lv_obj_set_style_bg_grad_dir(shade,LV_GRAD_DIR_VER,0);
  lv_obj_set_style_bg_grad_opa(shade,160,0);
  lv_obj_remove_flag(shade,LV_OBJ_FLAG_CLICKABLE);
}
