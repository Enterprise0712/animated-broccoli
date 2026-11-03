// 三页行情界面:大盘 / 自选 / 状态。所有函数都必须在持有 LVGL 锁时调用。
#pragma once

#include "cs_market_model.h"
#include "cs_net.h"
#include "lvgl.h"

typedef enum {
    CS_PAGE_MARKET = 0,
    CS_PAGE_WATCH,
    CS_PAGE_STATUS,
    CS_PAGE_COUNT,
} cs_page_t;

void cs_ui_create(void);
void cs_ui_show_page(cs_page_t page);

// 用最新快照刷新界面内容;battery_soc 小于 0 表示电量未知。
void cs_ui_update(const cs_market_snapshot_t *snapshot, const cs_net_status_t *net,
                  int battery_soc);
