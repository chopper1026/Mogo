/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/// 空闲时启动有界后台对时；主循环独占 PMU 与 NVS 的交接。
/// Start bounded background sync while idle; the main loop alone hands time to PMU/NVS.
void app_time_service_poll(bool idle);
/// 取消在途后台校准（组件内 100 ms 分片生效）；没有在途任务时无副作用。
/// Cancel an in-flight background calibration (takes effect in the component's 100 ms slices); a
/// no-op when nothing is in flight.
void app_time_service_cancel(void);
/// 手动（设置页）或升级页对时成功后登记：持久化时间线并清开机重试预算。
/// Register a manual or updater sync success: persist the timeline and clear the per-boot budget.
void app_time_service_note_success(uint32_t utc_seconds);
