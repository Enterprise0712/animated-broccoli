// main/main.c —— 「看 CS 大盘」应用入口与按键唤醒守卫。
#include "bsp_button.h"
#include "bsp_pins.h"
#include "cs_app.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "key_guard.h"

static const char *TAG = "main";

#define MAIN_KEY_GUARD_MAX_US (10LL * 1000 * 1000)

static key_guard_t s_key_guard;
static volatile bool s_input_ready;

// 按键回调运行在共享 esp_timer 任务上:先消耗唤醒守卫,再判断分发是否就绪。
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready) return;
    cs_app_submit_input(btn, ev);
}

void app_main(void)
{
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), MAIN_KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    esp_err_t err = cs_app_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动失败: %s", esp_err_to_name(err));
        return;
    }

    err = bsp_button_init(on_key, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s,可用串口命令操作", esp_err_to_name(err));
        return;
    }
    if (s_key_guard.armed) {
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
    }
    s_input_ready = true;
}
