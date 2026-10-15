"""Exercise bounded real cover batches, cache-first loading and cancellation.
SPDX-License-Identifier: Apache-2.0
"""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
ROOT=Path(__file__).resolve().parents[1]
book=ROOT/'main/apps/app_book.c'
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {SHELF,MANAGE};
typedef struct {bool touched;} touch_t;
typedef struct {int leaf;bool request_app,request_return,request_menu;int64_t now_ms;touch_t *touch;} app_ctx_t;
static unsigned s_cover_warm_mask,warm_calls;
static int64_t s_cover_warm_after_ms;
static bool s_clear_confirm,s_batch_confirm;
static int lock_style=2;
static int app_settings_lock_style(void){return lock_style;}
static int s_view=SHELF,s_visible_count=9,nav_stop=-1,rows=9;
static unsigned s_cover_pending_mask,s_cover_decode_mask,s_cover_dirty_mask,s_cover_present_mask,jobs,decode_jobs,nav_polls,conversion_calls;
static bool s_cover_batch_present_pending;
enum {EPD_DRAW_SUCCESS,EPD_DRAW_FAULT};
enum {APP_REDRAW_AREA,APP_REDRAW_PAGE,APP_REDRAW_FULL};
static int64_t now,cost=1000;
static bool hits=true,fast,fail,fast_hits;
static unsigned cached_mask=511,failed_mask;
static struct {int index;uint8_t *gray,*fast_bits;} s_covers[9];
static struct {char path[32],name[32],author[32];bool removed;} s_shelf[9];
static bool book_lock_collage_warm_cover(const char *path){assert(path==s_shelf[0].path||path==s_shelf[1].path);++warm_calls;return true;}
static int shelf_rows(void){return rows;}
static int64_t esp_timer_get_time(void){return now;}
static bool app_poll_main_navigation(app_ctx_t *ctx){++nav_polls;if(nav_stop>=0&&(int)nav_polls>=nav_stop){ctx->request_app=true;return true;}return false;}
static uint8_t *load_shelf_cover_gray(int row,const char *p,const char *n,const char *a,bool decode,bool *pending){
 assert(row>=0&&row<9&&p==s_shelf[row].path&&n==s_shelf[row].name&&a==s_shelf[row].author);
 ++jobs;now+=cost;if(decode)++decode_jobs;
 *pending=!decode&&(!hits||!(cached_mask&(1u<<row)));return fail||(failed_mask&(1u<<row))||*pending?NULL:malloc(1);
}
static void prepare_fast_covers(void){++conversion_calls;if(!fast)return;for(int i=0;i<9;++i)if(s_covers[i].gray){assert(!s_covers[i].fast_bits);s_covers[i].fast_bits=s_covers[i].gray;s_covers[i].gray=NULL;}}
'''
unit+=r'''
static bool load_shelf_cover_fast(int row,int index){assert(row==index);if(s_view!=SHELF||!fast||!fast_hits)return false;++jobs;now+=cost;s_covers[row].fast_bits=malloc(1);return true;}
static void save_shelf_cover_fast(int row,int index){assert(row==index);}
'''
unit+=function('prepare_cover_batch',book)+'\n'+function('shelf_cover_frame_ready',book)+'\n'+function('shelf_cover_reveal_mask',book)+'\n'+function('warm_shelf_lock_cover',book)+'\n'
present=function('present',book)
ack=present[present.index('    if ((s_view==SHELF || s_view==MANAGE) && err==EPD_DRAW_SUCCESS) {'):present.index('    if (err==EPD_DRAW_SUCCESS && reader_status_visible()')]
unit+='static void ack_present(int redraw,int err){\n'+ack+'\n}\n'

unit+=r'''
static void reset(app_ctx_t *ctx){
 for(int i=0;i<9;++i){free(s_covers[i].gray);free(s_covers[i].fast_bits);s_covers[i].gray=s_covers[i].fast_bits=NULL;s_covers[i].index=i;s_shelf[i].removed=false;}
 s_cover_pending_mask=(1u<<rows)-1;s_cover_decode_mask=s_cover_dirty_mask=s_cover_present_mask=0;jobs=decode_jobs=nav_polls=conversion_calls=0;now=0;cost=1000;nav_stop=-1;hits=true;fail=fast=fast_hits=false;cached_mask=511;failed_mask=0;ctx->request_app=false;
}
int main(void){
 app_ctx_t ctx={0};reset(&ctx);
 assert(prepare_cover_batch(&ctx,true)==511&&jobs==9&&decode_jobs==0&&s_cover_pending_mask==0);
 assert(s_cover_dirty_mask==511);
 // 缓存已加载但页面尚未推屏，下一次没有新解码仍保留全部脏区域。
 assert(prepare_cover_batch(&ctx,false)==0&&jobs==9&&s_cover_dirty_mask==511);
 s_cover_present_mask=7;s_cover_batch_present_pending=true;
 ack_present(APP_REDRAW_AREA,EPD_DRAW_FAULT);assert(s_cover_dirty_mask==511&&!s_cover_present_mask);
 s_cover_present_mask=7;ack_present(APP_REDRAW_AREA,EPD_DRAW_SUCCESS);assert(s_cover_dirty_mask==504);
 assert(prepare_cover_batch(&ctx,false)==0&&s_cover_dirty_mask==504);
 ack_present(APP_REDRAW_PAGE,EPD_DRAW_FAULT);assert(s_cover_dirty_mask==504);
 ack_present(APP_REDRAW_PAGE,EPD_DRAW_SUCCESS);assert(!s_cover_dirty_mask);
 s_cover_dirty_mask=511;ack_present(APP_REDRAW_FULL,EPD_DRAW_SUCCESS);assert(!s_cover_dirty_mask);
 reset(&ctx);hits=false;assert(prepare_cover_batch(&ctx,true)==0&&jobs==9&&s_cover_pending_mask==511&&s_cover_decode_mask==511);
 assert(prepare_cover_batch(&ctx,false)==7&&decode_jobs==3&&s_cover_pending_mask==504);
 assert(prepare_cover_batch(&ctx,false)==56&&decode_jobs==6&&s_cover_pending_mask==448);
 assert(prepare_cover_batch(&ctx,false)==448&&decode_jobs==9&&!s_cover_pending_mask);
 // 即便缓存读取超过旧100ms预算，也读齐一页才推第一帧，不拆为四本一批。
 reset(&ctx);cost=30000;assert(prepare_cover_batch(&ctx,true)==511&&jobs==9&&now==270000&&!s_cover_pending_mask);
 assert(shelf_cover_reveal_mask()==511);
 // 三种刷新模式、九本和四本列表：冷缓存分批准备，视觉上只提交完整的一组。
 for(int mode=0;mode<3;++mode)for(int list=0;list<2;++list){
  rows=list?4:9;reset(&ctx);hits=false;fast=mode==1;unsigned all=(1u<<rows)-1;
  assert(!prepare_cover_batch(&ctx,true)&&!shelf_cover_frame_ready()&&!shelf_cover_reveal_mask());
  unsigned frames=0;
  while(s_cover_pending_mask){
   unsigned before_decode=decode_jobs;prepare_cover_batch(&ctx,false);
   assert(decode_jobs-before_decode<=3);
   if(s_cover_pending_mask){assert(!shelf_cover_reveal_mask());ack_present(APP_REDRAW_PAGE,EPD_DRAW_SUCCESS);assert(s_cover_dirty_mask);}
   else {assert(shelf_cover_reveal_mask()==all);++frames;}
  }
  assert(frames==1&&decode_jobs==(unsigned)rows);
  s_cover_present_mask=all;s_cover_batch_present_pending=true;
  ack_present(APP_REDRAW_AREA,EPD_DRAW_FAULT);assert(shelf_cover_reveal_mask()==all);
  s_cover_present_mask=all;ack_present(APP_REDRAW_AREA,EPD_DRAW_SUCCESS);assert(!shelf_cover_reveal_mask());
 }
 rows=9;reset(&ctx);cached_mask=7;
 assert(prepare_cover_batch(&ctx,true)==7&&s_cover_pending_mask==504&&!shelf_cover_reveal_mask());
 ack_present(APP_REDRAW_FULL,EPD_DRAW_SUCCESS);assert(s_cover_dirty_mask==7);
 assert(prepare_cover_batch(&ctx,false)==56&&!shelf_cover_reveal_mask());
 failed_mask=256;assert(prepare_cover_batch(&ctx,false)==192&&!s_cover_pending_mask&&shelf_cover_reveal_mask()==255);
 // 慢解码仍遵循100ms文件间预算，失败封面使用原标题回退，不无限等待。
 reset(&ctx);hits=false;prepare_cover_batch(&ctx,true);cost=60000;
 assert(prepare_cover_batch(&ctx,false)==3&&decode_jobs==2&&s_cover_pending_mask==508);
 assert(!shelf_cover_reveal_mask());
 reset(&ctx);nav_stop=2;assert(prepare_cover_batch(&ctx,false)==1&&jobs==1&&ctx.request_app&&s_cover_pending_mask==510);
 reset(&ctx);s_view=MANAGE;nav_stop=1;assert(prepare_cover_batch(&ctx,true)==511&&nav_polls==0&&!ctx.request_app);s_view=SHELF;
 reset(&ctx);s_shelf[2].removed=true;s_covers[4].index=42;s_visible_count=7;
 assert(prepare_cover_batch(&ctx,true)==(1|2|8|32|64)&&jobs==5&&!s_cover_pending_mask);s_visible_count=9;
 reset(&ctx);fast=true;assert(prepare_cover_batch(&ctx,true)==511);for(int i=0;i<9;++i)assert(!s_covers[i].gray&&s_covers[i].fast_bits);
 reset(&ctx);fast=fast_hits=true;hits=false;assert(prepare_cover_batch(&ctx,true)==511&&jobs==9&&decode_jobs==0);for(int i=0;i<9;++i)assert(!s_covers[i].gray&&s_covers[i].fast_bits);
 reset(&ctx);fail=true;assert(prepare_cover_batch(&ctx,false)==0&&jobs==9&&!s_cover_pending_mask);
 reset(&ctx);touch_t touch={true};ctx.touch=&touch;assert(prepare_cover_batch(&ctx,false)==0&&jobs==0&&s_cover_pending_mask==511);ctx.touch=NULL;
 // 进入书架时手指尚未松开也可读缓存，后台首次解码仍在触摸期间让路。
 reset(&ctx);ctx.touch=&touch;assert(prepare_cover_batch(&ctx,true)==511&&jobs==9&&shelf_cover_frame_ready());ctx.touch=NULL;
 reset(&ctx);s_cover_pending_mask=0;
 s_cover_warm_mask=3;s_cover_warm_after_ms=1500;ctx.now_ms=1400;
 warm_shelf_lock_cover(&ctx);assert(!warm_calls&&s_cover_warm_mask==3);
 ctx.now_ms=1500;warm_shelf_lock_cover(&ctx);assert(warm_calls==1&&s_cover_warm_mask==2);
 warm_shelf_lock_cover(&ctx);assert(warm_calls==1);ctx.now_ms=2500;
 touch.touched=true;ctx.touch=&touch;warm_shelf_lock_cover(&ctx);assert(warm_calls==1);ctx.touch=NULL;
 ctx.request_menu=true;warm_shelf_lock_cover(&ctx);assert(warm_calls==1);ctx.request_menu=false;
 lock_style=0;warm_shelf_lock_cover(&ctx);assert(warm_calls==1);lock_style=2;
 s_cover_pending_mask=1;warm_shelf_lock_cover(&ctx);assert(warm_calls==1);s_cover_pending_mask=0;
 warm_shelf_lock_cover(&ctx);assert(warm_calls==2&&!s_cover_warm_mask);
 // Presentation must never load more covers after the tick has rendered its dirty rectangle.
 for(int i=0;i<9;++i){free(s_covers[i].gray);free(s_covers[i].fast_bits);}
 puts("PASS: whole-page cache reads even past 100ms, one complete cover reveal in all three modes/grid/list, placeholder acknowledgement retains dirty artwork, bounded cold decoding, cancellation, press deferral, failed-display retry, missing/OOM fallback and exclusive format ownership");
}
'''
source=book.read_text()
assert 's_shelf_feedback_pending || s_cover_batch_present_pending' in function('present',book)
assert 's_cover_batch_present_pending = true;' in function('on_tick',book)
assert 'unsigned changed=shelf_cover_reveal_mask();' in function('on_tick',book)
assert 'const bool artwork = shelf_cover_frame_ready();' in function('draw_shelf_cover',book)
assert 'err==EPD_DRAW_SUCCESS' in function('present',book) and 's_cover_dirty_mask &= ~s_cover_present_mask' in function('present',book)
assert 's_cover_batch_present_pending = false;' in function('present',book)
with tempfile.TemporaryDirectory(dir=ROOT/'build') as directory:
    d=Path(directory);c=d/'batch.c';exe=d/'batch';c.write_text(unit)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-isysroot','/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
