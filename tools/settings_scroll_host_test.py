"""设置页实际滚动反馈及字号选择回归。/ Actual settings scrolling and size-picker regression.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
s=(root/'main/apps/app_device_settings.c').read_text()
def block(start):
    at=s.index('{',start)+1
    depth,quote,escape=1,None,False
    while depth:
        c=s[at]
        if quote:
            if escape:escape=False
            elif c=='\\':escape=True
            elif c==quote:quote=None
        elif c in "\"'":quote=c
        elif c=='{':depth+=1
        elif c=='}':depth-=1
        at+=1
    return s[start:at]
def function(name):
    m=re.search(r'^static [^\n]+\b'+name+r'\([^;{}]*\)\s*\{',s,re.M)
    assert m,name
    return block(m.start())
render_start=s.index('static void render(')
render_mode=block(s.index("if (s_page == SETTINGS_MAIN_REFRESH)",render_start))
render_size=block(s.index('if (s_page == SETTINGS_SYSTEM_SIZE)',render_start))
gesture_start=s.index('static app_redraw_t on_gesture(')
select_mode=block(s.index("if (s_page == SETTINGS_MAIN_REFRESH)",gesture_start))
select_size=block(s.index('if (s_page == SETTINGS_SYSTEM_SIZE)',gesture_start))
# 资料卡必须在蓝牙和扫描页 return 之后绘制。/ The profile must draw only after both Bluetooth early returns.
profile=s.index('const int profile_y',render_start)
for page in ('SETTINGS_BLUETOOTH','SETTINGS_BLE_SCAN'):
    pos=s.index('if (s_page == '+page+')',render_start)
    assert pos<profile and 'return;' in block(pos) and 'profile_y' not in block(pos)
unit=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "epdiy.h"
#include "app.h"
#include "ui_follow_scroll.h"
#include "ui_follow_cache.h"
typedef enum {APP_REDRAW_NONE,APP_REDRAW_AREA,APP_REDRAW_PAGE,APP_REDRAW_FULL} app_redraw_t;
#include "ui_gesture.h"
enum {SETTINGS_MAIN,SETTINGS_BLUETOOTH,SETTINGS_BLE_SCAN,SETTINGS_UPGRADE,SETTINGS_SYSTEM_SIZE,SETTINGS_MAIN_REFRESH,
      SETTINGS_SHELF_STYLE,SETTINGS_SYSTEM_FONT,SETTINGS_WALLPAPER,SETTINGS_AVATAR,SETTINGS_TEXT_EDIT,SETTINGS_PIN_ENTRY};
enum EpdDrawMode {MODE_GC16,MODE_GL16,MODE_DU};
enum EpdDrawError {EPD_DRAW_SUCCESS};
#define UI_NAV_TOP 1096
#define UI_NAV_REFRESH_END (UI_NAV_TOP & ~31)
#define UI_LOCK_WIDTH 684
#define UI_GRAY_WHITE 255
#define UI_GRAY_BLACK 0
static const int E0470_WAVEFORM=0;
static const int E0470_FOLLOW_WAVEFORM=1;
static int s_page;
static bool s_scroll_present_pending,s_scroll_follow_present,s_scroll_frame_changed,s_scroll_settle_present;
static uint8_t *s_scroll_cache;
static unsigned s_scroll_cache_step;
static void settings_scroll_cache_release(void){free(s_scroll_cache);s_scroll_cache=NULL;s_scroll_cache_step=0;}
static bool app_settings_main_fast_refresh(void);
static ui_follow_scroll_t s_follow_scroll;
static int s_main_scroll,s_ble_scroll;
#define SETTINGS_SCROLL_MAX 334
typedef struct {int content_bottom;} test_layout_t;
static test_layout_t ble_layout(void){return (test_layout_t){2100};}
static test_layout_t ble_scan_layout(void){return (test_layout_t){2100};}
static bool s_input_layout,s_input_settle;
static EpdRect s_input_area;
static int paints,pushes,phases;
static EpdRect pushed;
static enum EpdDrawMode push_mode;
static void render(app_ctx_t *ctx,uint8_t *fb){(void)ctx;(void)fb;++paints;}
static int update_display_area_with(void *hl,const int *wave,enum EpdDrawMode mode,EpdRect area){(void)hl;(void)wave;pushed=area;push_mode=mode;++pushes;return 0;}
static int update_display_area_diff_with(void *hl,const int *wave,enum EpdDrawMode mode,EpdRect area){return update_display_area_with(hl,wave,mode,area);}
static int update_display_area_full_with(void *hl,const int *wave,enum EpdDrawMode mode,EpdRect area){return update_display_area_with(hl,wave,mode,area);}
static bool display_follow_busy(void){return phases>0;}
static int display_follow_area(void *hl,EpdRect area,bool changed,bool continuous){if(continuous&&changed)phases=6;if(continuous&&phases)--phases;return update_display_area_with(hl,&E0470_FOLLOW_WAVEFORM,MODE_DU,area);}
static int settles;
static int display_follow_settle(void *hl,EpdRect area,bool restore){assert(!phases);++settles;return restore?update_display_area_with(hl,&E0470_WAVEFORM,MODE_GL16,area):0;}
static EpdRect ui_pinpad_full(void){return (EpdRect){0,0,684,1216};}
static EpdRect upgrade_progress_area(void){return (EpdRect){52,468,580,64};}
static void display_main_transition_cancel(void){}
static void guard_draw_result(void *hl,int status){(void)hl;assert(!status);}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.width&&y<r.y+r.height;}
typedef enum {APP_MAIN_REFRESH_NORMAL,APP_MAIN_REFRESH_FAST,APP_MAIN_REFRESH_WATER} app_main_refresh_mode_t;
static app_main_refresh_mode_t main_choice;
static app_main_refresh_mode_t app_settings_main_refresh_mode(void){return main_choice;}
static bool app_settings_main_fast_refresh(void){return main_choice==APP_MAIN_REFRESH_FAST;}
static void app_settings_set_main_refresh_mode(app_main_refresh_mode_t value){main_choice=value;}
static void fit_value(char *text,int width){(void)text;(void)width;}
static uint8_t chosen=120;
static uint8_t app_settings_system_font_size(void){return chosen;}
static void app_settings_set_system_font_size(uint8_t percent){chosen=percent;}
static void ui_text_set_system_scale(bool enabled){assert(enabled);}
static void back_header(uint8_t *fb,const char *s){(void)fb;(void)s;}
static void section(uint8_t *fb,int y,const char *s){(void)fb;(void)y;(void)s;}
static unsigned cards;
static EpdRect boxes[11];
static void settings_card(uint8_t *fb,EpdRect r,int radius,int fill,int border){(void)fb;(void)radius;(void)fill;(void)border;assert(cards<11);boxes[cards++]=r;assert(r.y+r.height<UI_NAV_TOP);}
static void ui_text_vc(uint8_t *fb,...){(void)fb;}
void epd_fill_circle(int x,int y,int r,uint8_t gray,uint8_t *fb){(void)x;(void)y;(void)r;(void)gray;(void)fb;}
static void ui_nav_draw(uint8_t *fb,int tab){(void)fb;(void)tab;}
"""+function('settings_scroll_page')+'\n'+function('settings_scroll_feed')+'\n'+function('settings_present')+'\n'
unit+='static app_redraw_t scroll_tick(app_ctx_t *ctx){'+block(s.index('if (settings_scroll_page())',s.index('static app_redraw_t on_tick')))+'return APP_REDRAW_NONE;}\n'
unit+='static int row_taps;\nstatic app_redraw_t scroll_event(app_ctx_t *ctx,const ui_gesture_event_t *ev){'+block(s.index('if (settings_scroll_page())',gesture_start))+'if(ev->type==UI_GESTURE_TAP)++row_taps;return APP_REDRAW_NONE;}\n'
unit+='static void draw_mode(uint8_t *fb){'+render_mode+'}\n'
unit+='static app_redraw_t choose_mode(int x,int y){ui_gesture_event_t event={.x0=x,.y0=y};const ui_gesture_event_t *ev=&event;'+select_mode+'return APP_REDRAW_NONE;}\n'
unit+='static void draw_size(uint8_t *fb){'+render_size+'}\n'
unit+='static app_redraw_t choose_size(int x,int y){ui_gesture_event_t event={.x0=x,.y0=y};const ui_gesture_event_t *ev=&event;'+select_size+'return APP_REDRAW_NONE;}\n'
unit+=r"""
int main(void){
 app_ctx_t ctx={0};bool handled;cst836u_touch_t touch={.count=1,.touched=true,.x=300,.y=700};
 ctx.touch=&touch;ctx.now_ms=100;ctx.pressed=true;s_page=SETTINGS_MAIN;
 assert(settings_scroll_feed(&ctx,&handled)==APP_REDRAW_NONE&&!handled);
 ctx.pressed=false;ctx.now_ms+=10;touch.y=650;
 assert(settings_scroll_feed(&ctx,&handled)==APP_REDRAW_AREA&&s_main_scroll==50&&handled);
 assert(settings_present(&ctx,APP_REDRAW_AREA)&&paints==1&&pushes==1&&pushed.y==160&&pushed.height==936&&push_mode==MODE_DU);
 touch.y=649;ctx.now_ms+=10;assert(settings_scroll_feed(&ctx,&handled)==APP_REDRAW_AREA&&s_main_scroll==51);assert(settings_present(&ctx,APP_REDRAW_AREA)&&push_mode==MODE_DU);
 touch.y=300;ctx.now_ms+=10;assert(settings_scroll_feed(&ctx,&handled)==APP_REDRAW_AREA&&s_main_scroll==334);assert(settings_present(&ctx,APP_REDRAW_AREA)&&push_mode==MODE_DU);
 touch.touched=false;touch.count=0;ctx.released=true;ctx.now_ms+=10;
 assert(settings_scroll_feed(&ctx,&handled)==APP_REDRAW_NONE&&s_main_scroll==334&&handled);
 assert(!ui_follow_scroll_take_settle(&s_follow_scroll,&ctx));ctx.now_ms+=320;assert(ui_follow_scroll_take_settle(&s_follow_scroll,&ctx));
 s_scroll_present_pending=true;assert(settings_present(&ctx,APP_REDRAW_AREA)&&push_mode==MODE_GL16);assert(!ui_follow_scroll_take_settle(&s_follow_scroll,&ctx));
 s_page=SETTINGS_BLUETOOTH;
 for(int i=0;i<80;++i){s_scroll_present_pending=true;assert(settings_present(&ctx,APP_REDRAW_AREA));assert(pushed.y==190&&pushed.height==906&&push_mode==MODE_GL16);}
 assert(pushes==84&&paints==84);
 const int pages[]={SETTINGS_BLE_SCAN,SETTINGS_SYSTEM_FONT,SETTINGS_WALLPAPER,SETTINGS_AVATAR,SETTINGS_SHELF_STYLE};
 for(unsigned i=0;i<sizeof(pages)/sizeof(pages[0]);++i){
   s_page=pages[i];s_scroll_present_pending=true;assert(settings_present(&ctx,APP_REDRAW_AREA));
   assert(push_mode==MODE_GL16&&pushed.y==(s_page==SETTINGS_SHELF_STYLE?242:190)&&pushed.y+pushed.height==UI_NAV_TOP);
 }
 assert(!settings_present(&ctx,APP_REDRAW_PAGE));
 s_page=SETTINGS_UPGRADE;assert(settings_present(&ctx,APP_REDRAW_AREA)&&pushed.y==468&&pushed.height==64);
 s_page=SETTINGS_SYSTEM_SIZE;draw_size(NULL);assert(cards==11);
 for(unsigned i=0;i<11;++i){EpdRect r=boxes[i];assert(choose_size(r.x+r.width/2,r.y+r.height/2)==APP_REDRAW_PAGE&&chosen==100+i*10);}
 assert(chosen==200&&choose_size(340,310)==APP_REDRAW_NONE&&chosen==200&&choose_size(400,970)==APP_REDRAW_NONE);
 s_page=SETTINGS_MAIN_REFRESH;cards=0;draw_mode(NULL);assert(cards==3);
 for(int i=0;i<3;++i){EpdRect r=boxes[i];assert(choose_mode(r.x+r.width/2,r.y+r.height/2)==APP_REDRAW_PAGE&&main_choice==(app_main_refresh_mode_t)i);}
 assert(choose_mode(5,320)==APP_REDRAW_NONE&&main_choice==APP_MAIN_REFRESH_WATER);

 s_page=SETTINGS_MAIN;s_main_scroll=0;ui_follow_scroll_reset(&s_follow_scroll);ui_gesture_t gesture={0};
 cst836u_touch_t input={.count=1,.touched=true,.x=300,.y=700};ctx=(app_ctx_t){.touch=&input,.pressed=true,.now_ms=10};ui_gesture_event_t event;
 assert(ui_gesture_feed(&gesture,&ctx,&event)&&event.type==UI_GESTURE_PRESS);assert(scroll_event(&ctx,&event)==APP_REDRAW_NONE);assert(scroll_tick(&ctx)==APP_REDRAW_NONE);
 ctx.pressed=false;ctx.now_ms=600;assert(ui_gesture_feed(&gesture,&ctx,&event)&&event.type==UI_GESTURE_LONG_PRESS);assert(scroll_event(&ctx,&event)==APP_REDRAW_NONE);
 ctx.now_ms=610;input.y=640;assert(!ui_gesture_feed(&gesture,&ctx,&event));assert(scroll_tick(&ctx)==APP_REDRAW_AREA&&s_main_scroll==60);assert(settings_present(&ctx,APP_REDRAW_AREA));
 ctx.consumed=true;assert(scroll_tick(&ctx)==APP_REDRAW_NONE&&s_follow_scroll.tracking);ctx.consumed=false;
 ctx.now_ms=620;input.y=639;assert(!ui_gesture_feed(&gesture,&ctx,&event));assert(scroll_tick(&ctx)==APP_REDRAW_AREA&&s_main_scroll==61);assert(settings_present(&ctx,APP_REDRAW_AREA));
 input.touched=false;input.count=0;ctx.released=true;ctx.now_ms=630;assert(ui_gesture_feed(&gesture,&ctx,&event));assert(scroll_event(&ctx,&event)==APP_REDRAW_NONE);assert(scroll_tick(&ctx)==APP_REDRAW_NONE);
 ctx.released=false;ctx.now_ms=950;assert(scroll_tick(&ctx)==APP_REDRAW_AREA);assert(settings_present(&ctx,APP_REDRAW_AREA)&&push_mode==MODE_GL16);assert(scroll_tick(&ctx)==APP_REDRAW_NONE&&!row_taps);
 ui_follow_scroll_reset(&s_follow_scroll);ui_gesture_reset(&gesture);input.touched=true;input.count=1;input.y=700;ctx.pressed=true;ctx.now_ms=1000;
 assert(ui_gesture_feed(&gesture,&ctx,&event));scroll_event(&ctx,&event);ctx.pressed=false;input.y=687;ctx.now_ms=1010;
 assert(!ui_gesture_feed(&gesture,&ctx,&event));assert(scroll_tick(&ctx)==APP_REDRAW_AREA&&s_main_scroll==74);settings_present(&ctx,APP_REDRAW_AREA);
 input.touched=false;input.count=0;ctx.released=true;ctx.now_ms=1020;assert(ui_gesture_feed(&gesture,&ctx,&event)&&event.type==UI_GESTURE_TAP);scroll_event(&ctx,&event);assert(!row_taps);
 ui_follow_scroll_reset(&s_follow_scroll);ui_gesture_reset(&gesture);input.touched=true;input.count=1;input.y=700;ctx.pressed=true;ctx.released=false;ctx.now_ms=1100;
 assert(ui_gesture_feed(&gesture,&ctx,&event));scroll_event(&ctx,&event);input.touched=false;input.count=0;ctx.pressed=false;ctx.released=true;ctx.now_ms=1110;
 assert(ui_gesture_feed(&gesture,&ctx,&event));scroll_event(&ctx,&event);assert(row_taps==1);

 // Continuous path reads a newer position after each phase; unchanged ticks do not repaint.
 ui_follow_scroll_reset(&s_follow_scroll);input=(cst836u_touch_t){.count=1,.touched=true,.x=300,.y=700};
 ctx=(app_ctx_t){.touch=&input,.pressed=true,.now_ms=2000,.continuous_ready=true};
 main_choice=APP_MAIN_REFRESH_NORMAL;settings_scroll_feed(&ctx,&handled);ctx.pressed=false;input.y=650;ctx.now_ms+=10;
 assert(scroll_tick(&ctx)==APP_REDRAW_AREA);int prior_paints=paints,prior_pushes=pushes;
 settings_present(&ctx,APP_REDRAW_AREA);assert(phases==5&&paints==prior_paints+1&&pushes==prior_pushes+1);
 ctx.now_ms+=10;assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);
 assert(phases==4&&paints==prior_paints+1&&pushes==prior_pushes+2);
 input.y=640;ctx.now_ms+=10;assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);
 assert(phases==5&&paints==prior_paints+2);
 input.touched=false;input.count=0;ctx.released=true;ctx.now_ms+=10;scroll_tick(&ctx);
 if(s_scroll_present_pending)settings_present(&ctx,APP_REDRAW_AREA);
 ctx.released=false;ctx.now_ms+=320;
 // Even after the idle timeout, finish every binary phase before any gray restoration.
 while(phases){int before=paints;assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);assert(push_mode==MODE_DU&&paints==before);}
 assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);
 assert(!phases&&push_mode==MODE_GL16&&scroll_tick(&ctx)==APP_REDRAW_NONE);
 // Repeat under both modes: fast finishes with no render/scan; water restores only after draining.
 for(int mode=APP_MAIN_REFRESH_FAST;mode<=APP_MAIN_REFRESH_WATER;++mode){
  main_choice=mode;ui_follow_scroll_reset(&s_follow_scroll);s_main_scroll=0;
  input=(cst836u_touch_t){.count=1,.touched=true,.x=300,.y=700};ctx=(app_ctx_t){.touch=&input,.pressed=true,.continuous_ready=true,.now_ms=3000};
  settings_scroll_feed(&ctx,&handled);ctx.pressed=false;input.y=650;ctx.now_ms+=10;
  assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);
  input.touched=false;input.count=0;ctx.released=true;ctx.now_ms+=10;
  assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);ctx.released=false;
  // 惯性移动仍走黑白相位；停止以后才等待灰阶定稿。
  // Inertia stays on binary phases; wait for gray restoration only after stopping.
  int release_offset=s_main_scroll,coast_frames=0;
  while(s_follow_scroll.coasting){ctx.now_ms+=10;app_redraw_t r=scroll_tick(&ctx);if(r==APP_REDRAW_AREA){settings_present(&ctx,r);assert(push_mode==MODE_DU);}assert(++coast_frames<100);}
  assert(s_main_scroll>release_offset&&s_main_scroll<=334);
  if(mode==APP_MAIN_REFRESH_WATER)ctx.now_ms+=320;
  while(phases){assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);}
  int before_paints=paints,before_pushes=pushes,before_settles=settles;
  assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);
  assert(settles==before_settles+1);
  assert(paints==before_paints+(mode==APP_MAIN_REFRESH_WATER));
  assert(pushes==before_pushes+(mode==APP_MAIN_REFRESH_WATER));
  assert(scroll_tick(&ctx)==APP_REDRAW_NONE);
 }
 // Ready main cache serves the actual presenter with no glyph rendering.
 ui_follow_scroll_reset(&s_follow_scroll);main_choice=APP_MAIN_REFRESH_NORMAL;s_page=SETTINGS_MAIN;s_main_scroll=0;
 uint8_t *cached_frame=calloc(1216*684/2,1);assert(cached_frame);
 s_scroll_cache=calloc(684*((936+334+7)/8),1);assert(s_scroll_cache);s_scroll_cache_step=2;
 input=(cst836u_touch_t){.count=1,.touched=true,.x=300,.y=700};ctx=(app_ctx_t){.fb=cached_frame,.touch=&input,.pressed=true,.continuous_ready=true,.now_ms=4500};
 settings_scroll_feed(&ctx,&handled);ctx.pressed=false;input.y=680;ctx.now_ms+=10;
 int cached_paints=paints;assert(scroll_tick(&ctx)==APP_REDRAW_AREA);settings_present(&ctx,APP_REDRAW_AREA);
 assert(paints==cached_paints&&phases==5);while(phases)settings_present(&ctx,scroll_tick(&ctx));
 settings_scroll_cache_release();free(cached_frame);
 // A brief new tap postpones an earlier scroll's pending restoration rather than losing it.
 ui_follow_scroll_reset(&s_follow_scroll);s_follow_scroll.settle_pending=true;s_follow_scroll.settle_ms=0;
 input=(cst836u_touch_t){.count=1,.touched=true,.x=300,.y=700};ctx=(app_ctx_t){.touch=&input,.pressed=true,.now_ms=5000};
 ui_follow_scroll_feed(&s_follow_scroll,&ctx,160,UI_NAV_TOP,&s_main_scroll,334,&handled);
 assert(s_follow_scroll.settle_pending&&!ui_follow_scroll_take_settle(&s_follow_scroll,&ctx));
 input.touched=false;input.count=0;ctx.pressed=false;ctx.now_ms+=10;
 ui_follow_scroll_feed(&s_follow_scroll,&ctx,160,UI_NAV_TOP,&s_main_scroll,334,&handled);
 ctx.now_ms+=320;assert(ui_follow_scroll_take_settle(&s_follow_scroll,&ctx));
 // Paged font/image selectors keep their normal repaint even in fast main mode.
 main_choice=APP_MAIN_REFRESH_FAST;s_page=SETTINGS_SYSTEM_FONT;s_scroll_present_pending=true;
 int before=pushes;assert(settings_present(&ctx,APP_REDRAW_AREA)&&pushes==before+1&&push_mode==MODE_GL16);
 puts("PASS: one phase per follow tick, target reversal replaces pending phases, unchanged ticks reuse artwork, changed-gray settle in normal/water, no scan in fast");
 puts("PASS: real generic gesture + raw settings ticks: moves after a 500ms hold, one-pixel tracking, consumed-tick suppression, short drag cannot become a row tap, no release jump and one delayed gray settle");
 puts("PASS: pixel-follow bounded scroll, local short DU during contact and changed-gray settle in normal/water, no scan in fast, no periodic GC16 during 80 swipes, header/nav excluded across all settings lists, Bluetooth profile isolation and eleven size options through 200%");
}
"""
with tempfile.TemporaryDirectory() as folder:
    c,binary=Path(folder)/'test.c',Path(folder)/'test'
    c.write_text(unit)
    subprocess.run(['cc','-I'+str(root/'tools/ui_gesture_stubs'),'-I'+str(root/'main/ui'),'-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),str(root/'main/ui/ui_gesture.c'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
