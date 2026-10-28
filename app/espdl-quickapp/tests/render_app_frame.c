#define FILE_GESTURES_NO_MAIN
#include "render_file_gestures.c"
static lv_obj_t *launch_frame(const char *source)
{
  lv_obj_t *content=qapp_page_prepare("应用框架");assert(content);
  assert(!qpk_runtime_launch(content,"Frame","frame.fixture","1","builtin:/frame.js",source,strlen(source),zh_font,show_toast,qpk_show_dialog,qpk_show_unresponsive));
  qapp_page_commit(content);settle();return content;
}
int main(int argc,char **argv)
{
  lv_display_t *display=review_pointer_init(argc,argv);
  const char *navigation="globalThis.hits=0;let output;const entries=ui.sidebar([{label:'概览',onClick:()=>{hits++;ui.setText(output,'概览内容');}},{label:'历史',onClick:()=>{hits++;ui.setText(output,'历史内容');}}]);const size=ui.getSize();if(size.width!==860||size.height!==568)throw Error('navigation safe area');output=ui.text('分类和辅助功能自动放进侧栏',24,36,24,ui.primary);ui.button('主要操作',24,100,300,44,()=>{},ui.accent);";
  const char *empty="globalThis.hits=0;ui.sidebar([]);const size=ui.getSize();if(size.width!==1024||size.height!==512)throw Error('empty safe area');ui.text('没有辅助入口，自动使用完整宽度',24,36,24,ui.primary);ui.button('主要操作',24,100,300,44,()=>{hits++;},ui.accent);";
  for(unsigned theme=0;theme<2;theme++){
    g_desktop.light_theme=theme!=0;glass_rebuild();
    lv_obj_t *content=launch_frame(empty),*rail=lv_obj_get_user_data(content);
    assert(lv_obj_get_x(content)==0&&lv_obj_get_width(content)==1024&&lv_obj_get_height(content)==512);
    assert(lv_obj_get_height(rail)==56&&lv_obj_has_flag(lv_obj_get_child(rail,0),LV_OBJ_FLAG_HIDDEN));
    review_capture(display,"frame-auto-hidden","无辅助功能时自动隐藏侧栏");panel_hide(NULL);
    content=launch_frame(navigation);rail=lv_obj_get_user_data(content);
    assert(lv_obj_get_x(content)==164&&lv_obj_get_width(rail)==164&&lv_obj_get_child_count(rail)==4);
    stroke_at(82,32+88+22,24,0,4);assert(!qpk_review_eval("if(hits)throw Error('sidebar drag activated')"));
    stroke_at(82,32+140+22,0,0,1);assert(!qpk_review_eval("if(hits!==1)throw Error('sidebar tap lost')"));
    assert(review_label_find(content,"历史内容"));
    lv_obj_t *back=review_callback_find(rail,qapp_back);assert(back);lv_area_t area;lv_obj_get_coords(back,&area);
    assert(area.x1==12&&area.y1==544&&lv_obj_get_width(back)==140);
    review_capture(display,"frame-auto-navigation","额外功能自动整合到侧栏");panel_hide(NULL);
    launch_frame("let rejected=0;for(const value of [null,[{label:'坏入口'}],Array(9).fill({label:'超额',onClick:()=>{}})]){try{ui.sidebar(value);}catch(e){rejected++;}}if(rejected!==3||ui.getSize().width!==1024)throw Error('invalid navigation');ui.text('invalid menus rejected',24,40,20);try{ui.sidebar([{label:'过晚',onClick:()=>{}}]);throw 1;}catch(e){if(e===1)throw Error('late menu accepted');}");
    panel_hide(NULL);
    for(unsigned i=0;i<20;i++){launch_frame(i%2?empty:navigation);panel_hide(NULL);}
  }
  fputs("\n]\n",review_manifest);fclose(review_manifest);
  puts("APP_FRAME_OK: both themes; empty sidebar hidden; declarative auxiliary actions integrated; full-width/sidebar safe areas; drag suppression and callbacks; common footer; invalid/late menus rejected; 40 runtime cleanup cycles");
}
