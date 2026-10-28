#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
extern unsigned review_brightness_value(void);
extern void review_brightness_failure(int);
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();g_screen_brightness=100;
    review_brightness_failure(0);glass_screen_brightness_set(100);glass_speaker_volume_set(100);
    glass_settings(NULL);settle();
    lv_obj_t *brightness=g_glass.brightness_slider;
    lv_obj_t *volume=review_callback_find(g_desktop.current_card,glass_speaker_changed);assert(volume);
    assert(!lv_obj_has_state(brightness,LV_STATE_DISABLED));
    stroke_at(540,230,-100,0,10);assert(review_brightness_value()==(unsigned)g_screen_brightness);
    assert(g_screen_brightness<70&&g_screen_brightness>=10);
    stroke_at(800,230,100,0,10);assert(glass_speaker_volume()>50&&glass_speaker_volume()<100);
    lv_slider_set_value(volume,0,LV_ANIM_OFF);lv_obj_send_event(volume,LV_EVENT_VALUE_CHANGED,NULL);assert(!glass_speaker_volume());
    lv_slider_set_value(volume,75,LV_ANIM_OFF);lv_obj_send_event(volume,LV_EVENT_VALUE_CHANGED,NULL);assert(glass_speaker_volume()==75);
    glass_music_volume(35);glass_speaker_control_refresh();
    assert(lv_slider_get_value(volume)==35&&review_label_find(g_desktop.current_card,"35%"));
    glass_music_volume(75);glass_speaker_control_refresh();
    lv_slider_set_value(brightness,80,LV_ANIM_OFF);lv_obj_send_event(brightness,LV_EVENT_VALUE_CHANGED,NULL);assert(g_screen_brightness==80&&review_brightness_value()==80);
    review_capture(display,"device-controls","设置 · 屏幕亮度与扬声器音量");
    review_brightness_failure(-EIO);lv_slider_set_value(brightness,60,LV_ANIM_OFF);lv_obj_send_event(brightness,LV_EVENT_VALUE_CHANGED,NULL);
    assert(g_screen_brightness==80&&lv_slider_get_value(brightness)==80&&review_label_find(g_desktop.current_card,"80%"));
    review_brightness_failure(0);review_click("Wi-Fi 网络");settle();g_desktop.page_back(NULL);settle();
    assert(lv_slider_get_value(g_glass.brightness_slider)==80);
    volume=review_callback_find(g_desktop.current_card,glass_speaker_changed);assert(volume&&lv_slider_get_value(volume)==75);
    panel_hide(NULL);
    assert(!g_glass.speaker_slider&&!g_glass.speaker_value);
    review_music_fixture(1);glass_music(NULL);settle();music_ui_refresh(NULL);
    assert(lv_slider_get_value(g_music_ui.volume)==75);
    unsigned revision=g_music_ui.snapshot->revision;
    glass_speaker_volume_set(25);music_ui_refresh(NULL);
    assert(g_music_ui.snapshot->revision==revision&&lv_slider_get_value(g_music_ui.volume)==25);
    lv_slider_set_value(g_music_ui.volume,55,LV_ANIM_OFF);
    lv_obj_send_event(g_music_ui.volume,LV_EVENT_VALUE_CHANGED,NULL);assert(glass_speaker_volume()==55);
    music_full_open(NULL);assert(g_music_ui.full&&lv_slider_get_value(g_music_ui.full->volume)==55);
    lv_slider_set_value(g_music_ui.full->volume,0,LV_ANIM_OFF);
    lv_obj_send_event(g_music_ui.full->volume,LV_EVENT_VALUE_CHANGED,NULL);assert(!glass_speaker_volume());
    panel_hide(NULL);glass_settings(NULL);settle();
    volume=review_callback_find(g_desktop.current_card,glass_speaker_changed);
    assert(volume&&lv_slider_get_value(volume)==0&&review_label_find(g_desktop.current_card,"0%"));
    panel_hide(NULL);assert(!g_glass.speaker_slider&&!g_glass.speaker_value);
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("DEVICE_CONTROLS_OK: both themes; real pointer sliders; settings/music/fullscreen shared speaker volume; external update without music revision; mute/range; deleted-control cleanup; brightness rollback");
}
