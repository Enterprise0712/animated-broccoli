// 行情快照的 NVS 持久化:重启后先显示上次成功的数据,并标注“缓存”。
#pragma once

#include "cs_market_model.h"
#include "esp_err.h"

esp_err_t cs_store_load(cs_market_snapshot_t *out);
esp_err_t cs_store_save(const cs_market_snapshot_t *snapshot);
esp_err_t cs_store_clear(void);
