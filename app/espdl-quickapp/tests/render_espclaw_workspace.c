/* SPDX-License-Identifier: Apache-2.0 */
#define GLASS_LOCAL_SPEECH 1
#define UNIFIED_DEVICE_NO_MAIN
#include "render_statusbar_devices.c"
extern void review_chat_dialog_fixture(bool);
extern void review_chat_reply_fixture(const char *,bool);
extern void review_chat_phase_fixture(enum chat_phase);
extern void review_chat_error_fixture(void);
extern void review_face_available_fixture(bool);
extern void review_chat_tool_fixture(const char *,const char *);
extern void review_chat_card_fixture(unsigned);
extern void review_chat_music_card_fixture(bool);

static void workspace_frames(unsigned n)
{for(unsigned i=0;i<n;i++){lv_tick_inc(100);lv_timer_handler();usleep(1000);}}

static unsigned workspace_objects(lv_obj_t *object)
{unsigned n=1;for(unsigned i=0;i<lv_obj_get_child_count(object);i++)n+=workspace_objects(lv_obj_get_child(object,i));return n;}
static unsigned workspace_timers(void)
{unsigned n=0;for(lv_timer_t *t=lv_timer_get_next(NULL);t;t=lv_timer_get_next(t))n++;return n;}

static void workspace_send(const char *prompt)
{
  chat_ui_editor_open(NULL);assert(g_chat_ui.input);
  lv_textarea_set_text(g_chat_ui.input,prompt);chat_ui_send(NULL);
  workspace_frames(3);assert(!g_chat_ui.editor&&g_chat_ui.snapshot->count);
}

static void workspace_bounds(lv_obj_t *object)
{
  if(!object||!lv_obj_is_visible(object))return;
  if(lv_obj_has_flag(object,LV_OBJ_FLAG_CLICKABLE)&&!lv_obj_has_class(object,&lv_label_class)){
    lv_area_t area;lv_obj_get_coords(object,&area);
    assert(area.x1>=0&&area.x2<1024&&area.y1>=0&&area.y2<600);
  }
  for(unsigned i=0;i<lv_obj_get_child_count(object);i++)workspace_bounds(lv_obj_get_child(object,i));
}

static void workspace_cases(lv_display_t *display)
{
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme==0;glass_rebuild();review_chat_dialog_fixture(true);review_qpk_fixture(NULL);
    chat_interface_modes[0]=AGENT_MODE_CREATE;glass_chat_show(false);workspace_frames(4);
    assert(g_chat_ui.ball&&g_chat_ui.ball_mount&&g_chat_ui.task_title);
    assert(!review_find(g_chat_ui.root,"电脑 CLI"));
    lv_obj_update_layout(g_desktop.screen);
    assert(lv_obj_get_y(g_desktop.screen)==0&&lv_obj_get_height(g_desktop.screen)==600);
    assert(lv_obj_get_height(g_chat_ui.root)==600);
    assert(lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN));
    assert(glass_grok_current(g_chat_ui.ball)==2&&!g_chat_ui.workspace_ball_compact);
    struct glass_grok *original_ball=g_chat_ui.ball;
    workspace_bounds(g_chat_ui.root);check_visible_icons(g_chat_ui.root,0xe005);
    assert(lv_obj_has_state(g_chat_ui.preview_button,LV_STATE_DISABLED));
    assert(lv_obj_has_state(g_chat_ui.save_button,LV_STATE_DISABLED));
    review_capture(display,"workspace-start","ESPClaw · ESP 工作台");
    unsigned object_count=workspace_objects(g_chat_ui.root),timer_count=workspace_timers();
    unsigned panel_objects=workspace_objects(g_chat_ui.review_shade);
    for(unsigned i=0;i<30;i++){chat_workspace_inspect(NULL);chat_ui_refresh(NULL);chat_workspace_close(NULL);}
    assert(workspace_objects(g_chat_ui.root)==object_count&&workspace_timers()==timer_count);
    assert(g_chat_ui.ball==original_ball&&lv_obj_has_flag(g_chat_ui.review_shade,LV_OBJ_FLAG_HIDDEN));
    printf("WORKSPACE_PANEL_STABLE: theme=%u panel_objects=%u timers_before=%u timers_after=%u open_close_cycles=30 same_ball=1\n",theme,panel_objects,timer_count,workspace_timers());
    /* The mode chip is an independent target inside the composer. */
    lv_obj_t *composer=lv_obj_get_parent(g_chat_ui.action);
    lv_obj_update_layout(g_chat_ui.root);
    lv_area_t hero_ball,compose_area,hero_title;
    lv_obj_get_coords(g_chat_ui.ball_mount,&hero_ball);lv_obj_get_coords(composer,&compose_area);
    lv_obj_get_coords(lv_obj_get_child(g_chat_ui.messages,0),&hero_title);
    assert(hero_ball.x1+hero_ball.x2==compose_area.x1+compose_area.x2);
    assert(hero_ball.x1+hero_ball.x2==hero_title.x1+hero_title.x2);
    lv_obj_t *mode=review_callback_find(composer,chat_ui_modes);assert(mode);
    lv_obj_send_event(mode,LV_EVENT_CLICKED,NULL);assert(g_chat_ui.mode_panel&&!g_chat_ui.editor);
    review_capture(display,"workspace-modes","ESPClaw · 明确的模式边界");chat_ui_mode_close(NULL);
    lv_obj_send_event(composer,LV_EVENT_CLICKED,NULL);assert(g_chat_ui.editor);
    lv_textarea_set_text(g_chat_ui.input,"做一个专注计时器，支持开始、暂停和重置。");
    workspace_bounds(g_chat_ui.editor);
    review_capture(display,"workspace-input","ESPClaw · 中文任务输入");chat_ui_editor_close(NULL);
    assert(strstr(chat_draft,"专注计时器"));
    chat_ui_action(NULL);workspace_frames(3);assert(g_chat_ui.live_bubble);
    assert(g_chat_ui.ball==original_ball&&g_chat_ui.workspace_ball_compact);
    assert(lv_obj_get_width(g_chat_ui.ball_mount)==64&&lv_obj_get_height(g_chat_ui.ball_mount)==64);
    assert(glass_grok_current(g_chat_ui.ball)==30);
    review_capture(display,"workspace-working","ESPClaw · 处理任务");
    g_glass.reduced_motion=false;
    chat_ui_trace_update(&g_chat_ui.snapshot->turns[0]);lv_opa_t first=lv_obj_get_style_text_opa(g_chat_ui.waiting,0);
    lv_tick_inc(500);chat_ui_trace_update(&g_chat_ui.snapshot->turns[0]);
    assert(first!=lv_obj_get_style_text_opa(g_chat_ui.waiting,0));
    g_glass.reduced_motion=true;chat_ui_trace_update(&g_chat_ui.snapshot->turns[0]);
    assert(lv_obj_get_style_text_opa(g_chat_ui.waiting,0)==LV_OPA_COVER);
    review_chat_tool_fixture("qpk_read_example","正在读取验证过的示例");chat_ui_refresh(NULL);
    review_chat_tool_fixture("qpk_write_draft","正在编写应用草稿");chat_ui_refresh(NULL);
    review_capture(display,"workspace-tool-row","ESPClaw · 真实工具调用状态行");
    unsigned activity_objects=workspace_objects(g_chat_ui.root),activity_timers=workspace_timers();
    chat_ui_trace_toggle(NULL);assert(g_chat_ui.trace_expanded);
    assert(strstr(lv_label_get_text(g_chat_ui.trace_details),"qpk_read_example")&&strstr(lv_label_get_text(g_chat_ui.trace_details),"qpk_write_draft"));
    review_capture(display,"workspace-tool-detail","ESPClaw · 可展开的调用过程");
    for(unsigned i=0;i<20;i++)chat_ui_trace_toggle(NULL);
    assert(workspace_objects(g_chat_ui.root)==activity_objects&&workspace_timers()==activity_timers);
    chat_ui_trace_toggle(NULL);assert(!g_chat_ui.trace_expanded);
    review_chat_reply_fixture("先写一个最小入口。\n\n```ja",false);chat_ui_refresh(NULL);
    assert(g_chat_ui.live_count==1&&!g_chat_ui.live_parts[0].code);
    const char *code_start="先写一个最小入口。\n\n```javascript\nconst app = {\n  title: \"专注计时器\",\n  seconds: 25 * 60,\n";
    review_chat_reply_fixture(code_start,false);chat_ui_refresh(NULL);
    assert(g_chat_ui.live_count==2&&g_chat_ui.live_parts[1].code);
    struct chat_output_part *code=&g_chat_ui.live_parts[1];lv_obj_t *code_root=code->root,*code_text=code->text;
    assert(strstr(lv_label_get_text(code->caption),"JavaScript")&&strstr(lv_label_get_text(code->caption),"生成中"));
    assert(!strstr(lv_label_get_text(code->text),"```"));
    review_capture(display,"workspace-code-stream","ESPClaw · 代码逐段显示与行号");
    char long_code[1400];snprintf(long_code,sizeof(long_code),"%s  // ",code_start);
    size_t used=strlen(long_code);memset(long_code+used,'x',180);strcpy(long_code+used+180,"\n};\n```\n\n代码已返回，稍后可预览草稿。");
    review_chat_reply_fixture(long_code,false);chat_ui_refresh(NULL);
    assert(code->root==code_root&&code->text==code_text&&!strstr(lv_label_get_text(code->caption),"生成中"));
    lv_obj_update_layout(code->viewport);assert(lv_obj_get_scroll_right(code->viewport)>0);
    review_capture(display,"workspace-code-wide","ESPClaw · 长代码行横向滚动");
    lv_obj_scroll_to_x(code->viewport,120,LV_ANIM_OFF);review_capture(display,"workspace-code-scroll","ESPClaw · 代码水平滚动");
    printf("CHAT_RENDER_REUSE_OK: theme=%u code_root_reused=1 code_label_reused=1 trace_toggle_no_growth=1 original_ball=%d\n",theme,g_chat_ui.ball==original_ball);
    review_chat_reply_fixture("## 专注计时器\n先完成 **核心功能**，入口为 `main`。\n- 开始、暂停和重置\n- 退出后保留计时\n> 先预览，确认后安装。",false);chat_ui_refresh(NULL);
    assert(g_chat_ui.live_parts[0].rich&&lv_obj_check_type(g_chat_ui.live_parts[0].root,&lv_spangroup_class));
    review_capture(display,"workspace-markdown","ESPClaw · Markdown 正文");
    lv_obj_t *md_root=g_chat_ui.live_parts[0].root;
    lv_span_t *md_span=lv_spangroup_get_child(md_root,0);
    unsigned md_objects=workspace_objects(g_chat_ui.root);
    for(unsigned i=0;i<10;i++)chat_ui_live();
    assert(g_chat_ui.live_parts[0].root==md_root&&lv_spangroup_get_child(md_root,0)==md_span&&workspace_objects(g_chat_ui.root)==md_objects);
    unsigned rebuilds=g_chat_ui.rebuilds;
    for(unsigned i=0;i<6;i++){
      char text[256];snprintf(text,sizeof(text),"我会先为你生成一个专注计时器草稿。\n\n已完成 %u 项：开始、暂停和重置控件。",i+1);
      review_chat_reply_fixture(text,false);chat_ui_refresh(NULL);
    }
    assert(g_chat_ui.rebuilds==rebuilds&&g_chat_ui.live_updates>=6);
    assert(g_chat_ui.ball==original_ball&&glass_grok_current(g_chat_ui.ball)==39);
    assert(!strcmp(lv_label_get_text(g_chat_ui.task_title),"做一个专注计时器，支持开始、暂停和重置。"));
    review_capture(display,"workspace-stream","ESPClaw · 原位更新流式回复");
    review_chat_reply_fixture("计时器草稿已生成。\n\n支持开始、暂停与重置，离开应用后保留计时状态。点右上角「草稿」预览效果，确认后再安装。",true);
    review_qpk_fixture(g_hello_qpk_js);chat_ui_refresh(NULL);
    assert(!lv_obj_has_state(g_chat_ui.preview_button,LV_STATE_DISABLED));
    assert(lv_obj_has_state(g_chat_ui.save_button,LV_STATE_DISABLED));
    lv_obj_update_layout(g_chat_ui.root);chat_ui_ball_position(NULL);
    lv_area_t ball_area,reply_area;lv_obj_get_coords(g_chat_ui.ball_mount,&ball_area);lv_obj_get_coords(g_chat_ui.ball_anchor,&reply_area);
    assert(lv_obj_is_visible(g_chat_ui.ball_mount)&&ball_area.x2<reply_area.x1);
    review_capture(display,"workspace-reply","ESPClaw · 小球保留在回复旁边");
    assert(g_chat_ui.trace_root&&g_chat_ui.live_bubble);
    int reply_y=lv_obj_get_y(g_chat_ui.live_bubble);
    chat_ui_trace_toggle(NULL);assert(lv_obj_get_y(g_chat_ui.live_bubble)>reply_y);
    review_capture(display,"workspace-complete-process","ESPClaw · 完成后查看调用记录");
    chat_ui_trace_toggle(NULL);assert(lv_obj_get_y(g_chat_ui.live_bubble)==reply_y);
    chat_workspace_inspect(NULL);assert(lv_obj_is_visible(g_chat_ui.preview_button));
    review_capture(display,"workspace-draft","ESPClaw · 草稿待预览");
    chat_ui_preview_app(NULL);assert(chat_qpk_preview_active);
    review_capture(display,"workspace-preview","ESPClaw · 真实 QPK 预览");
    chat_qpk_preview_back();workspace_frames(3);
    assert(!lv_obj_has_state(g_chat_ui.save_button,LV_STATE_DISABLED));
    chat_workspace_inspect(NULL);
    review_capture(display,"workspace-ready","ESPClaw · 预览后可安装");
    chat_ui_save_app(NULL);assert(g_chat_ui.confirm);
    review_capture(display,"workspace-install","ESPClaw · 选择安装位置");chat_ui_confirm_close(NULL);
    chat_workspace_close(NULL);
    workspace_send("请把按钮做得更清晰。");review_chat_error_fixture();chat_ui_refresh(NULL);
    assert(!strcmp(lv_label_get_text(g_chat_ui.task_title),"请把按钮做得更清晰。"));
    assert(review_find(g_chat_ui.messages,"重新发送"));
    review_capture(display,"workspace-error","ESPClaw · 错误与重试");
    /* Voice entry preserves the selected semantic mode without starting ASR. */
    review_face_available_fixture(false);chat_workspace_voice(NULL);
    assert(g_chat_ui.ball_mode&&g_chat_ui.mode==AGENT_MODE_CREATE&&!g_chat_ui.voice_session);
    glass_chat_show(false);workspace_frames(3);
    chat_ui_new(NULL);assert(g_chat_ui.confirm);
    review_capture(display,"workspace-new","ESPClaw · 新任务确认");chat_ui_confirm_close(NULL);
    chat_ui_config(NULL);assert(chat_portal_return_ball==false);chat_ui_return_from_portal(NULL);
    assert(!g_chat_ui.ball_mode&&g_chat_ui.mode==AGENT_MODE_CREATE);
    panel_hide(NULL);review_qpk_fixture(NULL);
    chat_interface_modes[0]=AGENT_MODE_ASSISTANT;glass_chat_show(false);workspace_frames(3);
    assert(lv_obj_has_flag(g_chat_ui.preview_button,LV_OBJ_FLAG_HIDDEN));
    review_capture(display,"workspace-assistant","ESPClaw · 助手模式");
    /* Smart home results render as device cards inside the dialog only. */
    workspace_send("把客厅的灯打开。");review_chat_tool_fixture("mcp_call_tool","正在调用 MCP 工具");
    review_chat_card_fixture(4);chat_ui_refresh(NULL);
    assert(g_chat_ui.cards_root&&g_chat_ui.cards_h==CHAT_CARD_H*2+CHAT_CARD_GAP);
    assert(lv_obj_get_child_count(g_chat_ui.cards_root)==4&&g_chat_ui.live_bubble);
    lv_obj_update_layout(g_chat_ui.messages);
    assert(lv_obj_get_y(g_chat_ui.cards_root)>=lv_obj_get_y(g_chat_ui.trace_root));
    assert(lv_obj_get_y(g_chat_ui.live_bubble)>=lv_obj_get_y(g_chat_ui.cards_root)+g_chat_ui.cards_h);
    review_capture(display,"workspace-home-cards","ESPClaw · 智能家居结果卡片");
    review_chat_reply_fixture("已打开客厅主灯，亮度 80%。",true);chat_ui_refresh(NULL);
    assert(g_chat_ui.cards_root&&lv_obj_get_child_count(g_chat_ui.cards_root)==4);
    unsigned card_blocks=0;
    for(unsigned c=0;c<lv_obj_get_child_count(g_chat_ui.messages);c++){
      lv_obj_t *object=lv_obj_get_child(g_chat_ui.messages,c);
      if(lv_obj_get_width(object)==700&&lv_obj_get_height(object)==g_chat_ui.cards_h)card_blocks++;
    }
    assert(card_blocks==1);
    lv_font_glyph_dsc_t glyph={0};
    assert(lv_font_get_glyph_dsc(glass_icon_font(24),&glyph,0xe015,0)&&!glyph.is_placeholder);
    assert(lv_font_get_glyph_dsc(glass_icon_font(24),&glyph,0xe027,0)&&!glyph.is_placeholder);
    review_capture(display,"workspace-home-cards-done","ESPClaw · 卡片随回复保留");
    review_chat_card_fixture(0);
    workspace_send("播放 Beyond 的海阔天空。");
    assert(!strcmp(lv_label_get_text(g_chat_ui.task_title),"播放 Beyond 的海阔天空。"));
    review_chat_tool_fixture("mcp_call_tool","正在调用音乐工具");
    review_chat_music_card_fixture(false);chat_ui_refresh(NULL);
    assert(g_chat_ui.cards_root&&lv_obj_get_child_count(g_chat_ui.cards_root)==1);
    assert(g_desktop.chat_page&&!qpk_runtime_running());
    review_capture(display,"workspace-music-request","ESPClaw · 音乐请求卡片");
    review_chat_music_card_fixture(true);review_chat_reply_fixture("",true);chat_ui_refresh(NULL);
    assert(g_chat_ui.cards_root&&lv_obj_get_child_count(g_chat_ui.cards_root)==1);
    assert(g_desktop.chat_page&&!qpk_runtime_running());
    review_capture(display,"workspace-music-playing","ESPClaw · 音乐播放卡片");
    panel_hide(NULL);
    chat_interface_modes[0]=AGENT_MODE_CHAT;glass_chat_show(false);workspace_frames(3);
    assert(lv_obj_has_flag(g_chat_ui.save_button,LV_OBJ_FLAG_HIDDEN));
    review_capture(display,"workspace-chat","ESPClaw · 聊天模式");panel_hide(NULL);
    lv_obj_update_layout(g_desktop.screen);
    assert(!chat_display.active&&lv_obj_get_y(g_desktop.screen)==GLASS_STATUS_HEIGHT);
    assert(lv_obj_get_height(g_desktop.screen)==GLASS_CONTENT_HEIGHT);
    assert(!lv_obj_has_flag(g_desktop.statusbar,LV_OBJ_FLAG_HIDDEN));
  }
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
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);
  g_glass.reduced_motion=true;g_glass.temperature_c=-999;g_glass.humidity_pct=-1;g_glass.pm25=-1;
  g_desktop.screen=glass_box(lv_screen_active(),0,GLASS_STATUS_HEIGHT,1024,GLASS_CONTENT_HEIGHT,0,0);
  lv_obj_remove_flag(g_desktop.screen,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_text_font(g_desktop.screen,zh_font(20),0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,GLASS_CONTENT_HEIGHT,0,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  glass_edge_back_create();glass_status_create();workspace_cases(display);
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  printf("ESPCLAW_WORKSPACE_OK: %u native captures; composer/mode targets, task input, streaming reuse, draft preview/install gates, retries, voice/context, settings return, themes\n",review_count);
  return 0;
}
