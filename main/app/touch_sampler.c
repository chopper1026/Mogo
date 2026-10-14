// F4 触摸采样任务实现。结构：5ms 节拍读 CST836U，帧写入环形队列；主循环排空回放。
// 队列采用"序号环 + 读者侧钳制"：写者只发布写序号 wr（release），读者维护私有读
// 序号 rd，发现 wr - rd 超过容量即钳制到 wr - LEN（等价丢最旧，最新边沿总在队尾
// 被保全）。err 帧不入队：芯片复位/唤醒窗口的 NACK 不构成输入事件；深睡模式下
// cst836u_read 早退返回零帧（无 I2C 访问），照常入队无害。
// / F4 touch sampler implementation: 5ms polling task feeds an SPSC ring built
// on a monotonic write sequence with a reader-side clamp (overwrite-oldest, so
// the newest edges always survive at the tail). Error frames are dropped; the
// driver's deep-sleep early return yields harmless zero frames.

#include "touch_sampler.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TS_TAG "touch_sampler"
#define TS_PERIOD_MS 5
// 2 的幂；512 × 8B = 4KB .bss。512 帧 × 5ms = 2.56s，覆盖整刷 ~1528ms 与跨章
// 排版大部分窗口（方案 §3.1）。/ Power of two; 4KB static. 2.56s of coverage.
#define TS_QUEUE_LEN 512
#define TS_QUEUE_MASK (TS_QUEUE_LEN - 1)
#define TS_STACK_SIZE 3072  // 对齐 tdbeep 先例；调用深度仅 I2C 同步事务。
#define TS_PRIORITY 4       // 高于 book_prep(3)：跨章排版期间采样不被饿死；
                            // 与 OTA worker(4) 同优时间片轮转，双方每周期 ≤1ms。
#define TS_CORE 1           // 避开 core 0 的 WiFi/BT 高优任务。

static cst836u_handle_t s_tp;
static TaskHandle_t s_task;
static atomic_uint s_wr;     // 写序号：写者单写（release 发布），读者 acquire 读。
static unsigned s_rd;        // 读序号：仅读者（主循环）访问，无需原子。
static touch_frame_t s_queue[TS_QUEUE_LEN];

static void sampler_task(void* arg) {
    (void)arg;
    cst836u_touch_t t;
    while (true) {
        if (cst836u_read(s_tp, &t) == ESP_OK) {
            unsigned wr = s_wr;  // 写者私有
            touch_frame_t* slot = &s_queue[wr & TS_QUEUE_MASK];
            slot->t_ms = (uint32_t)(esp_timer_get_time() / 1000);
            slot->x = t.x;
            slot->y = t.y;
            slot->count = t.count;
            slot->touched = t.touched ? 1 : 0;
            // release：保证帧数据先于序号对读者可见。
            atomic_store_explicit(&s_wr, wr + 1, memory_order_release);
        }
        vTaskDelay(pdMS_TO_TICKS(TS_PERIOD_MS));
    }
}

bool touch_sampler_start(cst836u_handle_t tp) {
    if (s_task != NULL) return true;
    if (tp == NULL) return false;
    s_tp = tp;
    atomic_init(&s_wr, 0);
    s_rd = 0;
    if (xTaskCreatePinnedToCore(sampler_task, "tsampler", TS_STACK_SIZE, NULL,
                                TS_PRIORITY, &s_task, TS_CORE) != pdPASS) {
        s_task = NULL;
        ESP_LOGE(TS_TAG, "sampler task create failed; loop falls back to direct read");
        return false;
    }
    ESP_LOGI(TS_TAG, "sampler on core %d prio %d every %dms, queue %d x %uB",
             TS_CORE, TS_PRIORITY, TS_PERIOD_MS, TS_QUEUE_LEN,
             (unsigned)sizeof(touch_frame_t));
    return true;
}

bool touch_sampler_active(void) {
    return s_task != NULL;
}

unsigned touch_sampler_pending(void) {
    unsigned wr = atomic_load_explicit(&s_wr, memory_order_acquire);
    unsigned rd = s_rd;
    if (wr - rd > TS_QUEUE_LEN) rd = wr - TS_QUEUE_LEN;
    return wr - rd;
}

bool touch_sampler_pop(touch_frame_t* out) {
    if (out == NULL || s_task == NULL) return false;
    unsigned wr = atomic_load_explicit(&s_wr, memory_order_acquire);
    unsigned rd = s_rd;
    if (wr - rd > TS_QUEUE_LEN) rd = wr - TS_QUEUE_LEN;  // 慢消费者：跳过被覆盖的最旧帧
    if (wr == rd) return false;
    *out = s_queue[rd & TS_QUEUE_MASK];
    s_rd = rd + 1;
    return true;
}
