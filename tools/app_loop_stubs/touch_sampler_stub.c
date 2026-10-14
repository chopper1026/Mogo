// 宿主测试桩：采样任务不可用，主循环走降级直读分支（与 F4 之前行为一致），
// 用于回归验证 app_loop 的 ingest/dispatch 管线抽取。真机构建链接真实
// touch_sampler.c，本文件不参与。
// / Host test stub: the sampler task is unavailable so the loop takes the
// direct-read fallback (identical to pre-F4 behavior), regression-testing the
// ingest/dispatch extraction in app_loop. Device builds link the real
// touch_sampler.c; this file is not part of them.
#include "touch_sampler.h"

bool touch_sampler_start(cst836u_handle_t tp) { (void)tp; return false; }
bool touch_sampler_active(void) { return false; }
unsigned touch_sampler_pending(void) { return 0; }
bool touch_sampler_pop(touch_frame_t* out) { (void)out; return false; }
