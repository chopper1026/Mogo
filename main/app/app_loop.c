/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 事件循环。只做和页面无关的事：读触摸并去抖、三个实体键与菜单把手的路由、切页
 * 时调 on_exit/on_enter、按回调返回值决定怎么刷屏、电源键锁屏、SD 卡上的字体延迟
 * 加载。页面自己的状态和刷新节奏都在 main/apps/ 各自的文件里。
 *
 * Event loop for page-agnostic work: touch debounce, the three keys and
 * the menu handle, on_exit/on_enter, present, lock, delayed SD font load.
 * Page state and refresh cadence stay in main/apps/; gesture recognition is shared.
 *
 * 冻结：主循环只产出手势、不解释含义；产品系统页接管三键，按用户要求
 * 左右返回、中键首页；阅读页保留翻页、页面设置和长按回书架。把手仍按下即触发。
 * 接管页可选长按键回调，保留按下行为；同键单指500ms仅触发一次，中断取消。
 * Frozen: Produce gestures without interpreting page actions. Product system pages
 * own Back/Home/Back keys; reading retains page turns, settings and hold-to-shelf.
 * Owners may opt into a single 500ms same-key hold callback after the press action; interruptions cancel it.
 * The menu handle still fires on press.
 * 用户修订：四个主页面底栏作为统一导航路由在按下时报点即响应，模态和阅读手势保持原归属。
 * User revision: root-tab routing commits on initial contact; modal controls and reader gestures retain their owners.
 * 用户修订：系统页面切换使用 GL16，不再每次 GC16 全刷；锁屏、手动强刷和周期清残影仍可全刷。
 * User revision: system page changes use GL16 instead of GC16; lock, manual refresh and periodic ghost cleanup may still use full refresh.
 * 用户修订：设置可选普通/快刷/水波纹，默认普通；普通保留原灰阶路线，快刷采用确认的黑白主页面和亚克力灰阶。用户反馈残影和黑带后，书架退出只同向擦白，主页水波纹改白底/24窄带且端点单向推动，中灰保留校准序列；五行横条局部移动、图标固定，没有异步补刷。
 * User revision: settings offer ordinary/fast/water, default ordinary. Ordinary retains the original gray path; fast uses approved monochrome main screens and gray acrylic. Following reported ghosts and dark bands, shelf exit whitens directionally; main water uses white backgrounds, 24 narrow bands and directional endpoints, retaining calibrated midgrays. Move the five-row marker locally, retain icons, and schedule no asynchronous completion.
 * 用户修订：普通模式也固定主页面底栏，仅局部移动横条；快刷亚克力采用规则一像素棋盘格，与已缓存封面一起输出。普通及水波纹保留灰阶，首次进入及解锁仍完整绘制底栏。
 * User revision: ordinary main screens also retain navigation and move only its marker locally; fast acrylic uses a regular one-pixel checkerboard in the same output as cached covers. Ordinary/water retain grays; initial entry and unlock still draw complete navigation.
 * 冻结：返回消费最近切页来源，恢复原页及菜单位置，不跳固定首页。
 * Frozen: Return consumes the latest page origin, restoring its page and menu position.
 * 用户采用顶部快捷层：仅顶部32px起手下滑72px才接管，覆盖时冻结页内输入/轮询，WiFi直接连接最近保存网络/再次断开，蓝牙切换开关，全刷和锁屏复用原路径。用户要求三个模式均真实灰阶并加深轮廓；关闭重绘当前页并局部整理，不修改底栏。
 * User adopts the top quick layer: capture only a 72px downward swipe starting within the top 32px; suspend page input/ticks while covered. WiFi connects saved credentials or disconnects on the next tap; Bluetooth toggles its setting. Reuse full refresh and lock. Per the user, all modes retain grays with clearer outlines. Closing redraws and locally settles the underlay, retaining navigation.
 * 用户点击反馈：底栏及圆形返回键按下局部放大，释放复位；只暂存一个小控件，切页、全刷及锁屏丢弃缓存，不回放旧画面。
 * User click feedback: locally enlarge navigation/round backs on press and restore on release. Retain one small control only; navigation, full refresh and lock discard it without replaying old frames.
 * 用户修订：确认主动锁屏后，拼贴样式先给局部圆锁反馈，再保存/等待阅读预渲染；自动休眠和直接关机/重启不加此反馈。
 * User revision: acknowledge accepted manual collage locks before saves/reader joins; omit the badge for automatic sleep and direct shutdown/restart.
 */

#include "app_loop.h"
#include "auto_lock.h"
#include "crash_note.h"

#include <string.h>

#include "app_registry.h"
#include "app_content_open.h"
#include "boot_state.h"
#include "ble_page_turner.h"
#include "ota_online.h"
#include "continuous_du.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "read_pico_crashlog.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "sleep.h"
#include "time_service.h"
#include "ui_avatar.h"
#include "book/book_cover_jobs.h"
#include "book/book_lock_collage.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_nav.h"
#include "ui_gesture.h"
#include "ui_power_dialog.h"
#include "ui_quick_menu.h"
#include "ui_click_feedback.h"
#include "usb_storage.h"
#include "touch_diag.h"
#include "touch_sampler.h"
#if CONFIG_TOUCH_DIAG && CONFIG_TOUCH_DIAG_BEEP
#include "read_pico_buzzer.h"
#endif
#include "local_latency_probe.h"

#define TAG "app_loop"

#if CONFIG_TOUCH_DIAG
// 丢点诊断计数与峰值；单实例，文件级即可。/ Drop-diag counters; single instance.
static int64_t td_last_read_us;      // 上次读触摸时刻 / previous cst836u_read time
static int64_t td_stats_ms;          // 上次统计输出时刻 / previous stats line
static int64_t td_err_last_ms;       // READ_ERR 限流 / READ_ERR rate limit
static unsigned td_err_burst;        // 限流窗口内堆积次数 / bursts hidden by the limit
static unsigned td_press, td_tap, td_swipe, td_longp, td_cancel;
static unsigned td_read_err, td_kill_err, td_multi, td_stall, td_present_over;
static unsigned td_replays, td_replay_frames;  // F4 回放事件与帧数 / replay events and frames
static int32_t td_max_stall_ms, td_max_present_ms;
static bool td_multi_active;         // 多点去抖 / multitouch transition latch
static const char* td_gest_name(ui_gesture_type_t t) {
    switch (t) {
        case UI_GESTURE_PRESS: return "PRESS";
        case UI_GESTURE_MOVE: return "MOVE";
        case UI_GESTURE_LONG_PRESS: return "LONG";
        case UI_GESTURE_TAP: return "TAP";
        case UI_GESTURE_SWIPE_L: return "SWIPE_L";
        case UI_GESTURE_SWIPE_R: return "SWIPE_R";
        case UI_GESTURE_SWIPE_U: return "SWIPE_U";
        case UI_GESTURE_SWIPE_D: return "SWIPE_D";
        case UI_GESTURE_CANCEL: return "CANCEL";
        default: return "?";
    }
}
#if CONFIG_TOUCH_DIAG_BEEP
static void td_beep_task(void* arg) {
    (void)arg;
    (void)read_pico_buzzer_tone(2200, 60);
    vTaskDelete(NULL);
}
void touch_diag_beep(void) {
    if (xTaskCreate(td_beep_task, "tdbeep", 3072, NULL, 3, NULL) != pdPASS) return;
}
#else
void touch_diag_beep(void) { }
#endif
void touch_diag_block(const char* src, int64_t t0_us) {
    int32_t ms = (int32_t)((esp_timer_get_time() - t0_us) / 1000);
    if (ms > 50) TD_LOGI("BLOCK src=%s ms=%d", src, ms);
}
#endif  // CONFIG_TOUCH_DIAG

// 触摸芯片报点率远高于屏幕，主循环空转一轮只等这么久。
// Touch reports far faster than the panel; idle this long per loop.
#define LOOP_TICK_MS 5
// SD 卡插上或者刚挂好时，隔一会儿再试一次存在设置里的字体。
// Retry the saved SD font shortly after a card appears or mounts.
#define FONT_RETRY_INTERVAL_MS 750
#define MEDIA_POLL_INTERVAL_MS 500

static int64_t s_lock_ignore_until_ms;
extern const app_desc_t app_transfer;

static const app_desc_t* s_presented_main;
static ui_quick_menu_t s_quick;
static unsigned s_quick_status;
// 锁屏占用整屏后旧主页面底栏不再是物理基准；解锁第一帧必须完整绘制。
// A lock screen replaces the old navigation baseline; the first unlocked frame must render complete navigation.
static void main_lock_boundary(void) {
    ui_click_feedback_reset();
    ui_quick_menu_reset(&s_quick);
    s_presented_main = NULL;
    display_main_transition_cancel();
}

static unsigned quick_status(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    bool wifi = status.mode == READ_PICO_TRANSFER_MODE_STA && status.state != READ_PICO_TRANSFER_STOPPED;
    return (wifi ? 1u : 0u) | (app_settings_ble_turner() ? 2u : 0u) | (status.network_ready ? 4u : 0u);
}

// 同步输出完再接收下一条触摸，不安排旧层回放；错误时关闭层并可靠全刷当前页。
// Complete presentation before accepting another contact, with no stale replay; close and full-refresh the current page on error.
static void quick_present(app_ctx_t *ctx, const app_desc_t *current, bool full) {
    ui_click_feedback_begin(ctx->fb);
    display_main_transition_cancel();
    if (current->render) current->render(ctx, ctx->fb);
    s_quick_status = quick_status();
    if (s_quick.open) ui_quick_menu_draw(ctx->fb, s_quick_status & 1u, s_quick_status & 2u);
    enum EpdDrawError error = full ? update_display_full(ctx->hl)
        : update_display_area_full_with(ctx->hl, &E0470_WAVEFORM, MODE_GL16, ui_quick_menu_rect());
    guard_draw_result(ctx->hl, error);
    if (error != EPD_DRAW_SUCCESS) {
        ui_quick_menu_reset(&s_quick);
        app_present(ctx, current, APP_REDRAW_FULL);
    }
}

static void quick_toggle_wifi(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.state != READ_PICO_TRANSFER_STOPPED) {
        // 上传期间拒绝断开，复用存储任务已有的原子准入保护。
        // Refuse disconnecting during upload using the storage owner's atomic admission guard.
        (void)read_pico_transfer_try_stop_if_idle();
    } else {
        char ssid[33] = {0}; bool configured = false;
        if (read_pico_transfer_get_saved_wifi(ssid, &configured) == ESP_OK && configured) {
            // 先释放蓝牙占用，再用既有异步连接/重试流程；不新开上传服务器或改写凭据。
            // Release Bluetooth resources first, then reuse connection/retry handling without starting uploads or rewriting credentials.
            ble_pt_sync(false);
            const read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
            (void)read_pico_transfer_start(&cfg);
        }
    }
}
static bool main_page_ready(app_ctx_t* ctx, const app_desc_t* app);

static void main_present_finished(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_DONE || !main_page_ready(ctx, app)) s_presented_main = NULL;
    else if (redraw != APP_REDRAW_NONE) s_presented_main = app;
    display_main_transition_disarm();
}

void app_lock_ignore_for(int64_t ms) {
    s_lock_ignore_until_ms = esp_timer_get_time() / 1000 + ms;
}

#if CONFIG_TOUCH_DIAG
static void app_present_impl(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw) {
#else
void app_present(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw) {
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_start=esp_timer_get_time();
    extern int64_t local_display_elapsed_us;
    const int64_t probe_display=local_display_elapsed_us;
#define LATENCY_PRESENT_DONE() do { if(redraw==APP_REDRAW_PAGE||redraw==APP_REDRAW_FULL) \
    ESP_LOGI("latency","PRESENT page=%d redraw=%d total_us=%lld display_us=%lld paint_us=%lld", \
    app_index_of(app),redraw,(long long)(esp_timer_get_time()-probe_start), \
    (long long)(local_display_elapsed_us-probe_display), \
    (long long)(esp_timer_get_time()-probe_start-(local_display_elapsed_us-probe_display))); } while(0)
#else
#define LATENCY_PRESENT_DONE() do {} while(0)
#endif
#endif
    enum EpdDrawError result = EPD_DRAW_SUCCESS;
    if (redraw == APP_REDRAW_PAGE && ui_click_feedback_active()) {
        // 切页固定底栏之前先复位真实按压像素，避免把放大的图标当作长期基准。
        // Restore physical pressed pixels before freezing navigation, so an enlarged icon never becomes its baseline.
        EpdRect area;
        if (ui_click_feedback_release(ctx->fb, &area)) {
            display_main_transition_cancel();
            result = update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
            guard_draw_result(ctx->hl, result);
            if (result != EPD_DRAW_SUCCESS) redraw = APP_REDRAW_FULL;
        }
    }
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) ui_click_feedback_begin(ctx->fb);
    ui_text_set_system_scale(true);
    // 后台目录/封面完成及滚动仍属于当前主页面，不能借普通整页出口闪到底栏。
    // Background directory/cover completion and scrolling remain on this main page and must not flash navigation via ordinary page output.
    if ((redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_AREA) &&
        s_presented_main == app && main_page_ready(ctx, app)) display_main_transition_arm(app, false, false);
    if (app->present != NULL && app->present(ctx, redraw)) {
        main_present_finished(ctx, app, redraw);
        LATENCY_PRESENT_DONE();
        return;
    }
    switch (redraw) {
        case APP_REDRAW_NONE:
        case APP_REDRAW_DONE:
            main_present_finished(ctx, app, redraw);
            return;
        case APP_REDRAW_AREA: {
            // 回调已经画好了 fb，这里只负责把它那一块推上屏。
            // The callback already painted fb; push that rectangle only.
            EpdRect area = app->area_hint != NULL
                ? app->area_hint(ctx)
                : ui_content_refresh_area();
            result = update_display_area_with(
                ctx->hl, &E0470_WAVEFORM, APP_DYNAMIC_REFRESH_MODE, area
            );
            break;
        }
        case APP_REDRAW_FULL:
            if (app->render != NULL) app->render(ctx, ctx->fb);
            result = APP_PAGE_FORCE_FULL
                ? update_display_full(ctx->hl)
                : update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE);
            break;
        case APP_REDRAW_PAGE:
        default:
            if (app->render != NULL) app->render(ctx, ctx->fb);
            result = update_display_fast_page(ctx->hl);
            break;
    }
    guard_draw_result(ctx->hl, result);
    main_present_finished(ctx, app, redraw);
    LATENCY_PRESENT_DONE();
#undef LATENCY_PRESENT_DONE
}

#if CONFIG_TOUCH_DIAG
void app_present(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw) {
    int64_t t0 = esp_timer_get_time();
    app_present_impl(ctx, app, redraw);
    int32_t ms = (int32_t)((esp_timer_get_time() - t0) / 1000);
    // 刷新阻塞窗口是“点击落空”的头号嫌疑，超过 100ms 的都留痕。
    // Refresh blocking is the prime suspect for lost taps; record every window over 100ms.
    if (ms > 100) {
        if (ms > td_max_present_ms) td_max_present_ms = ms;
        td_present_over++;
        TD_LOGI("PRESENT ms=%d redraw=%d", ms, (int)redraw);
    }
}
#endif
// 反馈只推控件附近，失败后重新绘制当前页面；不占用底栏切页许可。
// Push feedback only near its control, repainting the current page on failure without consuming navigation permission.
static void click_present(app_ctx_t *ctx, const app_desc_t *current, EpdRect area) {
    display_main_transition_cancel();
    enum EpdDrawError error = update_display_area_diff_with(ctx->hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
    guard_draw_result(ctx->hl, error);
    if (error != EPD_DRAW_SUCCESS) app_present(ctx, current, APP_REDRAW_FULL);
}

// 中断边界先清页面装饰再清识别器。/ Clear decoration before resetting recognition.
static void cancel_gesture(app_ctx_t* ctx, const app_desc_t* app, ui_gesture_t* gesture) {
    if (gesture->active && app->on_gesture) {
        ui_gesture_event_t event = { .type = UI_GESTURE_CANCEL,
            .x0 = gesture->x0, .y0 = gesture->y0, .x = gesture->x, .y = gesture->y };
        const app_desc_t* requested = ctx->request_app;
        bool menu_requested = ctx->request_menu;
        bool return_requested = ctx->request_return;
        app_redraw_t redraw = app->on_gesture(ctx, &event);
        ctx->request_app = requested;
        ctx->request_menu = menu_requested;
        ctx->request_return = return_requested;
        ui_gesture_reset(gesture);
        app_present(ctx, app, redraw);
    }
    ui_gesture_reset(gesture);
}

static void present_page(app_ctx_t* ctx, const app_desc_t* app,
                         ui_gesture_t* gesture, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL)
        cancel_gesture(ctx, app, gesture);
    app_present(ctx, app, redraw);
}

typedef struct {
    unsigned updates;
    int64_t last_ms;
    EpdRect area;
} menu_feedback_t;

// 菜单反馈只推叶内单行。/ Menu feedback pushes one local row only.
static void menu_feedback(app_ctx_t* ctx, const app_desc_t* current,
                          int leaf, int hit, bool pressed, menu_feedback_t* feedback) {
    EpdRect area;
    int row = hit - leaf * UI_MENU_ITEMS_PER_PAGE;
    if (!ui_menu_row_rect(leaf, row, &area)) return;
    ui_draw_menu_row_pressed(ctx->fb, current, leaf, row, pressed);
    if (feedback->updates == 0) feedback->area = area;
    else {
        int bottom = feedback->area.y + feedback->area.height;
        int next_bottom = area.y + area.height;
        if (area.y < feedback->area.y) feedback->area.y = area.y;
        feedback->area.height = (bottom > next_bottom ? bottom : next_bottom) - feedback->area.y;
    }
    feedback->updates++;
    feedback->last_ms = ctx->now_ms;
    guard_draw_result(ctx->hl, update_display_area_with(
        ctx->hl, &E0470_WAVEFORM, APP_DYNAMIC_REFRESH_MODE, area));
    if (feedback->updates >= UI_SETTLE_DU_MAX) {
        guard_draw_result(ctx->hl, update_display_area_with(
            ctx->hl, &E0470_WAVEFORM, MODE_GL16, feedback->area));
        feedback->updates = 0;
    }
}

// 切页：先让上一页收尾，再给新页一次 on_enter，最后整页画出来。
// Leave the old page, enter the new one, then present a full page.
static bool main_page_ready(app_ctx_t* ctx, const app_desc_t* app) {
    int index = app_index_of(app);
    return index >= 0 && index < 4 &&
           app->main_page_visible && app->main_page_visible(ctx);
}

static void switch_to(
    app_ctx_t* ctx, const app_desc_t** current, const app_desc_t* next
) {
    if (next == NULL || next == *current) return;
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_start=esp_timer_get_time();
    const int probe_from=app_index_of(*current);
#endif
    bool main_source = main_page_ready(ctx, *current);
    bool shelf_source = main_source && app_index_of(*current) == 1;
    display_main_transition_cancel();
    if ((*current)->on_exit != NULL) (*current)->on_exit(ctx);
    // 传书页独占无线：进页前取消在途的后台对时，页面的会话才起得来。
    // The transfer page owns the radio: an in-flight background sync cancels before entry, or the
    // page's own session cannot start.
    if (next == &app_transfer) app_time_service_cancel();
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_exit=esp_timer_get_time();
#endif
    *current = next;
    app_font_activate_system();
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_font=esp_timer_get_time();
#endif
    if (next->on_enter != NULL) next->on_enter(ctx);
#ifdef KIIKO_LOCAL_LATENCY
    const int64_t probe_enter=esp_timer_get_time();
#endif
    if (main_source && main_page_ready(ctx, next)) display_main_transition_arm(next, shelf_source, true);
    app_present(ctx, next, next->enter_full ? APP_REDRAW_FULL : APP_REDRAW_PAGE);
    display_main_transition_disarm();
#ifdef KIIKO_LOCAL_LATENCY
    ESP_LOGI("latency","SWITCH from=%d to=%d mode=%d exit_us=%lld font_us=%lld enter_us=%lld present_us=%lld total_us=%lld",probe_from,app_index_of(next),app_settings_main_refresh_mode(),
        (long long)(probe_exit-probe_start),(long long)(probe_font-probe_exit),(long long)(probe_enter-probe_font),
        (long long)(esp_timer_get_time()-probe_enter),(long long)(esp_timer_get_time()-probe_start));
#endif
    ESP_LOGI(TAG, "page -> %s", next->title);
}

// 菜单页不是 app，单独画。/ The menu is not an app; present it here.
static void present_menu(app_ctx_t* ctx, const app_desc_t* current, int leaf, menu_feedback_t* feedback) {
    s_presented_main = NULL;
    feedback->updates = 0;
    if (display_take_white_exit()) {
        guard_draw_result(ctx->hl, update_display_white(ctx->hl));
    }
    ui_draw_menu_page(ctx->fb, current, leaf);
    guard_draw_result(ctx->hl, update_display_fast_page(ctx->hl));
}

// 空槽常带着抬起事件或残留坐标，不能进页面看到的快照；抬手后保留最后一次位置。
// Empty slots often carry lift events or stale coords; keep last position on lift.
static void debounce_touch(
    cst836u_touch_t* latest, const cst836u_touch_t* raw, bool released
) {
    if (raw->touched) {
        *latest = *raw;
        for (int i = 0; i < CST836U_MAX_POINTS; i++) {
            if (!latest->points[i].active) {
                memset(&latest->points[i], 0, sizeof(latest->points[i]));
            }
        }
    } else if (released) {
        latest->touched = false;
        latest->count = 0;
        for (int i = 0; i < CST836U_MAX_POINTS; i++) {
            latest->points[i].active = false;
        }
    }
    memcpy(latest->raw, raw->raw, sizeof(raw->raw));
}

// ---- F4 逐帧触摸管线（正常 tick 与回放共用；docs/local/2026-10-14-f4-touch-sampler-plan.md §3.2）----

// 采样器未跑时的降级直读也复用的最近帧。/ Last popped frame for reuse when idle.
static touch_frame_t s_last_frame;

// 由采样帧重建触摸快照：多点 points 与原始寄存器不回放——多点帧只承担 count>1
// 的拒绝语义，raw 仅诊断 RAW_INVALID 消费而该事件在驱动内（采样侧）打出。
// / Rebuild a touch snapshot from a frame; multi-point payloads and raw regs
// are not replayed (count>1 rejection semantics only).
static void frame_to_touch(const touch_frame_t* f, cst836u_touch_t* t) {
    memset(t, 0, sizeof(*t));
    t->touched = f->touched != 0;
    t->count = f->count;
    t->x = f->x;
    t->y = f->y;
}

// 单帧摄取：边沿判定（相对 *was_touched）→ 诊断计数 → 去抖入 latest → ctx 字段。
// 正常 tick 在原直读位置调用一次；回放每帧调用一次。frame_ms：回放传帧时间戳
// （队列内单调，手势机倒退保护不触发），正常传真实时间。
// / Ingest one frame: edges vs *was_touched → diag counters → debounce into
// latest → ctx fields. Called once per normal tick, once per replayed frame.
static void ingest_touch_frame(app_ctx_t* ctx, cst836u_touch_t* latest,
                               const cst836u_touch_t* touch, esp_err_t err,
                               bool* was_touched, int64_t frame_ms,
                               int64_t* last_input_ms,
                               bool* out_pressed, bool* out_released) {
    const bool pressed = touch->touched && !*was_touched;
    const bool released = !touch->touched && *was_touched;
#if CONFIG_TOUCH_DIAG
    if (pressed) {
        td_press++;
        TD_LOGI("PRESS x=%u y=%u n=%u", touch->x, touch->y, touch->count);
    }
    if (err == ESP_OK && touch->touched && touch->count > 1) {
        if (!td_multi_active) {
            td_multi_active = true;
            td_multi++;
            TD_LOGI("MULTI n=%u x=%u y=%u", touch->count, touch->x, touch->y);
        }
    } else if (td_multi_active) td_multi_active = false;
#endif
    debounce_touch(latest, touch, released);
    ctx->now_ms = frame_ms;
    if (err == ESP_OK && (touch->touched || released)) *last_input_ms = frame_ms;
    ctx->pressed = pressed;
    ctx->released = released;
    ctx->consumed = false;
    *out_pressed = pressed;
    *out_released = released;
}

// 手势分发：原主循环 829-857 行整块原样抽取，正常 tick 与回放共用，语义与抽取前
// 一致。owned_by_other = 电源弹窗 / 快捷层 / 菜单任一持有输入时为真。
// / Gesture dispatch extracted verbatim from the main loop; shared by the
// normal tick and replay. owned_by_other covers power dialog / quick layer / menu.
static void dispatch_gesture_frame(app_ctx_t* ctx, const app_desc_t* current,
                                   ui_gesture_t* gesture, esp_err_t err,
                                   bool owned_by_other) {
    if (owned_by_other || !current->on_gesture || err != ESP_OK) return;
    ui_gesture_event_t event;
    if (ui_gesture_feed(gesture, ctx, &event)) {
#if CONFIG_TOUCH_DIAG
        // MOVE 逐帧太多不记；按下与终态全记。MOVE frames are too noisy; log press + terminal.
        if (event.type != UI_GESTURE_MOVE) {
            switch (event.type) {
                case UI_GESTURE_TAP: td_tap++; break;
                case UI_GESTURE_LONG_PRESS: td_longp++; break;
                case UI_GESTURE_CANCEL:
                    td_cancel++;
                    touch_diag_beep();
                    break;
                case UI_GESTURE_SWIPE_L: case UI_GESTURE_SWIPE_R:
                case UI_GESTURE_SWIPE_U: case UI_GESTURE_SWIPE_D:
                    td_swipe++;
                    break;
                default: break;
            }
            TD_LOGI("GEND t=%s x0=%u y0=%u x=%u y=%u ms=%d",
                    td_gest_name(event.type), event.x0, event.y0, event.x, event.y,
                    (int)event.hold_ms);
        }
#endif
        app_redraw_t redraw = current->on_gesture(ctx, &event);
        present_page(ctx, current, gesture, redraw);
        if (redraw != APP_REDRAW_NONE) ctx->consumed = true;
    }
}

static bool try_load_saved_sd_font(bool allow_probe) {
    read_pico_sd_info_t info;
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err == ESP_ERR_NOT_FINISHED) return false;
    if (!info.mounted) {
        if (allow_probe && err == ESP_ERR_INVALID_STATE) read_pico_sd_start_probe();
        return false;
    }
    return app_font_retry_active();
}

// 先停文件使用者，再换字体；此处只观察挂载，不卸载也不重新挂载。
// Stop file consumers before font fallback; observe mounting without unmounting or remounting.
static bool poll_media(app_ctx_t* ctx, const app_desc_t* current,
                       bool* mounted, bool* invalidated) {
    if (usb_storage_active()) { *mounted = false; *invalidated = false; return false; }
    read_pico_sd_info_t info = {0};
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err == ESP_ERR_INVALID_STATE && info.present && !*invalidated) {
        (void)read_pico_sd_start_probe();
        return false;
    }
    if (err == ESP_ERR_NOT_FINISHED) return false;
    bool ready = err == ESP_OK && info.mounted;
    bool lost = *mounted && !ready;
    bool gained = !*mounted && ready;
    *mounted = ready;
    if (ready) *invalidated = false;
    if (gained || lost) { ui_avatar_invalidate(); (void)book_cover_jobs_quiesce(0); book_lock_collage_invalidate(); }
    if (gained && current->on_media_ready) current->on_media_ready(ctx);
    if (!lost) return false;
    *invalidated = true;
    if (current->on_media_lost) current->on_media_lost(ctx);
    if (ttf_font_ready() && !ttf_font_is_builtin()) ttf_font_open_builtin();
    ui_text_set_system_font(false);
    return true;
}

// 电源动作先保存当前页面，再定稿一张不带时钟的静态画面；避免关机后留下会误读的冻结时间。
// Save the current page first, then settle a clock-free static frame so shutdown never leaves a misleading frozen time.
// 锁屏前先由页面保存进度，再持久化一次恢复上下文；不在翻页或点击时写入。
// Flush page progress before persisting one lock checkpoint; never write it on page turns or taps.
// 关机睡眠选「仅浅睡」时，所有锁屏入口都无期限浅睡、永不断电。
// With shutdown style "light-only" every lock entry sleeps without a deadline and never powers off.
static bool lock_light_only(void) {
    return app_settings_shutdown_style() == APP_SHUTDOWN_LIGHT;
}

static void prepare_lock(app_ctx_t *ctx, const app_desc_t *current, bool feedback) {
    if (feedback) app_lock_feedback(ctx->hl);
    if (current->on_before_lock) current->on_before_lock(ctx);
    int tab = app_index_of(current);
    pico_resume_t resume = {.tab = tab >= 0 && tab < 4 ? (uint8_t)tab : 0};
    bool fullscreen = false;
    if (tab == 1 && app_book_resume_context(resume.path, sizeof(resume.path), &fullscreen)) {
        resume.reader = 1;
        resume.fullscreen = fullscreen;
    }
    (void)pico_boot_save_resume(&resume);
}
static void run_power_action(app_ctx_t *ctx, const app_desc_t *current, bool restart) {
    prepare_lock(ctx, current, false);
    ui_power_final_draw(ctx->fb, restart);
    guard_draw_result(ctx->hl, update_display_full(ctx->hl));
    app_lock_wait_key_idle(800);
    epd_poweroff();
    pico_boot_clear_resume();
    if (restart) app_restart_host();
    esp_err_t error = app_enter_host_sleep(APP_SLEEP_OFF);
    if (error != ESP_OK) ESP_LOGW(TAG, "power off failed: %s", esp_err_to_name(error));
    // 关机交接失败时回到可操作原页；不关闭书籍后再重新自动开书。/ A failed shutdown returns to the operable current page without closing/reopening the book.
    app_present(ctx, current, APP_REDRAW_PAGE);
}

bool app_poll_main_navigation(app_ctx_t *ctx) {
    if (!ctx || !ctx->tp) return false;
    if (ctx->request_app) return true;
    cst836u_touch_t touch = {0};
    if (cst836u_read(ctx->tp, &touch) != ESP_OK || !touch.touched || touch.count != 1) return false;
    int index = ui_nav_hit(touch.x, touch.y);
    if (index < 0 || index == 1) return false;
    ui_nav_request(ctx, index);
    ctx->user_activity = true;
    return ctx->request_app != NULL;
}

void app_loop_run(const app_loop_config_t* config) {
    s_presented_main = NULL;
    const app_desc_t* current = config->first_app != NULL
        ? config->first_app
        : app_home_page();

    cst836u_info_t touch_info = { 0 };
    cst836u_get_info(config->tp, &touch_info);
    cst836u_touch_t latest = { 0 };

    app_ctx_t ctx = {
        .hl = config->hl,
        .fb = config->fb,
        .acc = config->acc,
        .tp = config->tp,
        .sensor_ready = config->sensor_ready,
        .continuous_ready = continuous_du_init(),
        .touch = &latest,
        .touch_info = &touch_info,
    };

    bool was_touched = false;
    ui_quick_menu_reset(&s_quick);
    bool menu_open = false;
    ui_gesture_t gesture = {0};
    ui_power_dialog_t power_dialog = {0};
    bool power_dialog_open = false;
    int menu_pressed = UI_MENU_HIT_NONE;
    menu_feedback_t feedback = {0};
    int menu_leaf = 0;
    const app_desc_t* return_app = NULL;
    int return_leaf = 0, return_menu_leaf = 0;
    bool return_to_menu = false;
    int held_key = -1;
    int64_t held_since_ms = 0;
    int64_t last_lock_poll_ms = 0;
    int64_t last_font_retry_ms = 0;
    int64_t last_media_poll_ms = 0;
    int64_t last_network_poll_ms = 0;
    int64_t last_input_ms = esp_timer_get_time() / 1000;
    uint32_t last_ble_input = ble_pt_input_serial();
    unsigned idle_minutes = app_settings_auto_lock_minutes();
    unsigned cover_policy = (unsigned)app_settings_main_refresh_mode() | ((unsigned)app_settings_system_contrast() << 8);
    read_pico_sd_info_t initial_media = {0};
    (void)read_pico_sd_get_info(&initial_media);
    bool media_mounted = initial_media.mounted || (ttf_font_ready() && !ttf_font_is_builtin());
    bool media_invalidated = false;
    app_lock_ignore_for(APP_LOCK_IGNORE_BOOT_MS);

    // The SD probe starts during board setup and is usually ready by the first product frame.
    // Retry here so the very first bookshelf paint already has a full CJK system face.
    try_load_saved_sd_font(false);
    rp_boot_mark("sdfont");
    if (current->on_enter != NULL) current->on_enter(&ctx);
    rp_boot_mark("enter");
    poll_media(&ctx, current, &media_mounted, &media_invalidated);
    // 首帧必须整屏 GC16：fb 里还是 app_main 画的开机图，DU 盖不掉。
    // First frame must be full GC16: fb still holds the splash; DU cannot cover it.
    app_present(&ctx, current, APP_REDRAW_FULL);
    rp_boot_mark("frame");
    pico_boot_ready();
    ESP_LOGI(TAG, "UI ready on %s", current->title);
    // 上次崩溃的现场（异常类型与触发地址）：有的话打一遍，串口和屏幕各有一份。
    // The previous panic scene, when there was one: logged here, and shown on the home screen.
    crash_note_log();
#ifdef KIIKO_LOCAL_LATENCY
    local_latency_init();
#endif

    // F4：主循环被刷屏等操作阻塞期间由采样任务持续读触摸，恢复后逐帧回放，边沿
    // 不丢；任务创建失败时 active() 为假，循环内自动回退直读（原行为）。
    // / F4: a dedicated task keeps sampling while the loop is blocked and frames
    // are replayed on wake; if the task cannot start, the loop reads directly.
    touch_sampler_start(config->tp);

    // 上一轮是否切过页/换过屏；只有这种残留导航才丢弃，普通刷新阻塞期间的点击必须放行。
    // Whether the previous iteration changed screens; only such stale navigation is dropped.
    bool just_transitioned = false;
    while (true) {
        unsigned requested_cover_policy = (unsigned)app_settings_main_refresh_mode() | ((unsigned)app_settings_system_contrast() << 8);
        if (requested_cover_policy != cover_policy) {
            app_home_cover_mode_changed(); app_book_cover_mode_changed();
            cover_policy = requested_cover_policy;
        }
        cst836u_touch_t touch = { 0 };
        esp_err_t err = ESP_OK;
        int64_t frame_ms = esp_timer_get_time() / 1000;
        bool replayed = false;
        bool pressed = false, released = false;
#if CONFIG_TOUCH_DIAG
        int64_t td_gap_ms = td_last_read_us ? (esp_timer_get_time() - td_last_read_us) / 1000 : 0;
        td_last_read_us = esp_timer_get_time();
#endif
        if (touch_sampler_active()) {
            touch_frame_t f;
            if (touch_sampler_pending() > 1) {
                // 从阻塞中恢复：一次性排空积压，逐帧走摄取+手势管线（回放）。
                // 回放期间 ctx.now_ms 取帧时间戳（队列内单调，不触发手势机倒退
                // 保护），排空后恢复真实时间；必须回放全部帧而非只回放边沿——
                // 手势 moved 判定依赖中间点轨迹（方案 §3.2）。出现页面切换请求
                // 即停止排空：剩余帧留待切页后下一轮，避免旧页多吃输入。
#if CONFIG_TOUCH_DIAG
                uint32_t n = 0;
                int64_t first_ms = 0, last_ms = 0;
#endif
                while (touch_sampler_pop(&f)) {
                    // 回放必须同步最近帧：复用路径的「边沿天然无变化」前提依赖
                    // s_last_frame 与 was_touched 同步；否则阻塞前残留的按下帧
                    // 会在回放排空后制造假按下沿（实测工具栏连点链，根因）。
                    // / Replay must keep the last-frame cache in sync: the reuse
                    // path assumes edges are unchanged, which breaks if the cache
                    // still holds a stale press frame (measured toolbar chain).
                    s_last_frame = f;
                    frame_to_touch(&f, &touch);
                    ingest_touch_frame(&ctx, &latest, &touch, ESP_OK, &was_touched,
                                       (int64_t)f.t_ms, &last_input_ms, &pressed, &released);
                    dispatch_gesture_frame(&ctx, current, &gesture, ESP_OK, menu_open);
                    was_touched = touch.touched;
#if CONFIG_TOUCH_DIAG
                    n++;
                    if (n == 1) first_ms = (int64_t)f.t_ms;
                    last_ms = (int64_t)f.t_ms;
#endif
                    if (ctx.request_app || ctx.request_menu || ctx.request_return) break;
                }
                replayed = true;
                frame_ms = esp_timer_get_time() / 1000;
#if CONFIG_TOUCH_DIAG
                td_replays++;
                td_replay_frames += n;
                TD_LOGI("REPLAY n=%u ms=%d", (unsigned)n, (int)(last_ms - first_ms));
#endif
            } else if (touch_sampler_pop(&f)) {
                s_last_frame = f;
                frame_to_touch(&s_last_frame, &touch);
            } else {
                // 无新帧：复用最近帧（采样节拍 5ms 与循环节拍不完全同相），边沿天然无变化。
                // / No new frame: reuse the last one; edges are unchanged by construction.
                frame_to_touch(&s_last_frame, &touch);
            }
        } else {
            err = cst836u_read(config->tp, &touch);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "CST836U read failed: %s", esp_err_to_name(err));
#if CONFIG_TOUCH_DIAG
                td_read_err++;
                // 手指按着时出错：本轮点击注定报废（后续边沿丢失）。单独计数。
                // Error while the finger is down: this tap is doomed (edge lost). Counted alone.
                if (was_touched) td_kill_err++;
                {
                    int64_t now_ms = esp_timer_get_time() / 1000;
                    if (now_ms - td_err_last_ms > 500) {
                        TD_LOGI("READ_ERR err=%s finger=%d kill=%u burst=%u",
                                esp_err_to_name(err), (int)was_touched, td_kill_err, td_err_burst);
                        td_err_last_ms = now_ms;
                        td_err_burst = 0;
                    } else td_err_burst++;
                }
#endif
                memset(&touch, 0, sizeof(touch));
            }
            frame_ms = esp_timer_get_time() / 1000;
        }
#if CONFIG_TOUCH_DIAG
        // STALL 仅正常迭代累计；回放迭代的阻塞窗口由 REPLAY 行承担证据链（方案 §3.3）。
        // 相邻两次处理间隔超过 50ms：这轮里循环被别的活儿阻塞（刷屏、字体、PMU…）。
        if (!replayed && td_gap_ms > 50) {
            td_stall++;
            if (td_gap_ms > td_max_stall_ms) td_max_stall_ms = (int32_t)td_gap_ms;
            TD_LOGI("STALL gap=%dms", (int)td_gap_ms);
        }
#endif
        if (!replayed) {
            ingest_touch_frame(&ctx, &latest, &touch, err, &was_touched,
                               frame_ms, &last_input_ms, &pressed, &released);
        }
        if (ctx.now_ms - last_network_poll_ms >= 500) {
            last_network_poll_ms = ctx.now_ms;
            TD_TIME("transfer_poll", read_pico_transfer_service_poll());
            // 后台对时是系统服务（同 poll_media / 字体重试），守卫在 time_service 内部；
            // 空闲门控沿用上游：触压、近期输入、独占 PMU 的页面、USB 与联网升级期间不动。
            // Background sync is a system service like poll_media or the font retry, with its
            // guards inside time_service; idle gating stays upstream: no touch, no recent input,
            // pages holding the PMU, USB or an online update.
            app_time_service_poll(!touch.touched && ctx.now_ms - last_input_ms >= 10000 &&
                !current->holds_pmu && !usb_storage_active() && !pico_online_busy() && !ble_pt_connected());
        }
        // 只丢弃「上一轮切过屏」时残留的导航：那是用户在新界面就绪前点到旧界面的动作。
        // 无条件清会连触摸采样器回放路径设好的 request_return 一起抹掉（停止传书按钮
        // 点不动即由此而来）；普通刷新阻塞期间的点击必须放行。
        // Drop navigation only right after a screen change; the unconditional clear also erased
        // requests set by the sampler's replay path (the dead transfer stop button).
        if (just_transitioned) {
            just_transitioned = false;
            ctx.request_menu = false;
            ctx.request_return = false;
        }
        bool selected_from_menu = false;
        const bool power_input_owned = power_dialog_open;
        bool quick_lock_requested = false;
        bool quick_input_owned = false;

        if (ctx.now_ms - last_media_poll_ms >= MEDIA_POLL_INTERVAL_MS) {
            last_media_poll_ms = ctx.now_ms;
            bool media_changed;
            TD_TIME("media_poll", media_changed = poll_media(&ctx, current, &media_mounted, &media_invalidated));
            if (media_changed) {
                power_dialog_open = false;
                ui_power_dialog_reset(&power_dialog, touch.touched);
                ui_quick_menu_reset(&s_quick);
                held_key = -1;
                cancel_gesture(&ctx, current, &gesture);
                menu_pressed = UI_MENU_HIT_NONE;
                ctx.consumed = true;
                ctx.pressed = ctx.released = false;
                was_touched = touch.touched;
                if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                else app_present(&ctx, current, APP_REDRAW_FULL);
                // 本轮旧输入不再触发动作，下一轮恢复正常事件处理。
                // Discard this tick's stale input, then resume normal dispatch next tick.
                just_transitioned = true;
                vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
                continue;
            }
        }

        if (!power_input_owned && !menu_open) {
            if (err != ESP_OK) ui_quick_menu_cancel_input(&s_quick);
            else {
                ui_quick_action_t action;
                quick_input_owned = ui_quick_menu_feed(&s_quick, &ctx,
                    current->render && !current->holds_pmu && !usb_storage_active() && !pico_online_busy(), &action);
                if (quick_input_owned) {
                    held_key = -1;
                    cancel_gesture(&ctx, current, &gesture);
                    ctx.consumed = true;
                }
                if (action == UI_QUICK_OPEN || action == UI_QUICK_CLOSE) quick_present(&ctx, current, false);
                else if (action == UI_QUICK_WIFI) {
                    quick_toggle_wifi();
                    quick_present(&ctx, current, false);
                } else if (action == UI_QUICK_BLUETOOTH) {
                    app_settings_set_ble_turner(!app_settings_ble_turner());
                    if (!app_settings_ble_turner()) ble_pt_reset_failure();
                    quick_present(&ctx, current, false);
                } else if (action == UI_QUICK_FULL) quick_present(&ctx, current, true);
                else if (action == UI_QUICK_LOCK) quick_lock_requested = true;
            }
        }

        if (power_input_owned && err == ESP_OK) {
            ctx.consumed = false;
            EpdRect dirty;
            ui_power_action_t action = ui_power_dialog_feed(&power_dialog, &ctx, &dirty);
            if (action == UI_POWER_ACTION_NONE && dirty.height > 0) {
                ui_power_dialog_draw_controls(ctx.fb, &power_dialog);
                enum EpdDrawError result = update_display_area_diff_with(
                    ctx.hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, dirty);
                guard_draw_result(ctx.hl, result);
                if (result != EPD_DRAW_SUCCESS) {
                    power_dialog_open = false;
                    ui_power_dialog_reset(&power_dialog, touch.touched);
                    app_present(&ctx, current, APP_REDRAW_FULL);
                }
            } else {
                if (action == UI_POWER_ACTION_CANCEL) {
                    power_dialog_open = false;
                    ui_power_dialog_reset(&power_dialog, false);
                    if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                    else app_present(&ctx, current, APP_REDRAW_PAGE);
                } else if (action == UI_POWER_ACTION_SHUTDOWN) {
                    app_shutdown_style_t style = app_settings_shutdown_style();
                    if (style == APP_SHUTDOWN_FULL) {
                        power_dialog_open = false;
                        ui_power_dialog_reset(&power_dialog, false);
                        run_power_action(&ctx, current, false);
                    } else {
                        power_dialog_open = false;
                        ui_power_dialog_reset(&power_dialog, false);
                        prepare_lock(&ctx, current, true);
                        extern const app_desc_t app_book;
                        main_lock_boundary();
                        // 先浅后深走十分钟阶梯；仅浅睡不断电，按键/拿起回到关机前页面。
                        // Staged walks the ten-minute ladder; light-only never powers off and key/pickup returns.
                        enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp,
                                             current == &app_book && app_book_reader_body_visible(),
                                             style == APP_SHUTDOWN_LIGHT);
                        display_request_full_refresh();
                        ctx.now_ms = esp_timer_get_time() / 1000;
                        last_input_ms = ctx.now_ms;
                        poll_media(&ctx, current, &media_mounted, &media_invalidated);
                        last_media_poll_ms = ctx.now_ms;
                        ctx.pressed = false;
                        ctx.released = false;
                        app_present(&ctx, current, APP_REDRAW_PAGE);
                    }
                } else if (action == UI_POWER_ACTION_RESTART) {
                    power_dialog_open = false;
                    ui_power_dialog_reset(&power_dialog, false);
                    run_power_action(&ctx, current, true);
                }
            }
            ctx.consumed = true;
        }

        if (err != ESP_OK) {
            held_key = -1;
#if CONFIG_TOUCH_DIAG
            // I2C 出错强杀手势跟踪：单指场景下等价于这次点击报废。
            // I2C error force-kills tracking: with one finger this forfeits the tap.
            if (gesture.active) {
                TD_LOGI("KILL_BY_ERR finger=%d", (int)was_touched);
                touch_diag_beep();
            }
#endif
            cancel_gesture(&ctx, current, &gesture);
            EpdRect dirty;
            ui_power_dialog_cancel_input(&power_dialog, &dirty);
            if (power_dialog_open && dirty.height > 0) {
                ui_power_dialog_draw_controls(ctx.fb, &power_dialog);
                guard_draw_result(ctx.hl, update_display_area_diff_with(
                    ctx.hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, dirty));
            }
            ctx.consumed = true;
            if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
            menu_pressed = UI_MENU_HIT_NONE;
        }
        EpdRect click_area;
        if (ui_click_feedback_active() && (err != ESP_OK || released || touch.count != 1 ||
            quick_input_owned || power_input_owned || menu_open ||
            ui_click_feedback_cancel_at(touch.x, touch.y))) {
            if (ui_click_feedback_release(ctx.fb, &click_area)) click_present(&ctx, current, click_area);
        }
        // 主页面底栏在首次单指报点就确认；抬手、轻滑和停留不再重复切页。
        // Commit root navigation on the first single contact; lift, small motion and holds never repeat it.
        bool nav_press_owned = false;
        if (err == ESP_OK && pressed && touch.count == 1 && !power_input_owned &&
            !quick_input_owned && !menu_open && !usb_storage_active() && !ctx.request_app &&
            main_page_ready(&ctx, current)) {
            const int tab = ui_nav_hit(touch.x, touch.y);
            if (tab >= 0) {
                held_key = -1;
                cancel_gesture(&ctx, current, &gesture);
                ctx.consumed = true;
                ctx.user_activity = true;
                nav_press_owned = true;
                if (tab != app_index_of(current)) ui_nav_request(&ctx, tab);
            }
        }
        if (err == ESP_OK && pressed && touch.count == 1 && !power_input_owned &&
            !quick_input_owned && !menu_open && !usb_storage_active() &&
            ui_click_feedback_press(ctx.fb, touch.x, touch.y, &click_area))
            click_present(&ctx, current, click_area);
        if (!power_input_owned && !quick_input_owned && !nav_press_owned && pressed && err == ESP_OK) {
            held_key = -1;
            const int key = ui_key_hit_test(touch.x, touch.y);
            const bool handle_hit = ui_menu_handle_hit_test(touch.x, touch.y) &&
                (!current->menu_handle_enabled || current->menu_handle_enabled(&ctx));
            ctx.consumed = true;
            if (key >= 0 || handle_hit) {
                cancel_gesture(&ctx, current, &gesture);
                if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
                menu_pressed = UI_MENU_HIT_NONE;
            }
            if (key >= 0 && current->owns_keys && !menu_open) {
                app_redraw_t redraw = current->on_key && !(key == UI_KEY_2 && current->defer_middle_short)
                    ? current->on_key(&ctx, key) : APP_REDRAW_NONE;
                present_page(&ctx, current, &gesture, redraw);
                if (current->on_key_long && touch.count == 1) {
                    held_key = key;
                    held_since_ms = ctx.now_ms;
                }
            } else if (key == UI_KEY_2) {
                if (menu_open) {
                    feedback.updates = 0;
                    ui_draw_menu_page(ctx.fb, current, menu_leaf);
                    guard_draw_result(ctx.hl, update_display_full(ctx.hl));
                } else if (current->present != NULL && current->present(&ctx, APP_REDRAW_FULL)) {
                } else if (current->render != NULL) {
                    current->render(&ctx, ctx.fb);
                    guard_draw_result(ctx.hl, update_display_full(ctx.hl));
                }
            } else if ((key == UI_KEY_3 || handle_hit) && !usb_storage_active()) {
                menu_open = !menu_open;
                if (menu_open) {
                    menu_leaf = ui_menu_leaf_for_app(current);
                    present_menu(&ctx, current, menu_leaf, &feedback);
                } else app_present(&ctx, current, APP_REDRAW_PAGE);
            } else if (key >= 0 && menu_open) {
                int next = menu_leaf + (key == UI_KEY_1 ? -1 : 1);
                if (next >= 0 && next < ui_menu_leaf_count()) {
                    menu_leaf = next;
                    present_menu(&ctx, current, menu_leaf, &feedback);
                }
            } else if (key >= 0) {
                app_redraw_t redraw = current->on_key ? current->on_key(&ctx, key) : APP_REDRAW_NONE;
                if (redraw == APP_REDRAW_NONE)
                    ESP_LOGI(TAG, "KEY%d pressed, no action bound", key + 1);
                present_page(&ctx, current, &gesture, redraw);
            } else if (menu_open) {
                int hit = ui_menu_hit_test(touch.x, touch.y, menu_leaf);
                if (hit == UI_MENU_HIT_PREV || hit == UI_MENU_HIT_NEXT) {
                    menu_leaf += hit == UI_MENU_HIT_NEXT ? 1 : -1;
                    present_menu(&ctx, current, menu_leaf, &feedback);
                } else if (hit >= 0 && touch.count == 1) {
                    menu_pressed = hit;
                    menu_feedback(&ctx, current, menu_leaf, hit, true, &feedback);
                }
            } else if (current->on_gesture) {
                ctx.consumed = false;
            } else {
                // 旧页按下先收到 consumed=true，仅 NONE 为同轮 tick 放行。
                // Legacy press sees consumed=true; only NONE clears it for this tick.
                app_redraw_t redraw = current->on_touch
                    ? current->on_touch(&ctx, &latest) : APP_REDRAW_NONE;
                app_present(&ctx, current, redraw);
                if (redraw == APP_REDRAW_NONE) ctx.consumed = false;
            }
        }
        if (err == ESP_OK) was_touched = touch.touched;
        if (!power_input_owned && !quick_input_owned && menu_open && menu_pressed >= 0) {
            int hit = ui_menu_hit_test(latest.x, latest.y, menu_leaf);
            if (touch.count > 1 || hit != menu_pressed || released) {
                int chosen = menu_pressed;
                menu_feedback(&ctx, current, menu_leaf, chosen, false, &feedback);
                menu_pressed = UI_MENU_HIT_NONE;
                if (released && err == ESP_OK && hit == chosen) {
                    menu_open = false;
                    ctx.request_app = app_at(chosen);
                    selected_from_menu = true;
                    if (ctx.request_app == current) app_present(&ctx, current, APP_REDRAW_PAGE);
                }
            }
        }
        dispatch_gesture_frame(&ctx, current, &gesture, err,
                               power_input_owned || quick_input_owned || menu_open);

        ctx.now_ms = esp_timer_get_time() / 1000;
        rails_idle_check(ctx.now_ms);

        // 电源键短按默认锁屏；阅读正文可选择将它用于下一页。
        // A short power press normally locks; the reader body may use it for the next page.
        if (quick_lock_requested || (read_pico_pmu_ready() && !current->holds_pmu && !usb_storage_active()
            && ctx.now_ms - last_lock_poll_ms >= APP_LOCK_POLL_MS)) {
            last_lock_poll_ms = ctx.now_ms;
            read_pico_pmu_key_action_t key_action;
            if (quick_lock_requested) {
                key_action = READ_PICO_PMU_KEY_SHORT;
            } else {
                TD_TIME("pmu_poll", key_action = read_pico_pmu_take_key_action());
            }
            if (key_action != READ_PICO_PMU_KEY_NONE) {
                last_input_ms = ctx.now_ms;
                bool quick_was_open = s_quick.open;
                if (quick_was_open && !quick_lock_requested) {
                    ui_quick_menu_reset(&s_quick);
                    quick_present(&ctx, current, false);
                }
                if (!quick_lock_requested && ctx.now_ms < s_lock_ignore_until_ms) {
                    ESP_LOGI(TAG, "ignore boot power-key action %d", (int)key_action);
                } else if (key_action == READ_PICO_PMU_KEY_LONG) {
                    held_key = -1;
                    cancel_gesture(&ctx, current, &gesture);
                    if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
                    menu_pressed = UI_MENU_HIT_NONE;
                    extern const app_desc_t app_book;
                    bool reader_power_lock = !menu_open && current == &app_book &&
                        app_book_reader_body_visible() && app_settings_reader_power_turn();
                    if (reader_power_lock) {
                        prepare_lock(&ctx, current, true);
                        main_lock_boundary();
                        enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp, true, lock_light_only());
                        display_request_full_refresh();
                        ctx.now_ms = esp_timer_get_time() / 1000;
                        last_input_ms = ctx.now_ms;
                        poll_media(&ctx, current, &media_mounted, &media_invalidated);
                        last_media_poll_ms = ctx.now_ms;
                        ctx.consumed = true;
                        ctx.pressed = false;
                        ctx.released = false;
                        app_present(&ctx, current, APP_REDRAW_PAGE);
                    } else {
                        power_dialog_open = true;
                        ui_power_dialog_reset(&power_dialog, touch.touched);
                        ctx.request_app = NULL;
                        ctx.request_return = ctx.request_menu = false;
                        main_lock_boundary();
                        ui_text_set_system_scale(true);
                        ui_power_dialog_draw(ctx.fb, &power_dialog);
                        enum EpdDrawError result = update_display_area_with(
                            ctx.hl, &E0470_WAVEFORM, MODE_GL16, ui_power_dialog_rect());
                        guard_draw_result(ctx.hl, result);
                        if (result != EPD_DRAW_SUCCESS) {
                            power_dialog_open = false;
                            ui_power_dialog_reset(&power_dialog, touch.touched);
                            app_present(&ctx, current, APP_REDRAW_FULL);
                        }
                    }
                } else if (!power_dialog_open) {
                    held_key = -1;
                    cancel_gesture(&ctx, current, &gesture);
                    if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
                    menu_pressed = UI_MENU_HIT_NONE;
                    app_redraw_t power_redraw = !quick_lock_requested && !quick_was_open && !menu_open && current->on_power_short
                        ? current->on_power_short(&ctx) : APP_REDRAW_NONE;
                    if (power_redraw != APP_REDRAW_NONE) {
                        present_page(&ctx, current, &gesture, power_redraw);
                        ctx.consumed = true;
                    } else {
                        prepare_lock(&ctx, current, true);
                        extern const app_desc_t app_book;
                        main_lock_boundary();
                        enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp,
                                             !menu_open && current == &app_book && app_book_reader_body_visible(), lock_light_only());
                        display_request_full_refresh();
                        ctx.now_ms = esp_timer_get_time() / 1000;
                        last_input_ms = ctx.now_ms;
                        poll_media(&ctx, current, &media_mounted, &media_invalidated);
                        last_media_poll_ms = ctx.now_ms;
                        ctx.consumed = true;
                        ctx.pressed = false;
                        ctx.released = false;
                        // 醒来还在同一页，重画一次免得留着锁屏图。
                        // Still the same page; redraw so the lock image does not stay.
                        if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                        else app_present(&ctx, current, APP_REDRAW_PAGE);
                    }
                }
            }
        }

        // 设置里存的字体在 SD 卡上，开机时卡可能还没挂好，这里定期重试。
        // The saved font lives on the card; retry until the mount is ready.
        // The saved font lives on the card; retry until the mount is ready.
        if (!power_dialog_open && !s_quick.open && !usb_storage_active() && current != &app_transfer &&
            ctx.now_ms - last_font_retry_ms >= FONT_RETRY_INTERVAL_MS) {
            last_font_retry_ms = ctx.now_ms;
            bool font_ready;
            TD_TIME("font_retry", font_ready = try_load_saved_sd_font(!media_invalidated));
            if (font_ready) {
                held_key = -1;
                cancel_gesture(&ctx, current, &gesture);
                ctx.consumed = true;
                menu_pressed = UI_MENU_HIT_NONE;
                if (menu_open) {
                    present_menu(&ctx, current, menu_leaf, &feedback);
                } else {
                    app_present(&ctx, current, APP_REDRAW_FULL);
                }
                ESP_LOGI(TAG, "TTF font ready");
            }
        }

        if (!power_dialog_open && released && held_key == UI_KEY_2 && ui_key_hit_test(latest.x, latest.y) == UI_KEY_2 &&
            current->defer_middle_short && !menu_open &&
            !ctx.request_app && !ctx.request_menu && !ctx.request_return) {
            int key = held_key;
            held_key = -1;
            ctx.consumed = true;
            app_redraw_t redraw = ctx.now_ms - held_since_ms >= UI_LONG_PRESS_MS
                ? current->on_key_long(&ctx, key) : current->on_key(&ctx, key);
            present_page(&ctx, current, &gesture, redraw);
        }
        if (!power_dialog_open && held_key >= 0) {
            if (menu_open || ctx.request_app || ctx.request_menu || ctx.request_return || !touch.touched ||
                touch.count != 1 || ui_key_hit_test(touch.x, touch.y) != held_key ||
                ctx.now_ms < held_since_ms) {
                held_key = -1;
            } else if (!pressed && ctx.now_ms - held_since_ms >= UI_LONG_PRESS_MS) {
                int key = held_key;
                held_key = -1;
                ctx.consumed = true;
                present_page(&ctx, current, &gesture, current->on_key_long(&ctx, key));
            }
        }
        // 蓝牙翻页器：BLE 栈只在这里跟随设置启停。必须走 ble_pt_sync()——自己写
        // 「设置与状态不一致就 start()」会让启动失败的那次判断每轮都成立，变成每秒
        // 几十次重复初始化并把内存耗光。sync() 内部有内存闸门、失败退避和崩溃自锁。
        // Bluetooth page-turner: the BLE stack follows the setting only here, and only through
        // ble_pt_sync(). Rolling our own "state differs from setting, so start()" makes a failed
        // start true on every tick, re-initialising dozens of times a second until memory runs
        // out; sync() carries the memory gate, the failure back-off and the crash-loop lock.
        // WiFi 在 C6、BLE 在 S3；传输仍与 BLE 竞争 S3 内部内存，网络占用先拆 BLE，结束后恢复。
        // WiFi runs on C6 and BLE on S3; both consume S3 internal memory, so network leases pause BLE and release it afterwards.
        read_pico_transfer_status_t transfer;
        read_pico_transfer_get_status(&transfer);
        const bool wifi_busy = transfer.state != READ_PICO_TRANSFER_STOPPED ||
            pico_online_busy();
        ble_pt_sync(app_settings_ble_turner() && !wifi_busy);
        TD_TIME("ble_poll", ble_pt_poll());
        if (s_quick.open || quick_input_owned) {
            // 覆盖层期间丢弃页内蓝牙输入，关闭后不重放翻页或学习事件。
            // Discard page BLE input while covered, without replaying turns or learning edges after closing.
            ble_pt_event_t key_event; ble_pt_raw_t raw_event;
            while (ble_pt_pop_key(&key_event)) {}
            while (ble_pt_pop_raw(&raw_event)) {}
        }
        if (s_quick.open && quick_status() != s_quick_status) quick_present(&ctx, current, false);
        if (!power_dialog_open && !s_quick.open && !quick_input_owned && !ui_click_feedback_active() && !menu_open && !ctx.request_app && !ctx.request_menu && !ctx.request_return && current->on_tick != NULL) {
            app_redraw_t redraw = current->on_tick(&ctx);
            if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) held_key = -1;
            present_page(&ctx, current, &gesture, redraw);
        }
        if (ctx.user_activity) { last_input_ms = ctx.now_ms; ctx.user_activity = false; }
        uint32_t ble_input = ble_pt_input_serial();
        if (ble_input != last_ble_input) { last_ble_input = ble_input; last_input_ms = ctx.now_ms; }
        unsigned requested_idle = app_settings_auto_lock_minutes();
        bool idle_blocked = usb_storage_active() || wifi_busy || current == &app_transfer ||
            current->holds_pmu || power_dialog_open || touch.touched ||
            ctx.request_app || ctx.request_menu || ctx.request_return;
        if (idle_blocked || requested_idle != idle_minutes) last_input_ms = ctx.now_ms;
        idle_minutes = requested_idle;
        if (!idle_blocked && ctx.now_ms >= s_lock_ignore_until_ms &&
            auto_lock_due(ctx.now_ms, last_input_ms, idle_minutes)) {
            held_key = -1;
            cancel_gesture(&ctx, current, &gesture);
            if (menu_pressed >= 0) menu_feedback(&ctx, current, menu_leaf, menu_pressed, false, &feedback);
            menu_pressed = UI_MENU_HIT_NONE;
            prepare_lock(&ctx, current, false);
            extern const app_desc_t app_book;
            main_lock_boundary();
            enter_lock_and_sleep(ctx.hl, &s_lock_ignore_until_ms, ctx.acc, ctx.tp,
                                 !menu_open && current == &app_book && app_book_reader_body_visible(), lock_light_only());
            display_request_full_refresh();
            ctx.now_ms = esp_timer_get_time() / 1000;
            last_input_ms = ctx.now_ms;
            poll_media(&ctx, current, &media_mounted, &media_invalidated);
            last_media_poll_ms = ctx.now_ms;
            ctx.pressed = ctx.released = false;
            just_transitioned = true;
            if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
            else app_present(&ctx, current, APP_REDRAW_PAGE);
        }
        if (menu_open && feedback.updates > 0
            && ctx.now_ms - feedback.last_ms >= UI_SETTLE_IDLE_MS) {
            guard_draw_result(ctx.hl, update_display_area_with(
                ctx.hl, &E0470_WAVEFORM, MODE_GL16, feedback.area));
            feedback.updates = 0;
        }
        if (!menu_open) feedback.updates = 0;
#ifdef KIIKO_LOCAL_LATENCY
        static int64_t probe_heartbeat;
        if(ctx.now_ms-probe_heartbeat>5000){probe_heartbeat=ctx.now_ms;ESP_LOGI("latency","STATE touch=%d menu=%d dialog=%d usb=%d",touch.touched,menu_open,power_dialog_open,usb_storage_active());}
        if(!menu_open&&!power_dialog_open&&!usb_storage_active()&&!touch.touched) {
            app_redraw_t probe=local_latency_poll(&ctx,current);
            if(probe!=APP_REDRAW_NONE)app_present(&ctx,current,probe);
        }
#endif
        // 返回来源优先；普通请求仍按切页优先处理。
        // Return requests win; ordinary requests still prioritize page switches.
        // USB MSC 持有 TF 卡时禁止任何页面切换；必须先安全停止并重新挂载。
        // Never navigate while USB MSC owns the card; stop and remount it first.
        if (usb_storage_active()) {
            ctx.request_return = false;
            ctx.request_app = NULL;
            ctx.request_menu = false;
        }
        if (!power_dialog_open && (ctx.request_return || ctx.request_app || ctx.request_menu)) {
            if (ui_click_feedback_release(ctx.fb, &click_area)) click_present(&ctx, current, click_area);
            ui_quick_menu_reset(&s_quick);
            held_key = -1;
            const app_desc_t* next = ctx.request_app;
            const bool go_back = ctx.request_return;
            cancel_gesture(&ctx, current, &gesture);
            menu_pressed = UI_MENU_HIT_NONE;
            ctx.request_app = NULL;
            ctx.request_menu = false;
            ctx.request_return = false;
            ctx.consumed = true;
            just_transitioned = true;
            if (go_back) {
                bool main_source = main_page_ready(&ctx, current);
                bool shelf_source = main_source && app_index_of(current) == 1;
                display_main_transition_cancel();
                menu_open = return_app ? return_to_menu : true;
                menu_leaf = return_app ? return_menu_leaf : ui_menu_leaf_for_app(current);
                if (return_app) {
                    if (current->on_exit) current->on_exit(&ctx);
                    current = return_app;
                    return_app = NULL;
                    app_font_activate_system();
                    if (current->on_enter) current->on_enter(&ctx);
                    // 初始化后恢复通用页码，避免 on_enter 的默认页码覆盖来源。
                    // Restore the shared leaf after initialization overrides its default.
                    ctx.leaf = return_leaf;
                }
                if (menu_open) present_menu(&ctx, current, menu_leaf, &feedback);
                else {
                    if (main_source && main_page_ready(&ctx, current)) display_main_transition_arm(current, shelf_source, true);
                    app_present(&ctx, current, APP_REDRAW_PAGE);
                    display_main_transition_disarm();
                }
            } else if (next) {
                if (next != current) {
                    return_app = current;
                    return_leaf = ctx.leaf;
                    return_to_menu = selected_from_menu;
                    return_menu_leaf = menu_leaf;
                }
                menu_open = false;
                switch_to(&ctx, &current, next);

            } else {
                menu_open = true;
                menu_leaf = ui_menu_leaf_for_app(current);
                present_menu(&ctx, current, menu_leaf, &feedback);
            }
        }
#if CONFIG_TOUCH_DIAG
        if (ctx.now_ms - td_stats_ms >= 60000) {
            td_stats_ms = ctx.now_ms;
            TD_LOGI("STATS press=%u tap=%u swipe=%u long=%u cancel=%u err=%u kill=%u multi=%u stall=%u maxstall=%dms present>100ms=%u maxpresent=%dms replay_evt=%u replay_frames=%u",
                    td_press, td_tap, td_swipe, td_longp, td_cancel, td_read_err, td_kill_err,
                    td_multi, td_stall, td_max_stall_ms, td_present_over, td_max_present_ms,
                    td_replays, td_replay_frames);
        }
#endif
        // 连续列表相位期间缩短等待；每次仍读触摸，并给其他任务让出一个调度周期。
        // Reduce waits during continuous list phases; poll touch each time and yield one scheduling tick.
        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}
