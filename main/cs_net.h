// Wi-Fi 连接、SteamDT 行情拉取与凭证持久化。
#pragma once

#include "cs_market_model.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool configured; // 已保存 Wi-Fi 名称
    bool connecting;
    bool connected;
    bool has_key;
    int8_t rssi;
    char ssid[33];
    char ip[16];
    char key_tail[8]; // 只显示 API Key 末四位
} cs_net_status_t;

esp_err_t cs_net_init(void);

bool cs_net_has_credentials(void);
bool cs_net_has_api_key(void);

esp_err_t cs_net_set_credentials(const char *ssid, const char *password);
esp_err_t cs_net_set_api_key(const char *api_key);

// 擦除 Wi-Fi 与 API Key 配置并断开当前连接。
esp_err_t cs_net_clear_config(void);

void cs_net_status(cs_net_status_t *out);

// 阻塞执行。成功或部分成功时返回 ESP_OK 并把结果写回 snapshot;
// 失败时 snapshot 保留旧数据,error 写明原因。必须在网络工作线程调用。
esp_err_t cs_net_fetch(cs_market_snapshot_t *snapshot, int period);
