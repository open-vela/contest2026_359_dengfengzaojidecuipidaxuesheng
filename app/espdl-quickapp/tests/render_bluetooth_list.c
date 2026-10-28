/* SPDX-License-Identifier: Apache-2.0 */
#define main all_ui_reference_main
#include "render_all_ui.c"
#undef main
#include <unistd.h>
extern void review_ble_snapshot_fixture(const struct glass_ble_state *);
extern void review_ble_error_fixture(int);
extern void review_ble_pair_fixture(unsigned);
extern unsigned review_ble_call_count(unsigned);
extern void review_ble_hold_fixture(unsigned);
extern bool review_ble_hold_arrived(void);
extern void review_hid_snapshot_fixture(const struct glass_ble_hid_state *);

static unsigned object_count(lv_obj_t *root)
{unsigned count=1;for(unsigned i=0;i<lv_obj_get_child_count(root);i++)count+=object_count(lv_obj_get_child(root,i));return count;}
static bool job_busy(void)
{pthread_mutex_lock(&g_ble_list_lock);bool busy=g_ble_list_jobs.busy;pthread_mutex_unlock(&g_ble_list_lock);return busy;}
static void wait_job(void)
{
  for(unsigned i=0;i<5000&&job_busy();i++)usleep(1000);
  assert(!job_busy());glass_ble_list_refresh(NULL);
}
static void wait_held(void)
{for(unsigned i=0;i<5000&&!review_ble_hold_arrived();i++)usleep(1000);assert(review_ble_hold_arrived());glass_ble_list_refresh(NULL);}
static void fixture(struct glass_ble_state *state)
{review_ble_snapshot_fixture(state);glass_ble_list_refresh(NULL);}
static void click(lv_obj_t *button)
{lv_obj_send_event(button,LV_EVENT_CLICKED,NULL);}
static struct ble_list_row *row_for(unsigned index)
{for(unsigned i=0;i<GLASS_BLE_LIMIT;i++)if(!lv_obj_has_flag(g_ble_list->rows[i].root,LV_OBJ_FLAG_HIDDEN)&&
  g_ble_list->rows[i].binding.index==index)return &g_ble_list->rows[i];assert(false);return NULL;}
static void fill_devices(struct glass_ble_state *state,unsigned count)
{
  state->count=count;
  for(unsigned i=0;i<count;i++){
    struct glass_ble_device *peer=&state->devices[i];memset(peer,0,sizeof(*peer));
    peer->type=i%2;peer->address[0]=i+1;peer->address[5]=0xc2;peer->rssi=-40-(int)i;
    peer->connectable_known=peer->connectable=true;
    snprintf(peer->name,sizeof(peer->name),"设备 %02u · 蓝牙连接示例",i+1);
  }
}
int main(int argc,char **argv)
{
  assert(argc==3);review_out=argv[2];
  FILE *file=fopen(argv[1],"rb");assert(file);fseek(file,0,SEEK_END);size_t size=ftell(file);rewind(file);
  void *font=malloc(size);assert(font&&fread(font,1,size,file)==size);fclose(file);
  lv_init();lv_display_t *display=lv_display_create(1024,600);
  lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
  lv_display_set_buffers(display,review_fb,NULL,sizeof(review_fb),LV_DISPLAY_RENDER_MODE_FULL);
  lv_display_set_flush_cb(display,review_flush);
  g_desktop.font16=lv_tiny_ttf_create_data(font,size,16);
  g_desktop.font20=lv_tiny_ttf_create_data(font,size,20);
  g_desktop.font28=lv_tiny_ttf_create_data(font,size,28);
  g_desktop.font16->fallback=glass_icon_font(16);g_desktop.font20->fallback=glass_icon_font(20);g_desktop.font28->fallback=glass_icon_font(24);
  g_glass.font48=lv_tiny_ttf_create_data(font,size,48);glass_home_fonts_init(font,size);g_glass.reduced_motion=true;
  g_desktop.screen=glass_box(lv_screen_active(),0,32,1024,568,0,0);
  g_desktop.panel=glass_box(g_desktop.screen,0,0,1024,568,0,0);lv_obj_add_flag(g_desktop.panel,LV_OBJ_FLAG_HIDDEN);
  glass_status_create();
  char path[1024];snprintf(path,sizeof(path),"%s/screens.json",review_out);
  review_manifest=fopen(path,"w");assert(review_manifest);fputs("[\n",review_manifest);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;
    struct glass_ble_state state={.started=true,.ready=true,.scan_generation=7,.generation=10};
    struct glass_ble_hid_state hid={.host_ready=true};review_hid_snapshot_fixture(&hid);
    review_ble_snapshot_fixture(&state);unsigned scans=review_ble_call_count(1);
    glass_bluetooth_page(NULL);assert(g_ble_list);wait_job();
    assert(review_ble_call_count(1)==scans+1);
    unsigned objects=object_count(g_ble_list->root);
    const unsigned char *arrow=(const unsigned char *)LV_SYMBOL_RIGHT;
    uint32_t arrow_code=((arrow[0]&15)<<12)|((arrow[1]&63)<<6)|(arrow[2]&63);
    lv_font_glyph_dsc_t glyph={0};
    assert(lv_font_get_glyph_dsc(lv_obj_get_style_text_font(g_ble_list->rows[0].arrow,0),&glyph,arrow_code,0)&&!glyph.is_placeholder);
    assert(!lv_obj_has_flag(g_ble_list->empty,LV_OBJ_FLAG_HIDDEN));
    review_capture(display,"bluetooth-empty","蓝牙 · 设备连接入口");
    glass_ble_read(&state);
    assert(state.scanning&&state.scan_generation==8&&review_ble_call_count(1)==scans+1);
    review_capture(display,"bluetooth-scanning","蓝牙 · 正在搜索");
    fill_devices(&state,6);
    state.devices[0].name[0]=0;state.devices[0].connectable=false;
    state.devices[1].name[0]=0;state.devices[1].bonded=true;
    state.devices[2].connectable=false;state.devices[2].service_hint=0x181a;
    state.devices[3].appearance=0x03c1;state.devices[4].service128=true;
    state.devices[5].name[0]=0;fixture(&state);
    assert(g_ble_list->rows[0].binding.index==1);
    assert(g_ble_list->rows[1].binding.index==3&&g_ble_list->rows[3].binding.index==2);
    assert(strstr(lv_label_get_text(row_for(3)->address),"键盘"));
    assert(strstr(lv_label_get_text(row_for(2)->address),"环境传感服务"));
    assert(lv_obj_has_state(row_for(2)->root,LV_STATE_DISABLED));
    unsigned blocked=review_ble_call_count(2);click(row_for(2)->root);
    assert(!job_busy()&&review_ble_call_count(2)==blocked);
    assert(lv_obj_has_flag(g_ble_list->rows[4].root,LV_OBJ_FLAG_HIDDEN));
    review_capture(display,"bluetooth-priority-folded","蓝牙 · 已配对优先与未命名广播折叠");
    click(g_ble_list->broadcasts);assert(g_ble_list->expanded);
    assert(!lv_obj_has_flag(row_for(0)->root,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_state(row_for(0)->root,LV_STATE_DISABLED));
    assert(strstr(lv_label_get_text(row_for(0)->address),"类型未知"));
    assert(row_for(5)->binding.index==5&&lv_obj_get_y(row_for(5)->root)<lv_obj_get_y(row_for(0)->root));
    review_capture(display,"bluetooth-broadcasts-expanded","蓝牙 · 展开广播与类型提示");
    click(g_ble_list->broadcasts);assert(!g_ble_list->expanded);
    /* Capacity admission can replace a slot after the initial snapshot but
     * before cancellation finishes. The worker must revalidate its address. */
    review_ble_hold_fixture(3);click(row_for(3)->root);wait_held();
    glass_ble_read(&state);state.devices[3].address[0]=97;fixture(&state);
    review_ble_hold_fixture(0);wait_job();
    assert(g_ble_list_jobs.result==-ESTALE&&review_ble_call_count(2)==blocked);
    state.scanning=true;fixture(&state);
    fill_devices(&state,4);state.devices[0].name[0]=0;state.devices[0].rssi=-91;
    strcpy(state.devices[1].name,"无线键盘");strcpy(state.devices[2].name,"无线键盘");
    snprintf(state.devices[3].name,sizeof(state.devices[3].name),"Studio device with a deliberately long advertised device name");
    state.devices[3].rssi=127;fixture(&state);
    assert(g_ble_list->rows[0].binding.index==1);
    assert(lv_obj_has_flag(g_ble_list->rows[3].root,LV_OBJ_FLAG_HIDDEN));
    click(g_ble_list->broadcasts);
    assert(!strcmp(lv_label_get_text(row_for(0)->name),"未命名设备"));
    assert(strcmp(lv_label_get_text(row_for(1)->address),lv_label_get_text(row_for(2)->address)));
    assert(!strcmp(lv_label_get_text(row_for(3)->signal),"信号未知"));
    review_capture(display,"bluetooth-discovered","蓝牙 · 名称、地址与信号");
    unsigned cancelled=review_ble_call_count(3),connected=review_ble_call_count(2);
    click(row_for(1)->root);wait_job();glass_ble_read(&state);
    assert(!state.scanning&&state.connecting&&state.peer_valid&&state.peer.address[0]==2);
    assert(review_ble_call_count(3)==cancelled+1&&review_ble_call_count(2)==connected+1);
    assert(g_ble_list->rows[0].binding.index==1);
    review_capture(display,"bluetooth-connecting","蓝牙 · 正在连接选中设备");
    state.connecting=false;state.connected=true;state.encrypted=state.bonded=false;state.generation++;
    fixture(&state);assert(strstr(lv_label_get_text(g_ble_list->status),"连接成功"));
    assert(!lv_obj_has_state(g_ble_list->scan,LV_STATE_DISABLED));
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(g_ble_list->scan,0)),"配对设备"));
    review_capture(display,"bluetooth-linked","蓝牙 · 连接成功，等待配对");
    /* An accepted pair request without encryption must never look successful. */
    review_ble_pair_fixture(1);click(g_ble_list->scan);wait_job();
    assert(g_ble_list_jobs.result==-EACCES&&strstr(lv_label_get_text(g_ble_list->status),"未完成"));
    review_ble_pair_fixture(0);review_ble_error_fixture(-EACCES);
    click(g_ble_list->scan);wait_job();
    assert(strstr(lv_label_get_text(g_ble_list->detail),"允许配对"));
    review_capture(display,"bluetooth-pair-failed","蓝牙 · 配对失败可重试");
    review_ble_error_fixture(-ETIMEDOUT);click(g_ble_list->scan);wait_job();
    assert(strstr(lv_label_get_text(g_ble_list->detail),"超时"));
    review_ble_error_fixture(0);review_ble_hold_fixture(5);
    click(g_ble_list->scan);wait_held();
    assert(strstr(lv_label_get_text(g_ble_list->status),"正在配对"));
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(g_ble_list->control,0)),"取消配对"));
    click(g_ble_list->control);review_ble_hold_fixture(0);wait_job();
    assert(g_ble_list_jobs.result==-ECANCELED&&strstr(lv_label_get_text(g_ble_list->status),"取消"));
    /* Encryption without bonding is an encrypted session, not saved pairing. */
    review_ble_pair_fixture(2);click(g_ble_list->scan);wait_job();glass_ble_read(&state);
    assert(state.encrypted&&!state.bonded&&strstr(lv_label_get_text(g_ble_list->status),"安全连接成功"));
    assert(strstr(lv_label_get_text(g_ble_list->detail),"未保存配对"));
    review_ble_pair_fixture(0);state.encrypted=false;fixture(&state);
    review_ble_hold_fixture(5);click(g_ble_list->scan);wait_held();
    review_capture(display,"bluetooth-pairing-device","蓝牙 · 正在配对");
    panel_hide(NULL);assert(!g_ble_list);
    glass_bluetooth_page(NULL);assert(g_ble_list&&job_busy());
    review_ble_hold_fixture(0);wait_job();glass_ble_read(&state);
    assert(state.encrypted&&state.bonded&&strstr(lv_label_get_text(g_ble_list->status),"配对成功"));
    assert(strstr(lv_label_get_text(g_ble_list->status),"无线键盘"));
    glass_status_tick();assert(!strcmp(lv_label_get_text(g_status_ui.ble_count),"1"));
    hid.connected=true;review_hid_snapshot_fixture(&hid);
    glass_status_tick();assert(!strcmp(lv_label_get_text(g_status_ui.ble_count),"2"));
    hid.connected=false;review_hid_snapshot_fixture(&hid);
    assert(strstr(lv_label_get_text(g_ble_list->detail),"已配对"));
    state.store_error=-ENOSPC;state.bonded=false;fixture(&state);
    assert(strstr(lv_label_get_text(g_ble_list->detail),"保存失败"));
    assert(strstr(lv_label_get_text(g_ble_list->detail),"重启"));
    review_capture(display,"bluetooth-store-failed","蓝牙 · 配对保存失败");
    state.store_error=0;state.bonded=true;fixture(&state);
    assert(!lv_obj_has_state(g_ble_list->scan,LV_STATE_DISABLED));
    review_capture(display,"bluetooth-connected","蓝牙 · 当前连接与配对状态");
    struct glass_ble_state before_scan=state;
    click(g_ble_list->scan);wait_job();glass_ble_read(&state);
    assert(state.connected&&state.scanning&&state.generation==before_scan.generation);
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(g_ble_list->scan,0)),"停止搜索"));
    click(g_ble_list->scan);wait_job();glass_ble_read(&state);
    assert(state.connected&&!state.scanning&&state.generation==before_scan.generation&&state.peer_valid);
    before_scan.scan_generation=state.scan_generation;state=before_scan;fixture(&state);

    struct glass_ble_state single=state;
    state.central_capacity=2;state.session_count=state.connection_count=2;
    state.sessions[0]=(struct glass_ble_session){.generation=state.generation,.connected=true,.encrypted=true,.bonded=true,.peer=state.peer};
    state.sessions[1]=(struct glass_ble_session){.generation=state.generation+20,.connected=true,.encrypted=true,.bonded=true,.peer=state.devices[2]};
    fixture(&state);glass_status_tick();assert(!strcmp(lv_label_get_text(g_status_ui.ble_count),"2"));
    assert(!lv_obj_has_state(row_for(2)->root,LV_STATE_DISABLED));
    click(row_for(2)->root);wait_job();glass_ble_read(&state);
    assert(state.generation==single.generation+20&&state.connection_count==2&&state.sessions[0].connected);
    review_capture(display,"bluetooth-multiple","蓝牙 · 两个普通连接独立管理");
    click(g_ble_list->control);wait_job();glass_ble_read(&state);
    assert(state.sessions[1].disconnecting&&!state.sessions[0].disconnecting&&state.sessions[0].connected);
#if GLASS_BLE_CENTRAL_LIMIT == 8
    state=single;fill_devices(&state,8);state.central_capacity=8;
    state.session_count=state.connection_count=8;state.connected=state.peer_valid=true;
    for(unsigned i=0;i<8;i++)state.sessions[i]=(struct glass_ble_session){
      .generation=100+i,.connected=true,.encrypted=true,.bonded=true,.peer=state.devices[i]};
    state.generation=100;state.peer=state.devices[0];fixture(&state);glass_status_tick();
    assert(!strcmp(lv_label_get_text(g_status_ui.ble_count),"8"));
    for(unsigned i=0;i<8;i++)assert(!lv_obj_has_state(row_for(i)->root,LV_STATE_DISABLED));
    click(row_for(7)->root);wait_job();glass_ble_read(&state);
    assert(state.generation==107&&state.connection_count==8);
    review_capture(display,"bluetooth-eight","蓝牙 · 八个普通设备独立选择");
    state.connected=false;state.connection_count=0;state.mouse_paused=true;
    for(unsigned i=0;i<8;i++){state.sessions[i].connected=false;state.sessions[i].mouse_paused=true;}
    fixture(&state);glass_status_tick();
    assert(!strcmp(lv_label_get_text(g_status_ui.ble_count),"0"));
    assert(lv_obj_has_state(g_ble_list->scan,LV_STATE_DISABLED));
    assert(!strcmp(lv_label_get_text(lv_obj_get_child(g_ble_list->control,0)),"取消恢复"));
    assert(!strcmp(lv_label_get_text(row_for(7)->action),"等待恢复"));
    lv_obj_invalidate(lv_screen_active());
    review_capture(display,"bluetooth-eight-paused","蓝牙 · 八个设备暂停，保留恢复身份");
#endif
    state=single;fixture(&state);
    assert(!review_find(g_desktop.panel,"无线触摸板"));glass_mouse_open(NULL);
    assert(!g_ble_list&&g_mouse_status&&!lv_obj_has_state(g_mouse_start,LV_STATE_DISABLED));
    assert(!g_mouse_input_ready);
    glass_bluetooth_page(NULL);assert(g_ble_list&&!g_mouse_status);
    review_ble_error_fixture(-EBUSY);click(g_ble_list->control);wait_job();glass_ble_read(&state);
    assert(state.connected&&!state.disconnecting&&!lv_obj_has_state(g_ble_list->control,LV_STATE_DISABLED));
    review_ble_error_fixture(0);
    click(g_ble_list->control);wait_job();glass_ble_read(&state);
    assert(state.connected&&state.disconnecting&&strstr(lv_label_get_text(g_ble_list->status),"正在断开"));
    state.connected=state.disconnecting=state.peer_valid=state.encrypted=state.bonded=false;state.generation++;
    fixture(&state);assert(!strcmp(lv_label_get_text(g_ble_list->status),"蓝牙已断开"));
    if(!g_ble_list->expanded)click(g_ble_list->broadcasts);
    assert(lv_obj_get_style_opa(g_ble_list->rows[1].root,0)==LV_OPA_COVER);
    click(g_ble_list->rows[0].root);wait_job();glass_ble_read(&state);
    state.connecting=state.peer_valid=false;state.error=-ETIMEDOUT;fixture(&state);
    assert(strstr(lv_label_get_text(g_ble_list->detail),"超时"));
    review_capture(display,"bluetooth-failed","蓝牙 · 连接失败反馈");
    /* The display still references the previous scan. A worker must refuse
     * the changed generation rather than connecting the replacement peer. */
    connected=review_ble_call_count(2);state.error=0;state.scan_generation++;state.devices[0].address[0]=99;
    review_ble_snapshot_fixture(&state);click(g_ble_list->rows[0].root);wait_job();
    assert(review_ble_call_count(2)==connected&&strstr(lv_label_get_text(g_ble_list->detail),"列表已更新"));
    lv_obj_send_event(g_ble_list->rows[0].root,LV_EVENT_PRESSED,NULL);
    state.scan_generation++;state.devices[0].address[0]=98;fixture(&state);
    click(g_ble_list->rows[0].root);assert(!job_busy()&&review_ble_call_count(2)==connected);
    fill_devices(&state,GLASS_BLE_LIMIT);state.scan_generation++;fixture(&state);
    lv_obj_update_layout(g_ble_list->root);
    for(unsigned i=0;i<GLASS_BLE_LIMIT;i++)assert(!lv_obj_has_flag(g_ble_list->rows[i].root,LV_OBJ_FLAG_HIDDEN));
    lv_obj_scroll_to_y(g_ble_list->list,25*80,LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(g_ble_list->list)>0&&object_count(g_ble_list->root)==objects);
    review_capture(display,"bluetooth-full-list","蓝牙 · 32 台设备滚动列表");
    const char *name=lv_label_get_text(g_ble_list->rows[0].name);
    for(unsigned i=0;i<30;i++)glass_ble_list_refresh(NULL);
    assert(name==lv_label_get_text(g_ble_list->rows[0].name)&&object_count(g_ble_list->root)==objects);
    state.scan_generation++;state.count=0;state.scanning=true;fixture(&state);
    assert(lv_obj_get_scroll_y(g_ble_list->list)==0);
    state.scanning=false;fixture(&state);
    hid.enabled=hid.connected=true;hid.advertising=false;state.connected=false;
    review_hid_snapshot_fixture(&hid);fixture(&state);
    assert(g_ble_list->action==0&&lv_obj_has_state(g_ble_list->control,LV_STATE_DISABLED));
    assert(lv_obj_has_state(g_ble_list->scan,LV_STATE_DISABLED));
    scans=review_ble_call_count(1);
    click(g_ble_list->scan);wait_job();
    assert(review_ble_call_count(1)==scans);
    assert(!g_ble_list->state.connected&&g_ble_list->hid.connected);
    review_ble_error_fixture(0);
    review_capture(display,"bluetooth-hid-busy","蓝牙 · 独立业务状态与后端忙反馈");
    glass_mouse_open(NULL);assert(!g_ble_list&&g_mouse_status);
    review_capture(display,"bluetooth-touchpad","蓝牙 · 原有触摸板入口");
    hid.store_error=-ENOSPC;review_hid_snapshot_fixture(&hid);glass_mouse_refresh(NULL);
    assert(strstr(lv_label_get_text(g_mouse_detail),"保存失败"));
    review_capture(display,"bluetooth-mouse-store-failed","蓝牙 · 鼠标配对保存失败");
    hid.store_error=0;review_hid_snapshot_fixture(&hid);glass_mouse_refresh(NULL);
    glass_bluetooth_page(NULL);assert(g_ble_list&&!g_mouse_timer&&!g_mouse_status);
    wait_job();glass_ble_read(&state);
    assert(!state.scanning&&g_ble_list->hid.connected);
    assert(lv_obj_has_state(g_ble_list->scan,LV_STATE_DISABLED));
    scans=review_ble_call_count(1);
    hid.enabled=hid.advertising=hid.connected=false;state.connected=false;review_hid_snapshot_fixture(&hid);
    /* Initialization can be cancelled while the controller is not ready;
     * it never blocks the LVGL caller or launches a late scan after cancel. */
    state.ready=false;fixture(&state);click(g_ble_list->scan);assert(job_busy());
    click(g_ble_list->control);wait_job();assert(review_ble_call_count(1)==scans);
    assert(strstr(lv_label_get_text(g_ble_list->detail),"取消"));
    state.ready=true;fixture(&state);review_ble_hold_fixture(1);
    click(g_ble_list->scan);wait_held();click(g_ble_list->control);
    review_ble_hold_fixture(0);wait_job();glass_ble_read(&state);
    assert(!state.scanning&&g_ble_list_jobs.result==-ECANCELED);
    /* A new connection owner published while the old connect completes must
     * survive cancellation of the older UI request. */
    fill_devices(&state,2);state.scan_generation++;fixture(&state);
    unsigned disconnects=review_ble_call_count(4);review_ble_hold_fixture(2);
    click(g_ble_list->rows[0].root);wait_held();click(g_ble_list->control);
    glass_ble_read(&state);state.connecting=false;state.connected=state.peer_valid=true;
    state.peer=state.devices[1];state.generation+=2;fixture(&state);
    review_ble_hold_fixture(0);wait_job();glass_ble_read(&state);
    assert(state.connected&&state.peer.address[0]==2&&review_ble_call_count(4)==disconnects);
    state.encrypted=state.bonded=false;fixture(&state);review_ble_hold_fixture(5);
    click(g_ble_list->scan);wait_held();
    state.peer=state.devices[0];state.generation++;fixture(&state);
    review_ble_hold_fixture(0);wait_job();glass_ble_read(&state);
    assert(g_ble_list_jobs.result==-ESTALE&&!state.encrypted&&state.peer.address[0]==1);
    assert(review_ble_call_count(4)==disconnects);
    state.connected=state.peer_valid=false;state.generation++;fixture(&state);
    state.ready=true;fixture(&state);review_ble_error_fixture(-EIO);
    click(g_ble_list->scan);wait_job();assert(strstr(lv_label_get_text(g_ble_list->detail),"失败"));
    review_ble_error_fixture(0);
    /* Accepted work owns no GUI pointers: finish after deleting the page. */
    state.ready=false;fixture(&state);click(g_ble_list->scan);panel_hide(NULL);assert(!g_ble_list);
    state.ready=true;review_ble_snapshot_fixture(&state);wait_job();
    glass_bluetooth_page(NULL);glass_ble_read(&state);assert(state.scanning);
    click(g_ble_list->control);wait_job();panel_hide(NULL);assert(!g_ble_list);
    for(unsigned i=0;i<30;i++){
      glass_bluetooth_page(NULL);assert(g_ble_list);panel_hide(NULL);assert(!g_ble_list);
    }
    wait_job();glass_bluetooth_page(NULL);click(g_ble_list->control);wait_job();panel_hide(NULL);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("BLUETOOTH_LIST_OK: both themes; automatic scan; stop-scan/connect; observed connection success; explicit pairing, accepted-only rejection, refusal/timeout/retry/cancel; encrypted session versus saved bond; pairing across page deletion; stale pairing preserves newer peer; confirmed disconnect and row opacity reset; addresses/RSSI; stale rows; 32 retained rows; independent HID/device state and controls; per-business peer count; backend busy feedback; 60 reopen cycles; all BLE commands off UI thread");
  return 0;
}
