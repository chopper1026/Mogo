#!/usr/bin/env python3
"""中文：实际电源识别器、图形与列表跟随边界回归。/ English: Real power input/raster and follow-scroll boundary regression.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import re, subprocess, sys, tempfile
ROOT=Path(__file__).resolve().parents[1]
power=(ROOT/'main/ui/ui_power_dialog.c').read_text()
if len(sys.argv)>2 and sys.argv[1]=='--input-c':
    Path(sys.argv[2]).write_text(power[:power.index('EpdRect ui_power_dialog_rect(void)')])
    raise SystemExit(0)
def function(path,name):
    text=(ROOT/path).read_text()
    m=re.search(r'^(?:static )?[^\n]+\b'+name+r'\([^;{}]*\)\s*\{',text,re.M)
    assert m,name
    at=m.end();depth=1
    while depth:depth+=(text[at]=='{')-(text[at]=='}');at+=1
    return text[m.start():at]+'\n'
code=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ui_power_dialog.h"
#include "ui_follow_scroll.h"
#include "ui_kit.h"
#define W 684
#define H 1216
static uint8_t fb[W*H/2],before[sizeof(fb)];
static bool text_log;
void epd_draw_pixel(int x,int y,uint8_t v,uint8_t *p){if(x<0||y<0||x>=W||y>=H)return;size_t at=(size_t)y*W/2+x/2;int shift=(x&1)*4;p[at]=(p[at]&~(15<<shift))|((v>>4)<<shift);}
uint8_t epd_get_pixel(int x,int y,int w,int h,const uint8_t *p){assert(w==W&&h==H&&x>=0&&y>=0&&x<W&&y<H);return ((p[(size_t)y*W/2+x/2]>>((x&1)*4))&15)<<4;}
int epd_width(void){return W;}int epd_height(void){return H;}
int epd_rotated_display_width(void){return W;}int epd_rotated_display_height(void){return H;}
enum EpdRotation epd_get_rotation(void){return EPD_ROT_LANDSCAPE;}
void epd_draw_vline(int x,int y,int h,uint8_t v,uint8_t *p){for(int j=0;j<h;++j)epd_draw_pixel(x,y+j,v,p);}
void epd_draw_hline(int x,int y,int w,uint8_t v,uint8_t *p){for(int i=0;i<w;++i)epd_draw_pixel(x+i,y,v,p);}
void epd_fill_rect(EpdRect r,uint8_t v,uint8_t *p){for(int y=r.y;y<r.y+r.height;++y)for(int x=r.x;x<r.x+r.width;++x)epd_draw_pixel(x,y,v,p);}
void epd_draw_line(int x,int y,int xx,int yy,uint8_t v,uint8_t *p){assert(x==xx||y==yy);if(x==xx)epd_draw_vline(x,y,yy-y+1,v,p);else epd_draw_hline(x,y,xx-x+1,v,p);}
uint8_t ui_contrast_gray(uint8_t v){return v&0xf0;}
void ui_clear_page(uint8_t *p){memset(p,255,sizeof(fb));}
void ui_text_box(uint8_t *p,EpdRect box,int px,const char *s,enum EpdFontFlags align,bool inverted){(void)p;(void)align;(void)inverted;assert(box.x>=0&&box.y>=0&&box.x+box.width<=W&&box.y+box.height<=H);if(text_log)printf("TEXT %d %d %d %d %d %s\n",box.x,box.y,box.width,box.height,px,s);}
void ui_text(uint8_t *p,int x,int y,int px,const char *s,enum EpdFontFlags align,bool inverted){(void)p;(void)x;(void)y;(void)px;(void)s;(void)align;(void)inverted;}
'''
for name in ('epd_draw_circle','epd_fill_circle_helper','epd_fill_circle'):
    code+=function('components/epdiy/src/epdiy.c',name)
for name in ('ui_read_pixel','icon_nibble','ui_draw_icon','clamp_radius','draw_arc','ui_draw_round_rect','ui_inset_rect','ui_draw_control_frame','ui_fill_round_rect'):
    code+=function('main/ui/ui_kit.c',name)
code+=re.sub(r'^#include[^\n]*\n','',power,flags=re.M)
code+=r'''
static ui_power_dialog_t dialog;
static ui_follow_scroll_t scroll;
static cst836u_touch_t touch;
static app_ctx_t ctx;
static EpdRect dirty;
static void sample(int x,int y,int count,bool pressed,bool released){touch=(cst836u_touch_t){.x=x,.y=y,.count=count,.touched=count>0};ctx=(app_ctx_t){.touch=&touch,.now_ms=ctx.now_ms+10,.pressed=pressed,.released=released};}
static ui_power_action_t feed(int x,int y,int count,bool pressed,bool released){sample(x,y,count,pressed,released);return ui_power_dialog_feed(&dialog,&ctx,&dirty);}
static void press(void){assert(feed(UI_POWER_THUMB_X,UI_POWER_THUMB_Y,1,true,false)==UI_POWER_ACTION_NONE&&!dirty.height);}
static void raster_check(void){
 memcpy(before,fb,sizeof(fb));ui_power_dialog_draw_controls(fb,&dialog);unsigned changed=0;
 for(int y=0;y<H;++y)for(int x=0;x<W;++x)if(epd_get_pixel(x,y,W,H,fb)!=epd_get_pixel(x,y,W,H,before)){
  ++changed;assert(x>=dirty.x&&x<dirty.x+dirty.width&&y>=dirty.y&&y<dirty.y+dirty.height);
 }
 assert(changed);
}
static bool scroll_feed(int y,int n,bool pressed,bool released,int *offset,int limit,bool *handled){sample(300,y,n,pressed,released);return ui_follow_scroll_feed(&scroll,&ctx,160,1096,offset,limit,handled);}
int main(int argc,char **argv){
 ui_power_dialog_draw(fb,&dialog);press();
 assert(feed(UI_POWER_THUMB_X+1,UI_POWER_THUMB_Y,1,false,false)==UI_POWER_ACTION_NONE&&dialog.position==1);raster_check();
 assert(feed(UI_POWER_THUMB_X+1,UI_POWER_THUMB_Y,1,false,false)==UI_POWER_ACTION_NONE&&!dirty.height);
 for(int pos=2;pos<=UI_POWER_SLIDE_RANGE;++pos){assert(feed(UI_POWER_THUMB_X+pos,UI_POWER_THUMB_Y,1,false,false)==UI_POWER_ACTION_NONE);raster_check();assert(dirty.y>UI_POWER_TRACK_Y&&dirty.y+dirty.height<UI_POWER_TRACK_Y+UI_POWER_TRACK_H);}
 assert(feed(UI_POWER_THUMB_X+UI_POWER_SLIDE_RANGE,UI_POWER_THUMB_Y,0,false,true)==UI_POWER_ACTION_SHUTDOWN);
 assert(feed(UI_POWER_THUMB_X+UI_POWER_SLIDE_RANGE,UI_POWER_THUMB_Y,0,false,true)==UI_POWER_ACTION_NONE);
 ui_power_dialog_reset(&dialog,false);ui_power_dialog_draw(fb,&dialog);
 assert(feed(UI_POWER_THUMB_X+UI_POWER_SLIDE_RANGE,UI_POWER_THUMB_Y,1,true,false)==UI_POWER_ACTION_NONE);
 assert(feed(UI_POWER_THUMB_X+UI_POWER_SLIDE_RANGE,UI_POWER_THUMB_Y,0,false,true)==UI_POWER_ACTION_NONE);
 press();assert(feed(UI_POWER_THUMB_X+UI_POWER_SLIDE_COMMIT-1,UI_POWER_THUMB_Y,1,false,false)==UI_POWER_ACTION_NONE);raster_check();
 assert(feed(500,UI_POWER_THUMB_Y,0,false,true)==UI_POWER_ACTION_NONE&&!dialog.position);raster_check();
 for(int kind=0;kind<5;++kind){
  ui_power_dialog_reset(&dialog,false);press();feed(550,UI_POWER_THUMB_Y,1,false,false);
  if(kind==0)feed(550,UI_POWER_THUMB_Y,2,false,false);
  if(kind==1)feed(550,600,1,false,false);
  if(kind==2){ctx.consumed=true;assert(ui_power_dialog_feed(&dialog,&ctx,&dirty)==UI_POWER_ACTION_NONE);}
  if(kind==3){ctx.touch=NULL;assert(ui_power_dialog_feed(&dialog,&ctx,&dirty)==UI_POWER_ACTION_NONE);}
  if(kind==4){ctx.now_ms=0;assert(ui_power_dialog_feed(&dialog,&ctx,&dirty)==UI_POWER_ACTION_NONE);}
  assert(!dialog.position&&dialog.swallow);assert(feed(555,UI_POWER_THUMB_Y,0,false,true)==UI_POWER_ACTION_NONE);
 }
 ui_power_dialog_reset(&dialog,true);assert(feed(128,374,1,true,false)==UI_POWER_ACTION_NONE);assert(feed(555,374,1,false,false)==UI_POWER_ACTION_NONE);assert(feed(555,374,0,false,true)==UI_POWER_ACTION_NONE);
 for(int i=0;i<2;++i){int x=i?UI_POWER_CANCEL_X:UI_POWER_RESTART_X;
  ui_power_dialog_reset(&dialog,false);ui_power_dialog_draw(fb,&dialog);
  assert(feed(x,UI_POWER_BUTTON_Y,1,true,false)==UI_POWER_ACTION_NONE);raster_check();
  assert(dirty.width==111&&dirty.height==111);
  assert(feed(x,UI_POWER_BUTTON_Y,0,false,true)==(i?UI_POWER_ACTION_CANCEL:UI_POWER_ACTION_RESTART));
  assert(feed(x,UI_POWER_BUTTON_Y,0,false,true)==UI_POWER_ACTION_NONE);
  feed(x-53,UI_POWER_BUTTON_Y-53,1,true,false);assert(feed(x-53,UI_POWER_BUTTON_Y-53,0,false,true)==UI_POWER_ACTION_NONE);
  feed(x,UI_POWER_BUTTON_Y,1,true,false);assert(feed(x+100,UI_POWER_BUTTON_Y,1,false,false)==UI_POWER_ACTION_NONE&&dialog.swallow);assert(feed(x,UI_POWER_BUTTON_Y,0,false,true)==UI_POWER_ACTION_NONE);
 }
 bool handled;int offset=0;
 assert(!scroll_feed(700,1,true,false,&offset,1000,&handled)&&!handled);
 assert(!scroll_feed(695,1,false,false,&offset,1000,&handled)&&!offset);
 assert(scroll_feed(650,1,false,false,&offset,1000,&handled)&&offset==50&&handled);
 assert(scroll_feed(649,1,false,false,&offset,1000,&handled)&&offset==51);
 assert(scroll_feed(400,1,false,false,&offset,1000,&handled)&&offset==300);
 assert(scroll_feed(450,1,false,false,&offset,1000,&handled)&&offset==250);
 assert(!scroll_feed(450,0,false,true,&offset,1000,&handled)&&offset==250&&handled);
 assert(!ui_follow_scroll_take_settle(&scroll,&ctx));
 while(scroll.coasting){sample(300,450,0,false,false);ui_follow_scroll_feed(&scroll,&ctx,160,1096,&offset,1000,&handled);assert(offset>=0&&offset<=250);}
 ctx.now_ms=scroll.settle_ms;assert(ui_follow_scroll_take_settle(&scroll,&ctx));assert(!ui_follow_scroll_take_settle(&scroll,&ctx));
 offset=250;
 assert(!scroll_feed(600,1,true,false,&offset,1000,&handled));assert(scroll_feed(590,1,false,false,&offset,1000,&handled)==false);assert(scroll_feed(100,1,false,false,&offset,1000,&handled)&&offset==750);assert(scroll_feed(0,1,false,false,&offset,1000,&handled)&&offset==850);
 sample(300,0,2,false,false);assert(!ui_follow_scroll_feed(&scroll,&ctx,160,1096,&offset,1000,&handled)&&handled&&offset==850);assert(!scroll_feed(0,0,false,true,&offset,1000,&handled));
 ui_follow_scroll_reset(&scroll);offset=0;scroll_feed(700,1,true,false,&offset,100,&handled);assert(scroll_feed(300,1,false,false,&offset,100,&handled)&&offset==100);assert(!scroll_feed(200,1,false,false,&offset,100,&handled)&&offset==100);scroll_feed(200,0,false,true,&offset,100,&handled);
 ui_follow_scroll_reset(&scroll);assert(!scroll_feed(1096,1,true,false,&offset,100,&handled)&&!scroll.tracking);assert(!scroll_feed(140,1,true,false,&offset,100,&handled)&&!scroll.tracking);
 ui_follow_scroll_reset(&scroll);scroll_feed(500,1,true,false,&offset,100,&handled);assert(!scroll_feed(500,0,false,true,&offset,100,&handled)&&!handled&&!scroll.settle_pending);
 printf("PASS: native power capture/release, 428 one-pixel drags, local raster bounds, no rectangular circle halo, fault/multitouch/clock cancellation, no end-tap shutdown, circle corners; list 1:1/reverse/clamps/one settle/faults and stationary taps\n");
 if(argc>1){ui_power_dialog_reset(&dialog,false);text_log=true;ui_power_dialog_draw(fb,&dialog);FILE *file=fopen(argv[1],"wb");assert(file);fprintf(file,"P5\n%d %d\n255\n",W,H);for(int y=0;y<H;++y)for(int x=0;x<W;++x)fputc(epd_get_pixel(x,y,W,H,fb)*17/16,file);fclose(file);
 if(argc>2){ui_fill_round_rect(fb,(EpdRect){UI_POWER_TRACK_X,UI_POWER_TRACK_Y,UI_POWER_TRACK_W,UI_POWER_TRACK_H},UI_POWER_TRACK_H/2,0xe0);ui_draw_control_frame(fb,(EpdRect){UI_POWER_TRACK_X,UI_POWER_TRACK_Y,UI_POWER_TRACK_W,UI_POWER_TRACK_H},UI_POWER_TRACK_H/2,0x60);
 file=fopen(argv[2],"wb");assert(file);fprintf(file,"P5\n%d %d\n255\n",W,H);for(int y=0;y<H;++y)for(int x=0;x<W;++x)fputc(epd_get_pixel(x,y,W,H,fb)*17/16,file);fclose(file);}
 }
}
'''
with tempfile.TemporaryDirectory(prefix='kiikoread-power-',dir='/private/tmp') as folder:
    c,binary=Path(folder)/'test.c',Path(folder)/'test';c.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',
        '-I'+str(ROOT/'tools/ui_gesture_stubs'),'-I'+str(ROOT/'main/ui'),str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)]+sys.argv[1:],check=True)
