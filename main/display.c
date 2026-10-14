/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 刷屏出口：按模式推屏、HV 轨空闲下电、pclk 欠载回退。
 *
 * Present path: push by mode, drop HV rails on idle, fall back pclk on
 * underrun.
 * 用户继续优化快刷响应：无灰阶条带时按32位并行归一八像素，已为黑白的字不重复写PSRAM；不删相位、不增加缓存或改变清残影和底栏边界。
 * User continues fast responsiveness tuning: normalize eight pixels per 32-bit word when no gray bands exist and avoid rewriting binary PSRAM words; retain phases, cleanup and navigation bounds without extra caches.
 * 用户修订：三种主页刷新模式均固定底栏，仅局部移动横条；普通保留原灰阶及内容区清理周期。快刷棋盘格亚克力与封面合并黑白输出。首次进入、解锁及手动全刷仍重建完整底栏，不得伪造back。
 * User revision: all main-screen modes retain navigation and move only its marker locally; ordinary retains original grays and body cleanup cadence. Fast checkerboard acrylic and covers share one BW target. Initial entry, unlock and explicit full refresh rebuild complete navigation; never fabricate back.
 * 用户修订：所有推屏及欠载恢复与 OTA 擦写共用递归锁；无升级任务时保持原波形、范围和节奏。
 * User revision: serialize all display output and underrun recovery with OTA flash operations; keep existing waveforms, bounds and cadence outside updates.
 * 用户要求加快主页水波纹：复用阅读的一拍一带调度，以8带37相共44拍、11090us完成；保留主页原灰阶序列、底栏边界、OTA锁及失败恢复。
 * User requests faster main-page ripple: reuse reader-style compact launches for eight bands/37 phases over 44 ticks at 11090us; retain main gray sequences, navigation bounds, OTA locking and recovery.
 * 用户修订：书架顶部文字、框线与封面/亚克力一起使用水波纹，不再拆分顶部差分刷新；底栏仍固定。
 * User revision: shelf headers, frames, covers and acrylic share one ripple instead of a separate header differential; navigation remains fixed.
 * 用户修订：设置滚动复用已有连续相位状态；离页、唤醒或扫描错误先建立完整参考帧，避免半程状态外泄。
 * User revision: settings scrolling reuses continuous phase state; page exit, wake and scan faults establish a complete baseline before other outputs.
 */

#include "display.h"
#include "ota_online.h"
#include "continuous_du.h"

#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"
#include "e0470_epaper_waveform.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "settings.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "display_main_waveform.h"
#include "ui/ui_nav_layout.h"
#include "ui/ui_follow_pixels.h"

static const char* TAG = "read_pico";
static bool s_bulk_io;
static bool s_follow_active, s_full_refresh_requested;
void display_request_full_refresh(void) { s_full_refresh_requested = true; }
bool display_follow_busy(void) { return s_follow_active && continuous_du_busy(); }
static int s_pclk_mhz = DISPLAY_PCLK_DEFAULT_MHZ;

void display_set_bulk_io(bool active) {
    s_bulk_io = active;
    ESP_LOGI(TAG, "bulk I/O scan margin %s", active ? "on" : "off");
}

// HV 轨道空闲多久才断电。断电要等 500ms 放电，再上电又要几十毫秒，
// 所以连续操作期间一直保持常开，只有真的没人动才关掉省电并卸掉 VCOM。
// How long HV rails stay up when idle. Power-off waits 500 ms to discharge,
// and power-on takes tens of ms, so keep them on during a burst of work and
// drop them — and VCOM — only when nothing is happening.
#define RAILS_IDLE_TIMEOUT_MS 8000

// 0 表示轨道已断电；否则是到期时间（ms），到点后主循环断电。
// 0 means the rails are off; otherwise a deadline (ms) after which the loop powers them down.
static int64_t rails_deadline_ms;

void rails_keepalive(void) {
    rails_deadline_ms = esp_timer_get_time() / 1000 + RAILS_IDLE_TIMEOUT_MS;
}

void rails_idle_check(int64_t now_ms) {
    if (rails_deadline_ms != 0 && now_ms >= rails_deadline_ms) {
        rails_deadline_ms = 0;
        epd_poweroff();
    }
}

// 20 相完整/原厂 DU 按厂家时序走 FULL；只有触摸笔迹用的 8 帧跟随 DU 走 FAST。
// 20-phase full / vendor DU uses FULL timing; the 8-frame FOLLOW DU used for ink trails uses FAST.
static void use_scan_for(const EpdWaveform* waveform, enum EpdDrawMode mode) {
    const bool fast = (mode & 0xF) == MODE_DU && waveform == &E0470_FOLLOW_WAVEFORM;
    read_pico_epd_use_scan(fast ? READ_PICO_EPD_SCAN_FAST : READ_PICO_EPD_SCAN_FULL);
    // 高层刷新保持整屏扫描；两条63行可用队列在第127行入队前启动。
    // High-level updates scan the full panel; two 63-slot queues start before line 127 is enqueued.
    epd_lcd_set_prefill_lines(s_bulk_io ? 127 : (fast ? 64 : 32));
}

// 自上次整屏 GC16 以来的整页差分次数。局部反馈不计数，否则菜单按压几次就会触发黑白全刷。
// Whole-page differentials since the last full GC16. Local feedback is excluded so a few taps cannot trigger flashing.
static int s_page_refreshes;

static const void* s_main_armed;
static bool s_main_fault;
static bool s_main_shelf_page;
static EpdRect s_main_bands[3];
static unsigned s_main_band_count;
static bool s_main_shelf_exit;
static bool s_main_changing_page;
_Static_assert((UI_NAV_REFRESH_END & 31) == 0, "fixed navigation begins on an aligned framebuffer column");
_Static_assert(UI_NAV_REFRESH_END <= UI_NAV_TOP, "aligned refresh must stop above navigation and marker");
_Static_assert(UI_NAV_MARKER_SCAN_END <= UI_NAV_TOP + 27, "marker crop must stop before navigation icons");
_Static_assert((UI_NAV_MARKER_TOP & 1) && !(UI_NAV_MARKER_BOTTOM & 1), "marker nibble guards match current geometry");

static bool main_nav_geometry(void) {
    // 当前板为倒置竖屏：逻辑 y 对应物理 x。其他屏型保持原刷新路径。
    // On this inverted-portrait board logical y maps to physical x; other layouts keep the existing presenter.
    return epd_get_rotation() == EPD_ROT_INVERTED_PORTRAIT &&
           epd_width() == 1216 && epd_height() == 684;
}

static EpdRect main_body_area(void) {
    // 内容截止到 y=1087，32列对齐也不能扩展到 y=1097 的小横条。
    // Content ends at y=1087, so 32-column expansion cannot reach the marker at y=1097.
    return (EpdRect){0, 0, epd_rotated_display_width(), UI_NAV_REFRESH_END};
}

static uint8_t main_bw_byte(uint8_t value) {
    return (value & 0x0f) >= 8 ? ((value & 0xf0) >= 0x80 ? 0xff : 0x0f)
                             : ((value & 0xf0) >= 0x80 ? 0xf0 : 0x00);
}

static bool main_gray_column(int x) {
    for (unsigned i = 0; i < s_main_band_count; ++i)
        if (x >= s_main_bands[i].y && x < s_main_bands[i].y + s_main_bands[i].height) return true;
    return false;
}

static void main_normalize_body(EpdiyHighlevelState* hl) {
    const size_t stride = (size_t)epd_width() / 2;
    for (int y = 0; y < epd_height(); ++y) {
        uint8_t* row = hl->front_fb + y * stride;
        if (!s_main_band_count) {
            for (int x = 0; x < UI_NAV_REFRESH_END / 2; x += 4) {
                uint32_t value;
                memcpy(&value, row + x, sizeof(value));
                const uint32_t binary = ((value & UINT32_C(0x88888888)) >> 3) * 15u;
                if (value != binary) memcpy(row + x, &binary, sizeof(binary));
            }
            continue;
        }
        for (int x = 0; x < UI_NAV_REFRESH_END / 2; ++x)
            if (!main_gray_column(x * 2)) row[x] = main_bw_byte(row[x]);
    }
}

static bool main_body_changed(EpdiyHighlevelState* hl) {
    const size_t stride = (size_t)epd_width() / 2;
    for (int y = 0; y < epd_height(); ++y)
        if (memcmp(hl->front_fb + y * stride, hl->back_fb + y * stride, UI_NAV_REFRESH_END / 2)) return true;
    return false;
}

// 三条最终灰阶最多约90KiB；首帧跳过它们，实际输出成功才提交back。
// Three final gray bands cost at most about 90 KiB; skip them in the first pass and commit back only after successful output.
static size_t main_band_bytes(void) {
    size_t bytes = 0;
    for (unsigned i = 0; i < s_main_band_count; ++i) bytes += (size_t)s_main_bands[i].height * epd_height() / 2;
    return bytes;
}
static void main_band_copy(EpdiyHighlevelState *hl, uint8_t *cache, bool restore) {
    const size_t stride = (size_t)epd_width() / 2;
    for (unsigned i = 0; i < s_main_band_count; ++i) {
        const size_t bytes = (size_t)s_main_bands[i].height / 2;
        for (int y = 0; y < epd_height(); ++y) {
            uint8_t *row = hl->front_fb + y * stride + s_main_bands[i].y / 2;
            if (restore) memcpy(row, cache, bytes);
            else { memcpy(cache, row, bytes); memcpy(row, hl->back_fb + y * stride + s_main_bands[i].y / 2, bytes); }
            cache += bytes;
        }
    }
}

static void main_freeze_navigation(EpdiyHighlevelState* hl, bool update_marker) {
    const size_t stride = (size_t)epd_width() / 2;
    for (int y = 0; y < epd_height(); ++y) {
        uint8_t* front = hl->front_fb + y * stride;
        const uint8_t* back = hl->back_fb + y * stride;
        if (!update_marker) {
            memcpy(front + UI_NAV_REFRESH_END / 2, back + UI_NAV_REFRESH_END / 2,
                   stride - UI_NAV_REFRESH_END / 2);
            continue;
        }
        // 只保留目标横条的五行；起点为奇数像素，同行字节的分隔线半字节仍取真实 back。
        // Keep only the five target marker rows; at the odd first pixel retain the neighboring divider nibble from actual back.
        memcpy(front + UI_NAV_REFRESH_END / 2, back + UI_NAV_REFRESH_END / 2,
               (UI_NAV_MARKER_TOP - UI_NAV_REFRESH_END) / 2);
        const int first = UI_NAV_MARKER_TOP / 2;
        front[first] = (front[first] & 0xf0) | (back[first] & 0x0f);
        memcpy(front + UI_NAV_MARKER_BOTTOM / 2, back + UI_NAV_MARKER_BOTTOM / 2,
               stride - UI_NAV_MARKER_BOTTOM / 2);
    }
}

void display_main_transition_disarm(void) {
    s_main_armed = NULL;
    s_main_shelf_page = false;
    s_main_band_count = 0;
    s_main_shelf_exit = false;
    s_main_changing_page = false;
}

void display_main_transition_cancel(void) {
    display_main_transition_disarm();
}

void display_main_transition_arm(const void* owner, bool leaving_shelf, bool changing_page) {
    s_main_armed = owner;
    s_main_shelf_page = false;
    s_main_band_count = 0;
    s_main_shelf_exit = s_main_armed && leaving_shelf;
    s_main_changing_page = s_main_armed && changing_page;
}

void display_main_transition_gray_bands(const EpdRect *bands, unsigned count) {
    if (!s_main_armed) return;
    s_main_band_count = 0;
    if (!bands || count > 3) return;
    int last = 0;
    for (unsigned i = 0; i < count; ++i) {
        EpdRect b = bands[i];
        if (b.x || b.width != epd_rotated_display_width() || b.y < last || b.height <= 0 ||
            (b.y & 1) || (b.height & 1) || b.y > UI_NAV_REFRESH_END || b.height > UI_NAV_REFRESH_END - b.y) return;
        s_main_bands[i] = b; last = b.y + b.height;
    }
    s_main_band_count = count;
}

void display_main_transition_shelf_page(void) {
    s_main_shelf_page = s_main_armed != NULL;
}

static enum EpdDrawError main_recover(EpdiyHighlevelState* hl, enum EpdDrawError error) {
    // 扫描失败后光学状态未知，必须从白底重建，不能复用半途图的 back。
    // A failed scan leaves unknown optical state; rebuild from white instead of reusing an intermediate back.
    display_main_transition_cancel();
    if (error & EPD_DRAW_EMPTY_LINE_QUEUE) {
        s_pclk_mhz = DISPLAY_PCLK_SAFE_MHZ;
        read_pico_epd_set_pclk(DISPLAY_PCLK_SAFE_MHZ);
    }
    use_scan_for(&E0470_WAVEFORM, MODE_GC16);
    epd_poweron();
    epd_clear();
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    enum EpdDrawError result = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);
    s_main_fault = result != EPD_DRAW_SUCCESS;
    s_page_refreshes = 0;
    rails_keepalive();
    ESP_LOGW(TAG, "main transition recovery error=%d result=%d", error, result);
    return result;
}

static bool main_begin(EpdiyHighlevelState* hl, enum EpdDrawError* result) {
    const void* owner = s_main_armed;
    const bool shelf_exit = s_main_shelf_exit || s_main_shelf_page;
    s_main_shelf_page = false;
    const bool changing_page = s_main_changing_page;
    s_main_changing_page = false;
    const int refresh_mode = app_settings_main_refresh_mode();
    const bool water = refresh_mode == APP_MAIN_REFRESH_WATER;
    const bool normal = refresh_mode == APP_MAIN_REFRESH_NORMAL;
    const bool fast = refresh_mode == APP_MAIN_REFRESH_FAST;
    s_main_shelf_exit = false;
    s_main_armed = NULL;
    if (!owner || !hl || !hl->front_fb || !hl->back_fb) return false;
    if (s_main_fault) { *result = main_recover(hl, EPD_DRAW_SUCCESS); return true; }
    if (!main_nav_geometry()) return false;
    // 8帧整页方案被实机否定；用户接受主页面黑白、亚克力灰阶，黑白改用完整20相。
    // Hardware rejected whole-page 8-frame output; the user accepted black/white main pages with gray acrylic. Use the full 20-phase black/white table.
    main_freeze_navigation(hl, true);
    if (fast) main_normalize_body(hl);
    const bool changed = main_body_changed(hl);
    uint8_t *cache = fast && changed && s_main_band_count
        ? heap_caps_aligned_alloc(16, main_band_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    const bool gray_fallback = water || (fast && changed && s_main_band_count && !cache);
    if (cache) main_band_copy(hl, cache, false);
    const EpdWaveform* body_wave = normal ? &E0470_WAVEFORM : gray_fallback ? display_main_gray_waveform()
        : shelf_exit ? display_main_shelf_exit_waveform() : display_main_bw_waveform();
    enum EpdDrawMode body_mode = normal || gray_fallback ? MODE_GL16 : MODE_DU;
    bool body_full = false;
    // 普通模式保留第三次灰阶整理、十二次清残影，只驱动内容区，底栏不参与计数刷新。
    // Ordinary retains third-update gray settling and twelfth-update cleanup in the body only, excluding navigation.
    if (normal && changed) {
        ++s_page_refreshes;
        if (APP_UI_FAST_GC16_EVERY > 0 && s_page_refreshes >= APP_UI_FAST_GC16_EVERY) {
            s_page_refreshes = 0;
            body_mode = MODE_GC16;
            body_full = true;
        } else body_full = APP_UI_FAST_GL16_SETTLE_EVERY > 0 &&
                            s_page_refreshes % APP_UI_FAST_GL16_SETTLE_EVERY == 0;
    }
    use_scan_for(body_wave, body_mode);
    epd_poweron();
    epd_hl_waveform(hl, body_wave);
    int64_t start = esp_timer_get_time();
    // 快刷/水波纹不攒强刷次数；普通的周期整理也不能进入底栏。
    // Fast/water accrue no forced-cleanup debt; ordinary periodic settling also excludes navigation.
    if (water && changing_page && changed) {
        // 水波纹直接输出最终灰阶图，不先刷黑白、不追加整页灰阶；失败由统一恢复处理。
        // Water outputs the final gray target directly, without a BW prepass or a second whole-page gray update; common recovery handles failure.
        read_pico_epd_use_scan(READ_PICO_EPD_SCAN_FAST);
        epd_lcd_set_prefill_lines(s_bulk_io ? 127 : 64);
        EpdRect ripple = main_body_area();
        const int prior_tick=e0470_page_turn_tick_us();
        e0470_page_turn_set_tick_us(E0470_TURN_FAST_TICK_US);
        *result = e0470_page_turn_with_waveform_compact(hl, ripple, E0470_TURN_RTL,
                                                       display_main_water_waveform(), E0470_TURN_FAST_BANDS);
        e0470_page_turn_set_tick_us(prior_tick);
        if (*result == EPD_DRAW_NO_PHASES_AVAILABLE) {
            use_scan_for(body_wave, MODE_GL16);
            epd_hl_waveform(hl, body_wave);
            *result = epd_hl_update_area(hl, MODE_GL16, 25, ripple);
        }
        // 主页面动画不长期保留相位缓存，避免与联网和阅读资源争用。
        // Release main-animation LUTs instead of competing with networking and reader resources.
        e0470_page_turn_release();
    } else if (!changed) *result = EPD_DRAW_SUCCESS;
    else *result = body_full ? epd_hl_update_area_full(hl, body_mode, 25, main_body_area())
                            : epd_hl_update_area(hl, body_mode, 25, main_body_area());
    if (cache) {
        main_band_copy(hl, cache, true);
        heap_caps_free(cache);
        if (*result == EPD_DRAW_SUCCESS) {
            const EpdRect last = s_main_bands[s_main_band_count - 1];
            EpdRect gray = {0, s_main_bands[0].y, epd_rotated_display_width(), last.y + last.height - s_main_bands[0].y};
            // 采用原校准迁移，不自行按灰度比例截帧；等灰无需反复推动。
            // Retain calibrated transitions without scaling pulses by gray distance; equal grays need no repeated drive.
            epd_hl_waveform(hl, display_main_gray_waveform());
            use_scan_for(display_main_gray_waveform(), MODE_GL16);
            *result = epd_hl_update_area(hl, MODE_GL16, 25, gray);
        }
    }
    s_main_band_count = 0;
    if (*result == EPD_DRAW_SUCCESS) {
        const EpdRect marker = {0, UI_NAV_MARKER_TOP, epd_rotated_display_width(),
                               UI_NAV_MARKER_BOTTOM - UI_NAV_MARKER_TOP};
        // 同页重绘横条无变化时 highlevel 自动跳过；32列扩展范围中其余像素来自 back，保持不驱动。
        // Highlevel skips an unchanged marker; all other pixels in its expanded 32-column crop come from back and remain undriven.
        epd_hl_waveform(hl, &E0470_FOLLOW_WAVEFORM);
        use_scan_for(&E0470_FOLLOW_WAVEFORM, MODE_DU);
        *result = epd_hl_update_area(hl, MODE_DU, 25, marker);
    }
    if (*result != EPD_DRAW_SUCCESS) *result = main_recover(hl, *result);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    use_scan_for(&E0470_WAVEFORM, MODE_GL16);
    rails_keepalive();
    ESP_LOGI(TAG, "main refresh mode=%d shelf_exit=%d draw=%lldms result=%d", app_settings_main_refresh_mode(), shelf_exit, (esp_timer_get_time() - start) / 1000, *result);
    (void)start;
    return true;
}

// 其他显示出口只取消未用入口；主页面失败时先重建完整目标，绝不回放旧缓存。
// Other outputs disarm unused entry permission; a main-page failure first rebuilds the full current target, never replaying old cache.
static bool main_before_update(EpdiyHighlevelState* hl, enum EpdDrawError* result) {
    display_main_transition_cancel();
    if (s_main_fault) { *result = main_recover(hl, EPD_DRAW_SUCCESS); return true; }
    return false;
}

// 所有按 fb 刷屏的出口都经这里。GL16 必须全像素（白底补 1 帧靠它打到）。
// 整页差分刷攒够 APP_UI_GC16_EVERY 次就把这一次升为全像素 GC16：区域、fb 都不变，只换模式，
// 屏上内容仍由 fb 决定，不会丢；全像素是为了让未变化像素也过一遍 LUT，否则压不掉灰底。
// 跟随 DU 波形只有 DU 一张表，不计数也不升级。
// Every fb present goes through here. GL16 must be full-pixel (the extra white
// frame depends on that). After APP_UI_GC16_EVERY whole-page updates, this one is
// promoted to full-pixel GC16: area and fb stay the same, only the mode
// changes, so content is not lost. Full-pixel is so unchanged pixels also run
// the LUT; otherwise the gray floor will not clear. FOLLOW DU has only a DU
// table and does not count or promote.
static enum EpdDrawError hl_update(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode, bool full,
    const EpdRect* area, bool allow_gl16_diff
) {
    // 已显示主页面的局部封面或滚动更新同样向内裁剪，防止32列对齐带出小横条。
    // Local cover or scroll updates on an already displayed main page use the same inward crop, excluding the marker after alignment.
    if (area && s_main_shelf_page && app_settings_main_fast_refresh()) {
        enum EpdDrawError result;
        if (main_begin(hl, &result)) return result;
    }
    EpdRect clipped;
    const bool protected_area = area && s_main_armed && !s_main_fault && main_nav_geometry();
    if (protected_area) {
        main_freeze_navigation(hl, false);
        if (app_settings_main_fast_refresh()) main_normalize_body(hl);
        clipped = *area;
        if ((int64_t)clipped.y + clipped.height > UI_NAV_REFRESH_END)
            clipped.height = UI_NAV_REFRESH_END - clipped.y;
        if (clipped.height <= 0) { display_main_transition_disarm(); return EPD_DRAW_SUCCESS; }
        area = &clipped;
        // 显式短局刷不被主页模式改写，猫眼/秒针在普通与水波纹中采用同一动效。
        // Preserve explicit short partial updates so cat eyes and seconds share ordinary-mode motion in ripple mode.
        if (app_settings_main_refresh_mode() != APP_MAIN_REFRESH_NORMAL && !full &&
            waveform != &E0470_FOLLOW_WAVEFORM && (mode & 0x0f) != MODE_GC16) {
            bool has_gray = !app_settings_main_fast_refresh();
            for (unsigned i = 0; i < s_main_band_count; ++i)
                has_gray |= clipped.y < s_main_bands[i].y + s_main_bands[i].height && clipped.y + clipped.height > s_main_bands[i].y;
            waveform = has_gray ? display_main_gray_waveform() : s_main_shelf_page
                ? display_main_shelf_exit_waveform() : display_main_bw_waveform();
            mode = has_gray ? MODE_GL16 : MODE_DU;
            allow_gl16_diff = true;
            epd_hl_waveform(hl, waveform);
        }
    }
    enum EpdDrawError intercepted;
    if (main_before_update(hl, &intercepted)) return intercepted;
    if (waveform != &E0470_FOLLOW_WAVEFORM && area == NULL) {
        if ((mode & 0xF) == MODE_GC16) {
            s_page_refreshes = 0;
        } else if (APP_UI_GC16_EVERY > 0 && ++s_page_refreshes >= APP_UI_GC16_EVERY) {
            s_page_refreshes = 0;
            mode = (enum EpdDrawMode)((mode & ~0xF) | MODE_GC16);
            full = true;
            ESP_LOGI(TAG, "promote to GC16 after %d page transitions", APP_UI_GC16_EVERY);
        }
    }
    full = full || ((mode & 0xF) == MODE_GL16 && !allow_gl16_diff);
    // 比较完成后只切换一次扫描档，避免快刷前先切 FULL 再切 FAST。
    // Select scan timing once after comparison, without a FULL-to-FAST round trip before fast updates.
    use_scan_for(waveform, mode);
    enum EpdDrawError result;
    if (area != NULL) {
        result = full ? epd_hl_update_area_full(hl, mode, 25, *area)
                    : epd_hl_update_area(hl, mode, 25, *area);
    } else result = full ? epd_hl_update_screen_full(hl, mode, 25) : epd_hl_update_screen(hl, mode, 25);

    return protected_area && result != EPD_DRAW_SUCCESS ? main_recover(hl, result) : result;
}

static enum EpdDrawError update_display_mode_unlocked(
    EpdiyHighlevelState* hl, enum EpdDrawMode mode
) {
    epd_poweron();
    enum EpdDrawError result = hl_update(hl, &E0470_WAVEFORM, mode, false, NULL, false);
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_mode_diff_unlocked(
    EpdiyHighlevelState* hl, enum EpdDrawMode mode
) {
    epd_poweron();
    enum EpdDrawError result = hl_update(hl, &E0470_WAVEFORM, mode, false, NULL, true);
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_fast_page_unlocked(EpdiyHighlevelState* hl) {
    enum EpdDrawError navigation;
    if (main_begin(hl, &navigation)) return navigation;
    enum EpdDrawError intercepted;
    if (main_before_update(hl, &intercepted)) return intercepted;
    if (APP_UI_FAST_GC16_EVERY > 0 && ++s_page_refreshes >= APP_UI_FAST_GC16_EVERY) {
        s_page_refreshes = 0;
        use_scan_for(&E0470_WAVEFORM, MODE_GC16);
        epd_poweron();
        enum EpdDrawError result = epd_hl_update_screen_full(hl, MODE_GC16, 25);

        rails_keepalive();
        ESP_LOGI(TAG, "fast navigation cleanup after %d transitions", APP_UI_FAST_GC16_EVERY);
        return result;
    }
    use_scan_for(&E0470_WAVEFORM, MODE_GL16);
    epd_poweron();
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    // 间隔性驱动未变化区域，减少跨页白底与细线累积残影，不插入第二次刷新。
    // Periodically drive unchanged areas to reduce old-page ghosts without a second repaint.
    const bool settle = APP_UI_FAST_GL16_SETTLE_EVERY > 0 &&
                        s_page_refreshes % APP_UI_FAST_GL16_SETTLE_EVERY == 0;
    enum EpdDrawError result = settle ? epd_hl_update_screen_full(hl, MODE_GL16, 25)
                                      : epd_hl_update_screen(hl, MODE_GL16, 25);

    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_from_white_with_unlocked(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
) {
    enum EpdDrawError intercepted;
    if (main_before_update(hl, &intercepted)) return intercepted;
    use_scan_for(waveform, mode);
    epd_poweron();
    epd_hl_waveform(hl, waveform);
    enum EpdDrawError result = epd_hl_update_screen_from_white(hl, mode, 25);

    epd_hl_waveform(hl, &E0470_WAVEFORM);
    s_page_refreshes = 0;
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_from_white_unlocked(EpdiyHighlevelState* hl) {
    return update_display_from_white_with(hl, &E0470_WAVEFORM, MODE_GC16);
}

static enum EpdDrawError update_display_white_unlocked(EpdiyHighlevelState* hl) {
    epd_hl_set_all_white(hl);
    return update_display_full(hl);
}

static bool s_white_exit;

void display_hold_white_exit(bool hold) {
    s_white_exit = hold;
}

bool display_take_white_exit(void) {
    const bool hold = s_white_exit;
    s_white_exit = false;
    return hold;
}

static enum EpdDrawError update_display_full_unlocked(EpdiyHighlevelState* hl) {
    if (s_follow_active) continuous_du_reset();
    s_follow_active = s_full_refresh_requested = false;
    display_main_transition_cancel();
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    epd_poweron();
    enum EpdDrawError result = hl_update(hl, &E0470_WAVEFORM, MODE_GC16, true, NULL, false);
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_area_diff_with_unlocked(EpdiyHighlevelState*, const EpdWaveform*, enum EpdDrawMode, EpdRect);
static enum EpdDrawError update_display_area_full_with_unlocked(EpdiyHighlevelState*, const EpdWaveform*, enum EpdDrawMode, EpdRect);

enum EpdDrawError display_follow_area(EpdiyHighlevelState* hl, EpdRect area, bool changed, bool continuous) {
    bool held = pico_online_display_begin();
    display_main_transition_cancel();
    enum EpdDrawError result = EPD_DRAW_SUCCESS;
    if (main_nav_geometry() && area.x == 0 && area.width == 684 &&
        area.y >= 0 && area.y + area.height <= 1216 && !(area.y & 1) && !(area.height & 1)) {
        if (!continuous) {
            ui_follow_pixels(hl->front_fb, hl->back_fb, epd_width(), epd_height(), area.y, area.y + area.height);
            result = s_full_refresh_requested ? update_display_full_unlocked(hl)
                : update_display_area_diff_with_unlocked(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
            pico_online_display_end(held);
            return result;
        }
        if (!s_follow_active) continuous_du_reset();
        s_follow_active = true;
        EpdRect physical = continuous_du_rect_from_logical(area);
        if (changed && ui_follow_pixels(hl->front_fb, hl->back_fb, epd_width(), epd_height(),
                                       area.y, area.y + area.height)) {
            continuous_du_mark_diff(hl->front_fb, hl->back_fb, physical,
                                    continuous_du_dark_phases(), false);
            // back暂记最后目标，物理剩余相位由连续状态负责；常规出口会先重新定稿。
            // Back holds the last target, while continuous state owns physical phases; ordinary exits settle first.
            ui_follow_pixels_remember(hl->front_fb, hl->back_fb, epd_width(), epd_height(),
                                      area.y, area.y + area.height);
        }
        if (continuous_du_busy()) {
            epd_poweron(); rails_keepalive();
            result = continuous_du_scan(hl, physical);
            if (result != EPD_DRAW_SUCCESS) {
                continuous_du_reset(); s_follow_active=false;
                s_full_refresh_requested=true;
            }
        }
    } else result = update_display_area_diff_with_unlocked(hl, &E0470_FOLLOW_WAVEFORM, MODE_DU, area);
    pico_online_display_end(held);
    return result;
}
enum EpdDrawError display_follow_settle(EpdiyHighlevelState* hl, EpdRect area, bool restore_gray) {
    bool held = pico_online_display_begin();
    // 调用方先排完黑白相位；快刷完成后参考帧已是实际黑白目标。
    // The caller drains binary phases first; the fast-mode reference already holds that completed target.
    if (display_follow_busy()) {
        pico_online_display_end(held);
        return EPD_DRAW_SUCCESS;
    }
    if (s_follow_active) continuous_du_reset();
    s_follow_active = false;
    display_main_transition_cancel();
    enum EpdDrawError result = s_full_refresh_requested ? update_display_full_unlocked(hl)
        : restore_gray ? update_display_area_diff_with_unlocked(hl, &E0470_WAVEFORM, MODE_GL16, area)
        : EPD_DRAW_SUCCESS;
    pico_online_display_end(held);
    return result;
}

static enum EpdDrawError update_display_image_gray_unlocked(EpdiyHighlevelState* hl) {
    enum EpdDrawError intercepted;
    if (main_before_update(hl, &intercepted)) return intercepted;
    // 图片保留十六级原始灰阶；白底和完整 48 相波形稳定呈现层次。
    // Images keep sixteen source grays; a white baseline and the full 48-phase table preserve tones.
    use_scan_for(&E0470_FULL_WAVEFORM, MODE_GC16);
    epd_poweron();
    epd_clear();
    epd_hl_waveform(hl, &E0470_FULL_WAVEFORM);
    enum EpdDrawError result = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);

    epd_hl_waveform(hl, &E0470_WAVEFORM);
    s_page_refreshes = 0;
    rails_keepalive();
    return result;
}

// 指定波形整屏刷一次，刷完把默认波形装回去。用来 A/B 两条灰阶表。
// Present the whole screen with a given waveform, then restore the default. Used to A/B two gray tables.
static enum EpdDrawError update_display_with_unlocked(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
) {
    epd_poweron();
    epd_hl_waveform(hl, waveform);
    enum EpdDrawError result = hl_update(hl, waveform, mode, false, NULL, false);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_area_with_unlocked(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
) {
    epd_poweron();
    epd_hl_waveform(hl, waveform);
    enum EpdDrawError result = hl_update(hl, waveform, mode, false, &area, false);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_area_diff_with_unlocked(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
) {
    epd_poweron();
    epd_hl_waveform(hl, waveform);
    enum EpdDrawError result = hl_update(hl, waveform, mode, false, &area, true);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_area_full_with_unlocked(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
) {
    epd_poweron();
    epd_hl_waveform(hl, waveform);
    enum EpdDrawError result = hl_update(hl, waveform, mode, true, &area, false);
    epd_hl_waveform(hl, &E0470_WAVEFORM);
    rails_keepalive();
    return result;
}

static enum EpdDrawError update_display_water_turn_unlocked(EpdiyHighlevelState* hl, EpdRect area,
                                             e0470_turn_dir_t dir) {
    enum EpdDrawError intercepted;
    if (main_before_update(hl, &intercepted)) return intercepted;
    // 阅读页显式选择时调用；主页面水波纹有独立裁剪路径，普通模式不受影响。
    // Explicit reader effect; main-tab water has its own bounded path, and ordinary mode is unaffected.
    read_pico_epd_use_scan(READ_PICO_EPD_SCAN_FAST);
    epd_lcd_set_prefill_lines(s_bulk_io ? 127 : 64);
    epd_poweron();
    enum EpdDrawError result = e0470_page_turn(hl, area, dir);
    use_scan_for(&E0470_WAVEFORM, MODE_GL16);
    rails_keepalive();
    if (result == EPD_DRAW_SUCCESS) return result;
    if (result == EPD_DRAW_NO_PHASES_AVAILABLE) {
        ESP_LOGW(TAG, "water turn phases unavailable; use regular GL16");
        return update_display_area_with(hl, &E0470_WAVEFORM, MODE_GL16, area);
    }
    // 中断可能把物理屏留在半途。清屏后按目标 front 重建整屏基准。
    // An interrupted scan may leave intermediate pixels. Rebuild the whole baseline from white.
    if (result & EPD_DRAW_EMPTY_LINE_QUEUE) {
        s_pclk_mhz = DISPLAY_PCLK_SAFE_MHZ;
        read_pico_epd_set_pclk(DISPLAY_PCLK_SAFE_MHZ);
    }
    use_scan_for(&E0470_WAVEFORM, MODE_GC16);
    epd_clear();
    enum EpdDrawError recovered = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);

    s_page_refreshes = 0;
    rails_keepalive();
    ESP_LOGW(TAG, "water turn failed (%d), recovery=%d", result, recovered);
    return recovered;
}

// 供数不足时的兜底：把频率退回安全值，整屏白一次，让后面的差分刷有干净参考帧。
// Underrun fallback: drop to the safe clock and wipe the panel white so later differentials have a clean reference.
int display_pclk_mhz(void) { return s_pclk_mhz; }

static void guard_draw_result_unlocked(EpdiyHighlevelState* hl, enum EpdDrawError result) {
    if (!(result & EPD_DRAW_EMPTY_LINE_QUEUE)) return;
    display_main_transition_cancel();
    s_pclk_mhz = DISPLAY_PCLK_SAFE_MHZ;
    read_pico_epd_set_pclk(DISPLAY_PCLK_SAFE_MHZ);
    use_scan_for(&E0470_WAVEFORM, MODE_GC16);
    epd_poweron();
    epd_clear();
    // 清物理屏后仅重置旧帧基准，保留目标页；否则局部刷新会留下整页白屏。
    // Reset only the old-frame baseline after clearing; preserving the target prevents blank pages after partial updates.
    enum EpdDrawError recovered = epd_hl_update_screen_from_white(hl, MODE_GC16, 25);

    s_main_fault = recovered != EPD_DRAW_SUCCESS;

    if (recovered != EPD_DRAW_SUCCESS) ESP_LOGW(TAG, "underrun recovery failed (%d)", recovered);
    s_page_refreshes = 0;
    rails_keepalive();
    ESP_LOGW(TAG, "line queue underrun, pclk back to %d MHz, recovery=%d", DISPLAY_PCLK_SAFE_MHZ, recovered);
}

// 统一硬件出口，递归调用也必须覆盖整个扫描过程。/ Guard complete scans, including recursive fallback paths.
enum EpdDrawError update_display_mode(
    EpdiyHighlevelState* hl, enum EpdDrawMode mode
) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_mode_unlocked(hl, mode);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_mode_diff(
    EpdiyHighlevelState* hl, enum EpdDrawMode mode
) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_mode_diff_unlocked(hl, mode);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_fast_page(EpdiyHighlevelState* hl) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_fast_page_unlocked(hl);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_from_white_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_from_white_with_unlocked(hl, waveform, mode);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_from_white(EpdiyHighlevelState* hl) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_from_white_unlocked(hl);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_white(EpdiyHighlevelState* hl) {
    bool held = pico_online_display_begin();
    if (s_follow_active) continuous_du_reset();
    s_follow_active = s_full_refresh_requested = false;
    enum EpdDrawError result = update_display_white_unlocked(hl);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_full(EpdiyHighlevelState* hl) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = update_display_full_unlocked(hl);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_reader_cleanup(EpdiyHighlevelState* hl,const EpdWaveform* text_waveform) {
    bool held=pico_online_display_begin();
    if(s_follow_active)continuous_du_reset();
    s_follow_active=s_full_refresh_requested=false;
    display_main_transition_cancel();
    // 物理擦白与历史帧同步，不能只把软件历史置白；目标帧保持不变。
    // Erase physically before synchronizing white history; never merely fake the baseline or alter the target.
    use_scan_for(&E0470_WAVEFORM,MODE_GC16);epd_poweron();epd_clear();
    enum EpdDrawError result=update_display_from_white_with_unlocked(hl,text_waveform,MODE_GL16);
    pico_online_display_end(held);return result;
}
enum EpdDrawError update_display_image_gray(EpdiyHighlevelState* hl) {
    bool held = pico_online_display_begin();
    if (s_follow_active) continuous_du_reset();
    s_follow_active = s_full_refresh_requested = false;
    enum EpdDrawError result = update_display_image_gray_unlocked(hl);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode
) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_with_unlocked(hl, waveform, mode);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_area_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_area_with_unlocked(hl, waveform, mode, area);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_area_diff_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_area_diff_with_unlocked(hl, waveform, mode, area);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_area_full_with(
    EpdiyHighlevelState* hl, const EpdWaveform* waveform, enum EpdDrawMode mode,
    EpdRect area
) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_area_full_with_unlocked(hl, waveform, mode, area);
    pico_online_display_end(held);
    return result;
}

enum EpdDrawError update_display_water_turn(EpdiyHighlevelState* hl, EpdRect area,
                                             e0470_turn_dir_t dir) {
    bool held = pico_online_display_begin();
    enum EpdDrawError result = (s_follow_active || s_full_refresh_requested)
        ? update_display_full_unlocked(hl) : update_display_water_turn_unlocked(hl, area, dir);
    pico_online_display_end(held);
    return result;
}

void guard_draw_result(EpdiyHighlevelState* hl, enum EpdDrawError result) {
    bool held = pico_online_display_begin();
    guard_draw_result_unlocked(hl, result);
    pico_online_display_end(held);
}
