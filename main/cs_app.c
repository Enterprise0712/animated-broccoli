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
#define CS_UI_TICK_MS 500
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

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_refresh_signal, portMAX_DELAY) != pdTRUE) continue;

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
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,应用无法继续");
        return ESP_FAIL;
    }
    bsp_display_backlight(100);

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计不可用,右上角电量将显示 --");
    }

    esp_err_t err = cs_net_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "网络初始化失败: %s,仍可显示缓存", esp_err_to_name(err));
    }

    if (cs_store_load(&s_snapshot) == ESP_OK) {
        s_snapshot.index_from_cache = s_snapshot.index_valid;
        s_snapshot.refreshing = false;
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
