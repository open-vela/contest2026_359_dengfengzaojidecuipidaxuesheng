/* SPDX-License-Identifier: Apache-2.0 */
/* Settings own one parent scene. Children share it; explicit Home clears it.
 * Routes retain identities, never LVGL pointers or a live camera/audio owner. */
static struct {
  lv_event_cb_t open;
  unsigned state;
  bool valid,from_apps,external;
  enum qpk_volume volume;
  char package[48];
} g_settings_origin;
static unsigned g_navigation_resuming_state;
static void glass_navigation_clear(void)
{memset(&g_settings_origin,0,sizeof(g_settings_origin));}
static bool glass_navigation_has_origin(void){return g_settings_origin.valid;}
static unsigned glass_navigation_resume_state(void){return g_navigation_resuming_state;}
static void glass_navigation_capture_settings(void)
{
  if(g_desktop.settings_context)return;
  glass_navigation_clear();
  if(!g_desktop.current_card)return;
  g_settings_origin.open=g_desktop.apps_page?glass_apps:g_desktop.app_open;
  g_settings_origin.from_apps=g_desktop.app_from_apps;
  g_settings_origin.state=g_desktop.app_state;
  if(g_desktop.qapp_page&&g_desktop.app_external>=0&&g_desktop.app_external<g_desktop.nqpk){
    const struct qpk_entry_s *entry=&g_desktop.qpk[g_desktop.app_external];
    strlcpy(g_settings_origin.package,entry->package,sizeof(g_settings_origin.package));
    g_settings_origin.volume=entry->volume;g_settings_origin.external=true;
  }
  g_settings_origin.valid=g_settings_origin.open||g_settings_origin.external;
}
static void glass_navigation_return_settings(void)
{
  typeof(g_settings_origin) scene=g_settings_origin;
  panel_hide(NULL);
  if(!scene.valid)return;
  g_desktop.apps_page=scene.from_apps;
  g_navigation_resuming_state=scene.state;
  if(scene.external){
    qpk_scan();bool found=false;
    for(int i=0;i<g_desktop.nqpk;i++)if(g_desktop.qpk[i].volume==scene.volume&&!strcmp(g_desktop.qpk[i].package,scene.package)){
      launch_external_qapp(i);found=true;break;
    }
    if(!found){panel_hide(NULL);show_toast("原应用暂时不可用，请检查存储设备");}
  }else scene.open(NULL);
  g_navigation_resuming_state=0;
}
