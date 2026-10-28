/* SPDX-License-Identifier: Apache-2.0 */
/* Three live LRC effects; no SD frame-bank playback. */
#define MUSIC_PV_PARTS 3
#define MUSIC_PV_LAYOUTS 3
struct music_pv_ui {
  lv_obj_t *root,*title,*counter,*hint,*next,*rule,*marker,*sweep,*style_button,*translation;
  lv_obj_t *groups[MUSIC_PV_PARTS],*text[MUSIC_PV_PARTS];
  lv_timer_t *timer;
  struct music_lyrics_window window;
  char source[MUSIC_LYRIC_TEXT_MAX],parts[MUSIC_PV_PARTS][MUSIC_LYRIC_TEXT_MAX];
  char translation_source[MUSIC_LYRIC_TEXT_MAX];
  unsigned track,layout,count,start,end,last_position,style_index;
  unsigned glyphs[MUSIC_PV_PARTS],shown[MUSIC_PV_PARTS],width[MUSIC_PV_PARTS];
  unsigned updates,plans,plan_max_ms,bucket,opacity;
  uint32_t background,accent,foreground,secondary;
  int current;
  bool visible,valid,last_reduced;
};
static const char *const music_pv_names[MUSIC_PV_LAYOUTS]={"聚焦 · 1/3","强调 · 2/3","逐字 · 3/3"};

static void music_pv_text(lv_obj_t *label,const char *text)
{if(strcmp(lv_label_get_text(label),text))lv_label_set_text(label,text);}

static const char *music_pv_advance(const char *s,unsigned count)
{while(*s&&count--){s++;while(((unsigned char)*s&0xc0)==0x80)s++;}return s;}

static unsigned music_pv_glyphs(const char *s)
{unsigned n=0;for(;*s;s++)if(((unsigned char)*s&0xc0)!=0x80)n++;return n;}

static const lv_font_t *music_pv_font(unsigned size)
{
  return music_lyric_font(size);
}

static bool music_pv_punctuation(const char *s)
{
  if(*s&&strchr(",.!?;:)]",*s))return true;
  static const char *const marks[]={"，","。","、","！","？","；","：","）","】","》","”"};
  for(unsigned i=0;i<sizeof(marks)/sizeof(marks[0]);i++)if(!strncmp(s,marks[i],3))return true;
  return false;
}

static void music_pv_split(struct music_pv_ui *p,const char *text)
{
  unsigned left=music_pv_glyphs(text);p->count=left>24?3:left>10?2:1;
  /* Emphasize the final phrase, without claiming semantic/beat analysis. */
  if(p->layout==1&&left>=6&&left<=24)p->count=2;
  const char *s=text;
  for(unsigned i=0;i<p->count;i++){
    unsigned take=p->layout==1&&p->count==2&&left>=6&&left<=24&&i==0?left-4:(left+p->count-i-1)/(p->count-i);
    const char *begin=s;s=music_pv_advance(s,take);
    while(*s&&music_pv_punctuation(s))s=music_pv_advance(s,1);
    size_t bytes=(size_t)(s-begin);memcpy(p->parts[i],begin,bytes);p->parts[i][bytes]=0;
    p->glyphs[i]=music_pv_glyphs(p->parts[i]);left-=p->glyphs[i];
    if(!*s){p->count=i+1;break;}
  }
}

static void music_pv_plan(struct music_pv_ui *p,const char *text,const char *translation,unsigned track,int current)
{
  uint32_t began=lv_tick_get();(void)track;(void)current;
  static const uint32_t light[]={0x8b465a,0x27645b,0x805f26};
  static const uint32_t dark[]={0xe7b0c2,0x98d7c3,0xe9c586};
  p->layout=p->style_index%MUSIC_PV_LAYOUTS;
  p->foreground=g_desktop.light_theme?0x252a2c:0xf5f0e8;
  p->secondary=g_desktop.light_theme?0x66686a:0xa7acae;
  p->background=g_desktop.light_theme?0xf6f2eb:0x101619;
  p->accent=g_desktop.light_theme?light[p->layout]:dark[p->layout];
  music_pv_split(p,text);
  lv_obj_t *secondary[]={p->title,p->counter,p->next,p->hint,p->translation};
  for(unsigned i=0;i<5;i++)lv_obj_set_style_text_color(secondary[i],lv_color_hex(p->secondary),0);
  bool bilingual=translation[0]!=0;
  music_pv_text(p->translation,translation);
  lv_obj_set_style_text_font(p->translation,music_lyric_font(20),0);
  if(bilingual)lv_obj_remove_flag(p->translation,LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(p->translation,LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_bg_color(p->rule,lv_color_hex(p->secondary),0);
  lv_obj_set_style_bg_color(p->sweep,lv_color_hex(p->accent),0);
  lv_obj_set_style_bg_color(p->marker,lv_color_hex(p->accent),0);
  music_pv_text(lv_obj_get_child(p->style_button,0),glass_tr(music_pv_names[p->layout]));
  lv_obj_set_style_text_color(lv_obj_get_child(p->style_button,0),lv_color_hex(p->accent),0);
  for(unsigned i=0;i<MUSIC_PV_PARTS;i++){
    if(i>=p->count){lv_obj_add_flag(p->groups[i],LV_OBJ_FLAG_HIDDEN);continue;}
    lv_obj_remove_flag(p->groups[i],LV_OBJ_FLAG_HIDDEN);
    unsigned height=p->count==3?80:108;
    int y=p->count==1?140:p->count==2?92+(int)i*112:76+(int)i*80;
    if(bilingual&&p->count==3){height=64;y=76+(int)i*68;}
    else if(bilingual&&p->count==2){height=84;y=90+(int)i*92;}
    int x=p->layout==1?64+(int)i*24:64;
    unsigned width=p->layout==1?848-i*24:848;
    lv_obj_set_pos(p->groups[i],x,y);lv_obj_set_size(p->groups[i],width,height);
    unsigned sizes[]={p->layout==2?48:p->layout==1&&i+1<p->count?28:72,48,28,20};
    const lv_font_t *font=NULL;lv_point_t measure;
    for(unsigned f=0;f<4;f++){
      font=music_pv_font(sizes[f]);lv_text_get_size(&measure,p->parts[i],font,0,0,width,LV_TEXT_FLAG_NONE);
      if(measure.y<=(int)height)break;
    }
    p->width[i]=width;p->shown[i]=UINT_MAX;
    music_pv_text(p->text[i],p->parts[i]);
    lv_obj_set_width(p->text[i],width);lv_obj_set_height(p->text[i],measure.y>(int)height?(int)height:measure.y);
    lv_obj_set_style_text_font(p->text[i],font,0);
    lv_obj_set_style_text_align(p->text[i],p->layout==0?LV_TEXT_ALIGN_CENTER:LV_TEXT_ALIGN_LEFT,0);
    lv_obj_set_style_text_color(p->text[i],lv_color_hex(p->layout==1?(i+1==p->count?p->accent:p->secondary):p->foreground),0);
    lv_obj_align(p->text[i],LV_ALIGN_LEFT_MID,0,0);
  }
  snprintf(p->source,sizeof(p->source),"%s",text);
  snprintf(p->translation_source,sizeof(p->translation_source),"%s",translation);
  p->bucket=UINT_MAX;p->opacity=UINT_MAX;p->last_position=UINT_MAX;p->plans++;
  unsigned elapsed=lv_tick_elaps(began);if(elapsed>p->plan_max_ms)p->plan_max_ms=elapsed;
}

/* No bank files, frame buffers or worker threads are allocated by live effects. */

static void music_pv_refresh(struct music_pv_ui *p)
{
  if(!p->visible||!glass_ui_can_paint(p->root))return;
  struct music_now_playing now;glass_music_now_playing(&now);
  music_pv_text(p->title,now.name[0]?now.name:glass_tr("音乐 · 文字 PV"));
  glass_music_lyrics_read(p->current<0?0:(unsigned)p->current,&p->window);
  if(p->window.track_id==now.track_id&&p->window.current>=0&&
     ((unsigned)p->window.current<p->window.first||(unsigned)p->window.current>=p->window.first+p->window.count))
    glass_music_lyrics_read((unsigned)p->window.current,&p->window);
  int index=p->window.current;
  bool ready=p->window.track_id==now.track_id&&p->window.state==MUSIC_LYRICS_READY&&index>=0&&
    (unsigned)index>=p->window.first&&(unsigned)index<p->window.first+p->window.count&&
    (now.state==MUSIC_PLAYING||now.state==MUSIC_PAUSED);
  if(!ready){
    const char *hint=now.state==MUSIC_ERROR?glass_tr("播放失败"):now.state==MUSIC_STOPPED?glass_tr("已停止"):
      now.state==MUSIC_ENDED?glass_tr("播放结束"):now.state==MUSIC_IDLE?glass_tr("选择一首歌开始"):
      now.state==MUSIC_LOADING?glass_tr("正在加载音乐…"):now.lyrics_state==MUSIC_LYRICS_LOADING?glass_tr("正在获取歌词…"):
      now.lyrics_state==MUSIC_LYRICS_READY?glass_tr("前奏 · 等待歌词"):glass_tr("暂无歌词 · 仍可继续听歌");
    music_pv_text(p->hint,hint);lv_obj_remove_flag(p->hint,LV_OBJ_FLAG_HIDDEN);
    for(unsigned i=0;i<MUSIC_PV_PARTS;i++)lv_obj_add_flag(p->groups[i],LV_OBJ_FLAG_HIDDEN);
    music_pv_text(p->counter,"");music_pv_text(p->next,"");
    music_pv_text(p->translation,"");lv_obj_add_flag(p->translation,LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(p->sweep,LV_OBJ_FLAG_HIDDEN);lv_obj_add_flag(p->marker,LV_OBJ_FLAG_HIDDEN);
    p->valid=false;return;
  }
  unsigned slot=(unsigned)index-p->window.first;
  const char *text=p->window.lines[slot][0]?p->window.lines[slot]:"· · ·";
  const char *translation=g_glass.music_original_only?"":p->window.translations[slot];
  if(!p->valid||p->track!=now.track_id||p->current!=index||strcmp(p->source,text)||strcmp(p->translation_source,translation)){
    music_pv_plan(p,text,translation,now.track_id,index);p->track=now.track_id;p->current=index;p->valid=true;
    char counter[32];snprintf(counter,sizeof(counter),"%03u / %03u",(unsigned)index+1,p->window.total);
    music_pv_text(p->counter,counter);music_pv_text(p->next,slot+1<p->window.count?p->window.lines[slot+1]:"");
    lv_obj_add_flag(p->hint,LV_OBJ_FLAG_HIDDEN);lv_obj_remove_flag(p->sweep,LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(p->marker,LV_OBJ_FLAG_HIDDEN);
  }
  p->start=p->window.start_ms[slot];p->end=p->window.end_ms[slot];
  if(p->last_position==now.position_ms&&p->last_reduced==g_glass.reduced_motion)return;
  p->last_position=now.position_ms;p->last_reduced=g_glass.reduced_motion;
  unsigned local=now.position_ms>p->start?now.position_ms-p->start:0;
  unsigned span=p->end>p->start?p->end-p->start:0;
  unsigned progress=span?(unsigned)((uint64_t)(local>span?span:local)*32/span):32;
  if(g_glass.reduced_motion)progress=32;
  /* Only small accent geometry moves continuously. Large text stays in place. */
  if(p->bucket!=progress){
    p->bucket=progress;unsigned width=1+progress*847/32;
    lv_obj_set_width(p->sweep,width);lv_obj_set_x(p->marker,64+width-1);p->updates++;
  }
  unsigned entrance=span&&span<1200?span/3:320;if(!entrance)entrance=1;
  unsigned opacity=p->layout!=0||g_glass.reduced_motion||local>=entrance?255:96+(local*4/entrance)*159/4;
  if(p->opacity!=opacity){
    p->opacity=opacity;for(unsigned i=0;i<p->count;i++)lv_obj_set_style_opa(p->groups[i],opacity,0);p->updates++;
  }
  if(p->layout==2){
    unsigned duration=span&&span<2400?span/2:1200;if(!duration)duration=1;
    unsigned total=0;for(unsigned i=0;i<p->count;i++)total+=p->glyphs[i];
    /* Line-timed reveal, not word-level karaoke alignment. Bound repaint rate. */
    unsigned typed=g_glass.reduced_motion||local>=duration?total:(uint64_t)total*(local/80*80)/duration;
    for(unsigned i=0;i<p->count;i++){
      unsigned n=typed>p->glyphs[i]?p->glyphs[i]:typed;typed-=n;
      if(p->shown[i]==n)continue;p->shown[i]=n;
      const char *end=music_pv_advance(p->parts[i],n);size_t bytes=(size_t)(end-p->parts[i]);
      char visible[MUSIC_LYRIC_TEXT_MAX];memcpy(visible,p->parts[i],bytes);visible[bytes]=0;
      music_pv_text(p->text[i],visible);p->updates++;
    }
  }
}
static void music_pv_tick(lv_timer_t *timer){music_pv_refresh(lv_timer_get_user_data(timer));}
static void music_pv_style_next(lv_event_t *event)
{
  struct music_pv_ui *p=lv_event_get_user_data(event);
  p->style_index=(p->style_index+1)%MUSIC_PV_LAYOUTS;p->valid=false;
  music_pv_text(lv_obj_get_child(p->style_button,0),glass_tr(music_pv_names[p->style_index]));
  music_ui_refresh(NULL);
}
static void music_pv_create(struct music_pv_ui *p,lv_obj_t *parent)
{
  p->current=-1;p->root=glass_box(parent,24,78,976,394,g_music_ui.background,0);
  lv_obj_set_style_bg_opa(p->root,LV_OPA_TRANSP,0);
  lv_obj_remove_flag(p->root,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
  uint32_t sub=g_desktop.light_theme?0x66686a:0xa7acae;
  p->title=music_ui_text(p->root,"",44,20,680,16,sub);
  p->counter=music_ui_text(p->root,"",806,20,140,16,sub);
  p->rule=glass_box(p->root,44,60,888,1,sub,0);lv_obj_set_style_bg_opa(p->rule,LV_OPA_20,0);
  p->sweep=glass_box(p->root,64,334,1,2,g_music_ui.accent,1);
  p->marker=glass_box(p->root,64,331,6,8,g_music_ui.accent,3);
  lv_obj_remove_flag(p->sweep,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(p->marker,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
  for(unsigned i=0;i<MUSIC_PV_PARTS;i++){
    p->groups[i]=glass_box(p->root,64,76+i*80,848,80,g_music_ui.background,0);
    lv_obj_set_style_bg_opa(p->groups[i],LV_OPA_TRANSP,0);
    lv_obj_remove_flag(p->groups[i],LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_CLICKABLE);
    p->text[i]=music_ui_text(p->groups[i],"",0,0,848,48,g_music_ui.primary);
    lv_label_set_long_mode(p->text[i],LV_LABEL_LONG_WRAP);
  }
  p->hint=music_ui_text(p->root,"",44,178,888,28,sub);lv_obj_set_style_text_align(p->hint,LV_TEXT_ALIGN_CENTER,0);
  p->translation=music_ui_text(p->root,"",64,284,848,20,sub);
  lv_obj_set_height(p->translation,42);lv_label_set_long_mode(p->translation,LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(p->translation,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_add_flag(p->translation,LV_OBJ_FLAG_HIDDEN);
  p->next=music_ui_text(p->root,"",64,356,848,16,sub);
  p->style_button=music_ui_button(parent,glass_tr(music_pv_names[0]),832,16,168,44,g_music_ui.surface,music_pv_style_next,p,false);
  lv_obj_set_style_bg_opa(p->style_button,LV_OPA_TRANSP,0);lv_obj_add_flag(p->style_button,LV_OBJ_FLAG_HIDDEN);
  p->timer=lv_timer_create(music_pv_tick,50,p);lv_timer_pause(p->timer);lv_obj_add_flag(p->root,LV_OBJ_FLAG_HIDDEN);
}
