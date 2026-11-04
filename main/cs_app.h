// 应用入口:外设、界面、按键、网络工作线程与自动刷新。
#pragma once

#include "bsp_button.h"
#include "cs_market_model.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t cs_app_start(void);

// 请求网络工作线程刷新。可从任意任务调用,不阻塞。
void cs_app_request_refresh(void);

// 复制最近发布的快照(供串口 status 命令使用)。
void cs_app_get_snapshot(cs_market_snapshot_t *out);

// 清空内存中的行情快照,并作废正在进行的刷新(reset 命令使用)。
void cs_app_clear_state(void);

// 提交按键事件到应用输入队列。可从按键回调调用,不阻塞。
void cs_app_submit_input(bsp_btn_t btn, bsp_btn_ev_t event);

#ifdef __cplusplus
}
#endif

