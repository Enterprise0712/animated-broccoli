#include "cs_app.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "cs_console.h"
#include "cs_market_store.h"
#include "cs_net.h"
#include "cs_ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "cs_app";

#define CS_INPUT_QUEUE_DEPTH 8
// 快照由 view_timer 以 1Hz 更新,界面刷新与之对齐即可;更快的周期
// 只会把相同文本反复写回 lv_label,白白消耗 LVGL 内存池的 malloc/free。
#define CS_UI_TICK_MS 1000
#define CS_VIEW_TICK_US (1LL * 1000 * 1000)
#define CS_AUTO_REFRESH_US (30LL * 1000 * 1000)
#define CS_AUTO_REFRESH_INTERVAL_S 300

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef struct {
    cs_market_snapshot_t snapshot;
    cs_net_status_t net;
    int battery_soc;
} cs_view_t;

static cs_market_snapshot_t s_snapshot;
static cs_view_t s_view;
static SemaphoreHandle_t s_snapshot_mutex;
static SemaphoreHandle_t s_refresh_signal;
static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static TaskHandle_t s_net_task;
static volatile int s_period = 2;
static cs_page_t s_page = CS_PAGE_MARKET;
// 每次清除状态就自增,让正在进行的刷新结果失效,避免它把旧数据写回缓存。
static volatile uint32_t s_refresh_generation;
// 记录上一次"首次自动拉取"的时刻,避免刷新持续失败时每 2 秒重试打爆接口。
static int64_t s_last_auto_probe_us;
// 开机后是否已完成第一次自动拉取。缓存里带着上次的 refreshed_at,
// 不能用它判断"是否需要首拉",否则开机后仍要等 30 秒。
static bool s_boot_refresh_done;

static void snapshot_lock(void)
{
    xSemaphoreTake(s_snapshot_mutex, portMAX_DELAY);
}

static void snapshot_unlock(void)
{
    xSemaphoreGive(s_snapshot_mutex);
}

void cs_app_request_refresh(void)
{
    if (s_refresh_signal) xSemaphoreGive(s_refresh_signal);
}

void cs_app_get_snapshot(cs_market_snapshot_t *out)
{
    if (!out || !s_snapshot_mutex) return;
    snapshot_lock();
    *out = s_snapshot;
    snapshot_unlock();
}

void cs_app_clear_state(void)
{
    // 让正在进行的刷新结果作废,并清空内存快照,与 NVS 缓存的清除保持一致。
    s_refresh_generation++;
    s_last_auto_probe_us = 0;
    s_boot_refresh_done = false;
    if (!s_snapshot_mutex) return;
    snapshot_lock();
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.period = s_period;
    s_view.snapshot = s_snapshot;
    snapshot_unlock();
    // 丢弃尚未被网络线程消费的刷新请求。
    if (s_refresh_signal) xSemaphoreTake(s_refresh_signal, 0);
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        // 平时阻塞等待刷新请求;超时唤醒只用于"首次联网后自动拉一次",
        // 否则开机后要等 30 秒的 auto_refresh_cb 才会出现第一份数据。
        bool requested = (xSemaphoreTake(s_refresh_signal, pdMS_TO_TICKS(2000)) == pdTRUE);
        if (!requested) {
            cs_net_status_t net;
            cs_net_status(&net);
            if (!net.connected || !net.has_key) continue;
            // 开机后已完成首拉:后续刷新交给 auto_refresh_cb 的 5 分钟节奏。
            if (s_boot_refresh_done) continue;
            // 刷新失败时这里会反复进来,限制最短重试间隔,避免打爆指数接口。
            int64_t now_us = esp_timer_get_time();
            if (s_last_auto_probe_us > 0 &&
                now_us - s_last_auto_probe_us < 10LL * 1000 * 1000) {
                continue;
            }
            s_last_auto_probe_us = now_us;
            ESP_LOGI(TAG, "首次联网,自动拉取行情");
        }

        const uint32_t generation = s_refresh_generation;

        cs_market_snapshot_t work;
        const int period = s_period;
        snapshot_lock();
        work = s_snapshot;
        snapshot_unlock();

        work.refreshing = true;
        work.period = period;
        snapshot_lock();
        s_snapshot = work;
        snapshot_unlock();

        esp_err_t err = cs_net_fetch(&work, period);
        work.refreshing = false;
        if (generation != s_refresh_generation) {
            // 刷新期间状态被清除(reset),丢弃本次结果,避免把旧数据写回缓存和界面。
            ESP_LOGI(TAG, "刷新结果已过期,丢弃");
            continue;
        }
        if (err != ESP_OK) {
            if (work.error[0] == 0) {
                snprintf(work.error, sizeof(work.error), "刷新失败: %s",
                         esp_err_to_name(err));
            }
            // 刷新失败时保留上一份数据,并明确标注它是缓存。
            work.index_from_cache = work.index_valid;
        }

        snapshot_lock();
        s_snapshot = work;
        snapshot_unlock();

        if (err == ESP_OK) {
            s_boot_refresh_done = true;
            cs_store_save(&work);
            ESP_LOGI(TAG, "行情已刷新: 指数 %s", work.index_valid ? "OK" : "无");
        } else {
            ESP_LOGW(TAG, "行情刷新失败: %s (%s)", esp_err_to_name(err), work.error);
        }
    }
}

static void ui_tick(lv_timer_t *timer)
{
    (void)timer;
    cs_view_t view;
    snapshot_lock();
    view = s_view;
    snapshot_unlock();
    cs_ui_update(&view.snapshot, &view.net, view.battery_soc);
}

static void view_timer_cb(void *arg)
{
    (void)arg;
    cs_net_status_t net;
    cs_net_status(&net);
    int soc = bsp_battery_soc();

    snapshot_lock();
    s_view.snapshot = s_snapshot;
    s_view.net = net;
    s_view.battery_soc = soc;
    snapshot_unlock();
}

static void auto_refresh_cb(void *arg)
{
    (void)arg;
    cs_net_status_t net;
    cs_net_status(&net);
    if (!net.connected || !net.has_key) return;

    cs_market_snapshot_t snapshot;
    cs_app_get_snapshot(&snapshot);
    if (snapshot.refreshing) return;

    int64_t now = (int64_t)time(NULL);
    if (snapshot.refreshed_at == 0 ||
        now - snapshot.refreshed_at >= CS_AUTO_REFRESH_INTERVAL_S) {
        cs_app_request_refresh();
    }
}

static void show_page(cs_page_t page)
{
    s_page = page;
    if (!bsp_lvgl_lock(500)) return;
    cs_ui_show_page(page);
    bsp_lvgl_unlock();
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) != pdTRUE) continue;

        if (input.event != BSP_BTN_CLICK) continue;

        if (input.btn == BSP_BTN_UP) {
            show_page((cs_page_t)(((int)s_page - 1 + CS_PAGE_COUNT) % CS_PAGE_COUNT));
        } else if (input.btn == BSP_BTN_DOWN) {
            show_page((cs_page_t)(((int)s_page + 1) % CS_PAGE_COUNT));
        } else if (input.btn == BSP_BTN_OK) {
            cs_app_request_refresh();
        }
    }
}

// 按键回调运行在共享 esp_timer 任务上:只入队,不阻塞。
void cs_app_submit_input(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (!s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

static esp_err_t input_start(void)
{
    s_input_queue = xQueueCreate(CS_INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "cs_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t timers_start(void)
{
    esp_timer_create_args_t view_args = {
        .callback = view_timer_cb,
        .name = "cs_view",
    };
    esp_timer_handle_t view_timer = NULL;
    if (esp_timer_create(&view_args, &view_timer) != ESP_OK) return ESP_ERR_NO_MEM;
    if (esp_timer_start_periodic(view_timer, CS_VIEW_TICK_US) != ESP_OK) {
        esp_timer_delete(view_timer);
        return ESP_FAIL;
    }

    esp_timer_create_args_t auto_args = {
        .callback = auto_refresh_cb,
        .name = "cs_auto",
    };
    esp_timer_handle_t auto_timer = NULL;
    if (esp_timer_create(&auto_args, &auto_timer) != ESP_OK) return ESP_ERR_NO_MEM;
    if (esp_timer_start_periodic(auto_timer, CS_AUTO_REFRESH_US) != ESP_OK) {
        esp_timer_delete(auto_timer);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t cs_app_start(void)
{
    s_snapshot_mutex = xSemaphoreCreateMutex();
    s_refresh_signal = xSemaphoreCreateBinary();
    if (!s_snapshot_mutex || !s_refresh_signal) return ESP_ERR_NO_MEM;

    memset(&s_snapshot, 0, sizeof(s_snapshot));
    memset(&s_view, 0, sizeof(s_view));
    s_view.battery_soc = -1;
    s_snapshot.period = s_period;

    bsp_i2c_init();
    vTaskDelay(pdMS_TO_TICKS(1));
    bsp_i2c_scan();
    vTaskDelay(pdMS_TO_TICKS(1));

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return ESP_FAIL;
    }
    bsp_display_backlight(100);
    vTaskDelay(pdMS_TO_TICKS(1));

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,右上角电量将显示 --");
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    esp_err_t err = cs_net_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "网络初始化失败: %s,仍可显示缓存", esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    if (cs_store_load(&s_snapshot) == ESP_OK) {
        s_snapshot.index_from_cache = s_snapshot.index_valid;
        s_snapshot.refreshing = false;
        // 缓存里保存的是上一次会话的错误文本,开机时要清掉,避免显示过期提示。
        s_snapshot.error[0] = 0;
        if (s_snapshot.period >= 1 && s_snapshot.period <= 3) {
            s_period = s_snapshot.period;
        }
        ESP_LOGI(TAG, "已载入缓存行情");
    }

    cs_net_status_t net;
    cs_net_status(&net);
    int soc = bsp_battery_soc();
    s_view.snapshot = s_snapshot;
    s_view.net = net;
    s_view.battery_soc = soc;
    vTaskDelay(pdMS_TO_TICKS(1));

    if (!bsp_lvgl_lock(1000)) return ESP_ERR_TIMEOUT;
    cs_ui_create();
    cs_ui_show_page(CS_PAGE_MARKET);
    cs_ui_update(&s_snapshot, &net, soc);
    lv_timer_create(ui_tick, CS_UI_TICK_MS, NULL);
    bsp_lvgl_unlock();

    if (timers_start() != ESP_OK) {
        ESP_LOGW(TAG, "定时器创建失败,自动刷新不可用");
    }

    if (input_start() != ESP_OK) {
        ESP_LOGE(TAG, "按键任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(net_task, "cs_net", 8192, NULL, 4, &s_net_task) != pdPASS) {
        ESP_LOGE(TAG, "网络任务创建失败");
        return ESP_ERR_NO_MEM;
    }

    cs_console_init();
    ESP_LOGI(TAG, "应用就绪");
    return ESP_OK;
}
