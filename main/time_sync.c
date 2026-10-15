/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：自动对时。深睡期间主机完全断电，唯一走时源是 PMU 的 RC RTC（无晶振，±1–2%），
 * 每次开机由 restore_time_from_pmu() 把累积漂移灌回系统时钟；本文件负责在有网时定期把它校回来。
 *
 * English: Auto time sync. While the host is unpowered the PMU's crystal-less RC RTC (±1–2%) is the
 * only timekeeper, and restore_time_from_pmu() injects that drift at every boot; this file pulls the
 * clock back whenever a network is reachable.
 *
 * 冻结：不主动改时区/UTC+8；不在后台任务里碰 PMU 与无线会话（会话属界面循环，PMU 写入同任务）；
 * 只复用 read_pico_transfer 的会话与 SNTP，不自行初始化无线。
 * Frozen: no timezone work; the background worker touches neither the PMU nor the session (the loop
 * owns both); the radio and SNTP stay in read_pico_transfer.
 */
#include "time_sync.h"

#include <stdatomic.h>
#include <time.h>

#include "ble_page_turner.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "time_sync_policy.h"

#define TAG "time_sync"

// 会话就绪与单次联网的预算；到点释放会话，不占着无线等。
// Budgets for the link and the single sync; the session is released at the deadline.
#define TIME_SYNC_READY_TIMEOUT_MS  30000
#define TIME_SYNC_WORKER_TIMEOUT_MS 20000
// 电量低于该百分比且未充电时不为对时开无线；油量表未知视为可以通过。
// Below this state of charge, and while not charging, keep the radio off; an unknown gauge passes.
#define TIME_SYNC_MIN_BATTERY_PERCENT 10

typedef enum {
    TIME_SYNC_IDLE,
    TIME_SYNC_WAIT_READY,
    TIME_SYNC_RUNNING,
} time_sync_state_t;

static time_sync_state_t s_state;
// 会话是自动对时自己启动的才允许释放；设置页/传书的会话不许动。
// Only a session this service started may be released; Settings and transfer sessions are left alone.
static bool s_owned;
static int64_t s_deadline_ms;
static unsigned s_boot_attempts;
static atomic_bool s_cancel;
static atomic_bool s_worker_live;
static atomic_bool s_result_ok;
static atomic_uint s_result_utc;

/* ---- 判定 / Decisions ---- */

static bool link_ready(const read_pico_transfer_status_t *status) {
    return status != NULL && status->mode == READ_PICO_TRANSFER_MODE_STA && status->network_ready;
}

static void note_attempt(uint32_t now) {
    ++s_boot_attempts;
    if (app_time_sync_clock_valid(now)) app_settings_set_time_sync_attempt(now);
}

// 电量极低且未充电时不为了对时开无线。/ Low and unplugged: not worth a radio session.
static bool battery_allows(void) {
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    int percent = pmu_battery_percent(pmu);
    return percent < 0 || percent >= TIME_SYNC_MIN_BATTERY_PERCENT || pmu_battery_charging(pmu);
}

// 蓝牙翻页器正在连接时不开无线：WiFi 启动会拆掉 NimBLE。
// Keep the radio off while a Bluetooth page-turner is linked: starting WiFi tears NimBLE down.
static bool ble_allows(void) {
    return !app_settings_ble_turner() || (!ble_pt_connected() && !ble_pt_connecting());
}

static bool may_start(bool blocked) {
    if (blocked || !app_settings_auto_time_sync()) return false;
    if (s_boot_attempts >= APP_TIME_SYNC_ATTEMPTS_PER_BOOT) return false;
    if (!battery_allows() || !ble_allows()) return false;
    char ssid[33];
    bool configured = false;
    return read_pico_transfer_get_saved_wifi(ssid, &configured) == ESP_OK && configured;
}

/* ---- 写入 / Apply ---- */

// 与设置页手动对时同一套判据：写 PMU 后回读，偏差超过 10 s 视为失败。
// Same criteria as the manual sync in Settings: write the PMU, read it back, fail past 10 s.
static bool apply_time(uint32_t utc) {
    uint8_t payload[4] = {(uint8_t)utc, (uint8_t)(utc >> 8), (uint8_t)(utc >> 16), (uint8_t)(utc >> 24)};
    if (read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload)) != ESP_OK) return false;
    if (read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0) != ESP_OK) return false;
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    return pmu != NULL && pmu->time_synced && pmu->unix_sec >= utc && pmu->unix_sec - utc <= 10;
}

/* ---- 联网 / Network ---- */

// 收敛一次尝试：成功记时间戳，失败记冷却；自己启动的会话在这里释放。
// Settle one attempt: timestamps on success, cooldown on failure, and release a session we own.
static void settle(bool applied, uint32_t utc) {
    atomic_store(&s_cancel, true);
    if (s_owned) {
        read_pico_transfer_stop();
        s_owned = false;
    }
    s_state = TIME_SYNC_IDLE;
    if (applied) {
        app_time_sync_note_success(utc);
        ESP_LOGI(TAG, "clock seeded from network utc=%u", (unsigned)utc);
    } else {
        note_attempt((uint32_t)time(NULL));
        ESP_LOGW(TAG, "skipped, next attempt after cooldown");
    }
}

// 只做 SNTP；PMU、NVS 与会话生命周期都留在界面循环，PMU 的 I2C 因此始终单任务访问。
// Network only: the PMU, NVS and session lifecycle stay on the UI loop, keeping PMU I2C single-task.
static void sync_worker(void *argument) {
    (void)argument;
    uint32_t utc = 0;
    esp_err_t err = read_pico_transfer_sync_time_online_cancelable(&utc, &s_cancel);
    if (err == ESP_OK && !atomic_load(&s_cancel)) {
        atomic_store(&s_result_utc, (unsigned)utc);
        atomic_store(&s_result_ok, true);
    } else {
        ESP_LOGW(TAG, "sync failed: %s", esp_err_to_name(err));
    }
    atomic_store(&s_worker_live, false);
    vTaskDelete(NULL);
}

static void start_worker(int64_t now_ms) {
    if (!atomic_load(&s_worker_live)) {
        atomic_store(&s_cancel, false);
        atomic_store(&s_result_ok, false);
        atomic_store(&s_result_utc, 0);
        atomic_store(&s_worker_live, true);
        if (xTaskCreate(sync_worker, "time_sync", 4096, NULL, 4, NULL) != pdPASS) {
            atomic_store(&s_worker_live, false);
            ESP_LOGW(TAG, "no memory for sync worker");
            settle(false, 0);
            return;
        }
        ESP_LOGI(TAG, "syncing over the saved network");
    }
    s_state = TIME_SYNC_RUNNING;
    s_deadline_ms = now_ms + TIME_SYNC_WORKER_TIMEOUT_MS;
}

static void poll_idle(int64_t now_ms, bool blocked) {
    // 上一个 worker 还在收尾：这一轮不动，下一轮再判。
    // The previous worker is still exiting; decide again on the next tick.
    if (atomic_load(&s_worker_live)) return;
    uint32_t now = (uint32_t)time(NULL);
    if (app_time_sync_clock_valid(now)) {
        if (!app_time_sync_due(now, app_settings_time_sync_last(), app_settings_time_sync_attempt())) return;
    } else if (s_boot_attempts != 0) {
        // 时钟无效时没有冷却依据，每次开机只试一次。
        // With an invalid clock there is no cooldown to honour: one attempt per power cycle.
        return;
    }
    if (!may_start(blocked)) return;

    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.state != READ_PICO_TRANSFER_STOPPED) {
        // 已有会话（设置页、传书或快速开关）：顺着它同步，不抢所有权、不释放它。
        // An existing session (Settings, transfer, quick toggle) is reused and never released.
        if (link_ready(&status)) {
            start_worker(now_ms);
        } else {
            s_state = TIME_SYNC_WAIT_READY;
            s_deadline_ms = now_ms + TIME_SYNC_READY_TIMEOUT_MS;
        }
        return;
    }
    read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .network_only = true};
    esp_err_t err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "session start failed: %s", esp_err_to_name(err));
        note_attempt(now);
        return;
    }
    s_owned = true;
    s_state = TIME_SYNC_WAIT_READY;
    s_deadline_ms = now_ms + TIME_SYNC_READY_TIMEOUT_MS;
    ESP_LOGI(TAG, "connecting for a time sync");
}

static void poll_wait_ready(int64_t now_ms, bool blocked) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (link_ready(&status)) {
        start_worker(now_ms);
        return;
    }
    bool dead = blocked || status.state == READ_PICO_TRANSFER_STOPPED ||
        status.state == READ_PICO_TRANSFER_ERROR;
    if (dead || now_ms >= s_deadline_ms) {
        ESP_LOGW(TAG, "no link for time sync (%s)", dead ? "session gone" : "timeout");
        settle(false, 0);
    }
}

static void poll_running(int64_t now_ms) {
    if (atomic_load(&s_worker_live)) {
        if (now_ms < s_deadline_ms) return;
        // 超时：取消后按失败收敛，worker 会在 500 ms 内自行退出。
        // Timeout: cancel and settle as a failure; the worker exits within 500 ms by itself.
        ESP_LOGW(TAG, "network time wait timed out");
        settle(false, 0);
        return;
    }
    bool ok = atomic_load(&s_result_ok);
    uint32_t utc = atomic_load(&s_result_utc);
    atomic_store(&s_result_ok, false);
    settle(ok && apply_time(utc), utc);
}

/* ---- 对外 / Public ---- */

void app_time_sync_poll(int64_t now_ms, bool blocked) {
    switch (s_state) {
        case TIME_SYNC_IDLE: poll_idle(now_ms, blocked); break;
        case TIME_SYNC_WAIT_READY: poll_wait_ready(now_ms, blocked); break;
        case TIME_SYNC_RUNNING: poll_running(now_ms); break;
    }
}

void app_time_sync_yield_radio(void) {
    atomic_store(&s_cancel, true);
    if (s_owned) {
        read_pico_transfer_stop();
        s_owned = false;
    }
    s_state = TIME_SYNC_IDLE;
    s_deadline_ms = 0;
}

void app_time_sync_note_success(uint32_t utc_seconds) {
    if (!app_time_sync_clock_valid(utc_seconds)) return;
    // 一次成功对时（手动或升级页）也算本机本轮已对时，本次开机的重试预算随之清零。
    // A success from any entry point counts for this power cycle, so the retry budget resets too.
    s_boot_attempts = 0;
    app_settings_set_time_sync_last(utc_seconds);
    app_settings_set_time_sync_attempt(utc_seconds);
}
