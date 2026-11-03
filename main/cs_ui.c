#include "cs_ui.h"

#include "cs_fonts.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

#define TAG "cs_ui"

#define CS_SCREEN_W 240
#define CS_SCREEN_H 320
#define CS_BAR_H 30
#define CS_BODY_H (CS_SCREEN_H - CS_BAR_H)

#define CS_COLOR_BG 0x0B1220
#define CS_COLOR_BAR 0x101A2B
#define CS_COLOR_CARD 0x162136
#define CS_COLOR_TEXT 0xE9EEF7
#define CS_COLOR_MUTED 0x8A99B4
#define CS_COLOR_ACCENT 0x5B8CFF
#define CS_COLOR_UP 0xFF5D5D
#define CS_COLOR_DOWN 0x39D98A
#define CS_COLOR_WARN 0xFFB020

static lv_obj_t *s_screen;
static lv_obj_t *s_pages[CS_PAGE_COUNT];
static lv_obj_t *s_page_dots[CS_PAGE_COUNT];
static lv_obj_t *s_wifi_dot;
static lv_obj_t *s_battery_label;

static lv_obj_t *s_index_value;
static lv_obj_t *s_index_diff;
static lv_obj_t *s_footer;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_series;
static int64_t s_chart_key = -1;

static lv_obj_t *s_watch_price[CS_MARKET_WATCH_COUNT];
static lv_obj_t *s_watch_delta[CS_MARKET_WATCH_COUNT];

static lv_obj_t *s_net_wifi;
static lv_obj_t *s_net_ip;
static lv_obj_t *s_net_key;
static lv_obj_t *s_net_refresh;
static lv_obj_t *s_net_cache;
static lv_obj_t *s_net_error;

static cs_page_t s_current_page = CS_PAGE_MARKET;

static lv_obj_t *card_create(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(CS_COLOR_CARD), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *label_create(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, &cs_font_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_label_set_text(label, "");
    return label;
}

static lv_obj_t *page_create(void)
{
    lv_obj_t *page = lv_obj_create(s_screen);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, CS_SCREEN_W, CS_BODY_H);
    lv_obj_set_pos(page, 0, CS_BAR_H);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    return page;
}

static lv_obj_t *dot_create(lv_obj_t *parent, int x, int y, uint32_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static void build_status_bar(void)
{
    lv_obj_t *bar = lv_obj_create(s_screen);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, CS_SCREEN_W, CS_BAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(CS_COLOR_BAR), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = label_create(bar, 10, 7, CS_COLOR_TEXT);
    lv_label_set_text(title, "看CS大盘");

    for (int i = 0; i < CS_PAGE_COUNT; i++) {
        s_page_dots[i] = dot_create(bar, 104 + i * 12, 12, CS_COLOR_MUTED);
    }

    s_wifi_dot = dot_create(bar, 180, 12, CS_COLOR_MUTED);
    s_battery_label = label_create(bar, 194, 7, CS_COLOR_TEXT);
    lv_obj_set_width(s_battery_label, 38);
    lv_obj_set_style_text_align(s_battery_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(s_battery_label, "--");
}

static void build_market_page(lv_obj_t *page)
{
    lv_obj_t *index_card = card_create(page, 8, 8, 224, 88);
    lv_obj_t *index_caption = label_create(index_card, 12, 10, CS_COLOR_MUTED);
    lv_label_set_text(index_caption, "大盘指数");

    lv_obj_t *source = label_create(index_card, 132, 10, CS_COLOR_MUTED);
    lv_obj_set_width(source, 80);
    lv_obj_set_style_text_align(source, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(source, "SteamDT");

    s_index_value = label_create(index_card, 12, 28, CS_COLOR_TEXT);
    lv_obj_set_style_text_font(s_index_value, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_index_value, "--");

    s_index_diff = label_create(index_card, 12, 64, CS_COLOR_MUTED);
    lv_label_set_text(s_index_diff, "等待刷新");

    lv_obj_t *chart_card = card_create(page, 8, 102, 224, 148);
    lv_obj_t *chart_caption = label_create(chart_card, 12, 9, CS_COLOR_MUTED);
    lv_label_set_text(chart_caption, "走势");

    s_chart = lv_chart_create(chart_card);
    lv_obj_set_pos(s_chart, 31, 32);
    lv_obj_set_size(s_chart, 162, 104);
    lv_obj_remove_style_all(s_chart);
    // 主区域:透明背景、无边框;内边距使曲线绘制区等比缩小10%
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_left(s_chart, 8, 0);
    lv_obj_set_style_pad_right(s_chart, 8, 0);
    lv_obj_set_style_pad_top(s_chart, 5, 0);
    lv_obj_set_style_pad_bottom(s_chart, 5, 0);
    lv_obj_set_style_pad_column(s_chart, 0, 0);
    lv_obj_set_style_pad_row(s_chart, 0, 0);
    // 分割线(水平网格):细、淡
    lv_obj_set_style_line_width(s_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_chart, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(CS_COLOR_MUTED), LV_PART_MAIN);
    // 走势折线:2px、实色、圆角端点
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(CS_COLOR_ACCENT), LV_PART_ITEMS);
    lv_obj_set_style_line_opa(s_chart, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(s_chart, true, LV_PART_ITEMS);
    // 数据点标记:完全隐藏(零尺寸+透明)
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_SCROLLABLE);

    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CS_MARKET_POINTS_MAX);
    lv_chart_set_div_line_count(s_chart, 3, 0);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s_series = lv_chart_add_series(s_chart, lv_color_hex(CS_COLOR_ACCENT),
                                   LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(s_chart, s_series, LV_CHART_POINT_NONE);

    s_footer = label_create(page, 8, 254, CS_COLOR_MUTED);
    lv_obj_set_width(s_footer, 224);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_footer, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_footer, "按 确定 刷新");
}

static void build_watch_page(lv_obj_t *page)
{
    lv_obj_t *caption = label_create(page, 10, 6, CS_COLOR_MUTED);
    lv_label_set_text(caption, "自选饰品");

    for (int i = 0; i < CS_MARKET_WATCH_COUNT; i++) {
        lv_obj_t *card = card_create(page, 8, 28 + i * 52, 224, 48);
        lv_obj_t *alias = label_create(card, 10, 14, CS_COLOR_TEXT);
        lv_label_set_text(alias, CS_MARKET_WATCH_DEFS[i].alias);
        lv_obj_set_width(alias, 104);
        lv_label_set_long_mode(alias, LV_LABEL_LONG_DOT);

        s_watch_price[i] = label_create(card, 112, 6, CS_COLOR_TEXT);
        lv_obj_set_width(s_watch_price[i], 100);
        lv_obj_set_style_text_align(s_watch_price[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(s_watch_price[i], "--");

        s_watch_delta[i] = label_create(card, 112, 26, CS_COLOR_MUTED);
        lv_obj_set_width(s_watch_delta[i], 100);
        lv_obj_set_style_text_align(s_watch_delta[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_text(s_watch_delta[i], "");
    }
}

static void build_status_page(lv_obj_t *page)
{
    lv_obj_t *card = card_create(page, 8, 4, 224, 212);
    lv_obj_t *caption = label_create(card, 12, 8, CS_COLOR_MUTED);
    lv_label_set_text(caption, "连接与数据");

    s_net_wifi = label_create(card, 12, 32, CS_COLOR_TEXT);
    lv_obj_set_width(s_net_wifi, 200);
    lv_label_set_text(s_net_wifi, "Wi-Fi: --");

    s_net_ip = label_create(card, 12, 54, CS_COLOR_TEXT);
    lv_obj_set_width(s_net_ip, 200);
    lv_label_set_text(s_net_ip, "IP: --");

    s_net_key = label_create(card, 12, 76, CS_COLOR_TEXT);
    lv_obj_set_width(s_net_key, 200);
    lv_label_set_text(s_net_key, "API Key: --");

    lv_obj_t *source = label_create(card, 12, 98, CS_COLOR_MUTED);
    lv_label_set_text(source, "数据源: SteamDT");

    s_net_refresh = label_create(card, 12, 120, CS_COLOR_TEXT);
    lv_obj_set_width(s_net_refresh, 200);
    lv_label_set_text(s_net_refresh, "更新: --");

    s_net_cache = label_create(card, 12, 142, CS_COLOR_MUTED);
    lv_obj_set_width(s_net_cache, 200);
    lv_label_set_text(s_net_cache, "缓存: --");

    s_net_error = label_create(card, 12, 166, CS_COLOR_WARN);
    lv_obj_set_width(s_net_error, 200);
    lv_obj_set_height(s_net_error, 36);
    lv_label_set_long_mode(s_net_error, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_net_error, "");

    lv_obj_t *hint1 = label_create(page, 8, 226, CS_COLOR_MUTED);
    lv_label_set_text(hint1, "串口配置: wifi 名称 密码");
    lv_obj_t *hint2 = label_create(page, 8, 246, CS_COLOR_MUTED);
    lv_label_set_text(hint2, "API Key: key 密钥");
    lv_obj_t *hint3 = label_create(page, 8, 266, CS_COLOR_MUTED);
    lv_label_set_text(hint3, "输入 help 查看全部命令");
}

void cs_ui_create(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(CS_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar();
    for (int i = 0; i < CS_PAGE_COUNT; i++) {
        s_pages[i] = page_create();
    }
    build_market_page(s_pages[CS_PAGE_MARKET]);
    build_watch_page(s_pages[CS_PAGE_WATCH]);
    build_status_page(s_pages[CS_PAGE_STATUS]);

    cs_ui_show_page(CS_PAGE_MARKET);
    lv_screen_load(s_screen);
}

void cs_ui_show_page(cs_page_t page)
{
    if (page >= CS_PAGE_COUNT) return;
    s_current_page = page;
    for (int i = 0; i < CS_PAGE_COUNT; i++) {
        if (i == (int)page) {
            lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_set_style_bg_color(s_page_dots[i],
            lv_color_hex(i == (int)page ? CS_COLOR_ACCENT : CS_COLOR_MUTED), 0);
    }
}

static void set_index_text(const cs_market_snapshot_t *snapshot)
{
    if (!snapshot->index_valid) {
        lv_label_set_text(s_index_value, "--");
        lv_obj_set_style_text_color(s_index_value, lv_color_hex(CS_COLOR_MUTED), 0);
        lv_label_set_text(s_index_diff, "等待刷新");
        lv_obj_set_style_text_color(s_index_diff, lv_color_hex(CS_COLOR_MUTED), 0);
        return;
    }

    char value[24];
    cs_market_format_price(snapshot->index_centi, value, sizeof(value));
    lv_label_set_text(s_index_value, value);
    lv_obj_set_style_text_color(s_index_value, lv_color_hex(CS_COLOR_TEXT), 0);

    char diff[24];
    char ratio[24];
    cs_market_format_price(snapshot->diff_centi, diff, sizeof(diff));
    cs_market_format_ratio(snapshot->ratio_x100, ratio, sizeof(ratio));
    char text[64];
    if (snapshot->diff_centi > 0) {
        snprintf(text, sizeof(text), "较昨日 +%s (%s)", diff, ratio);
    } else {
        snprintf(text, sizeof(text), "较昨日 %s (%s)", diff, ratio);
    }
    lv_label_set_text(s_index_diff, text);

    uint32_t color = CS_COLOR_MUTED;
    if (snapshot->diff_centi > 0) color = CS_COLOR_UP;
    else if (snapshot->diff_centi < 0) color = CS_COLOR_DOWN;
    lv_obj_set_style_text_color(s_index_diff, lv_color_hex(color), 0);
}

static void set_chart_text(const cs_market_snapshot_t *snapshot)
{
    int64_t key = snapshot->refreshed_at * 4 + snapshot->point_count +
                  (int64_t)snapshot->period * 1000000;
    if (key == s_chart_key) return;
    s_chart_key = key;

    int32_t min_value = 0;
    int32_t max_value = 0;
    if (snapshot->point_count < 2 ||
        !cs_market_chart_bounds(snapshot->point_value_centi,
                                snapshot->point_count, &min_value, &max_value)) {
        lv_chart_set_point_count(s_chart, CS_MARKET_POINTS_MAX);
        lv_chart_set_all_values(s_chart, s_series, LV_CHART_POINT_NONE);
        lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
        lv_chart_refresh(s_chart);
        return;
    }

    lv_chart_set_point_count(s_chart, (uint32_t)snapshot->point_count);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, min_value, max_value);
    lv_chart_set_series_values(s_chart, s_series, snapshot->point_value_centi,
                               (size_t)snapshot->point_count);
    lv_chart_refresh(s_chart);
}

static void set_footer_text(const cs_market_snapshot_t *snapshot)
{
    if (snapshot->refreshing) {
        lv_label_set_text(s_footer, "正在刷新行情…");
        lv_obj_set_style_text_color(s_footer, lv_color_hex(CS_COLOR_ACCENT), 0);
        return;
    }
    if (snapshot->error[0]) {
        lv_label_set_text(s_footer, snapshot->error);
        lv_obj_set_style_text_color(s_footer, lv_color_hex(CS_COLOR_WARN), 0);
        return;
    }

    char time_text[24];
    cs_market_format_time(snapshot->refreshed_at, false, time_text, sizeof(time_text));
    char text[64];
    if (snapshot->refreshed_at > 0) {
        if (snapshot->index_from_cache) {
            snprintf(text, sizeof(text), "缓存 · 更新于 %s", time_text);
        } else {
            snprintf(text, sizeof(text), "更新于 %s", time_text);
        }
    } else {
        snprintf(text, sizeof(text), "按 确定 刷新行情");
    }
    lv_label_set_text(s_footer, text);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(CS_COLOR_MUTED), 0);
}

static void set_watch_text(const cs_market_snapshot_t *snapshot)
{
    for (int i = 0; i < CS_MARKET_WATCH_COUNT; i++) {
        const cs_watch_quote_t *quote = &snapshot->watch[i];
        if (!quote->valid || quote->price_cents <= 0) {
            lv_label_set_text(s_watch_price[i], "--");
            lv_obj_set_style_text_color(s_watch_price[i], lv_color_hex(CS_COLOR_MUTED), 0);
            lv_label_set_text(s_watch_delta[i], "暂无报价");
            lv_obj_set_style_text_color(s_watch_delta[i], lv_color_hex(CS_COLOR_MUTED), 0);
            continue;
        }

        char price[24];
        cs_market_format_price(quote->price_cents, price, sizeof(price));
        char text[48];
        snprintf(text, sizeof(text), "¥%s", price);
        lv_label_set_text(s_watch_price[i], text);
        lv_obj_set_style_text_color(s_watch_price[i], lv_color_hex(CS_COLOR_TEXT), 0);

        bool known = false;
        int32_t delta = cs_market_price_diff(quote->price_cents, quote->prev_price_cents,
                                             quote->has_prev, &known);
        uint32_t color = CS_COLOR_MUTED;
        if (!known) {
            lv_label_set_text(s_watch_delta[i], "无历史报价");
        } else {
            char delta_text[24];
            cs_market_format_price(delta, delta_text, sizeof(delta_text));
            if (delta > 0) {
                snprintf(text, sizeof(text), "较上次 +%s", delta_text);
                color = CS_COLOR_UP;
            } else if (delta < 0) {
                snprintf(text, sizeof(text), "较上次 %s", delta_text);
                color = CS_COLOR_DOWN;
            } else {
                snprintf(text, sizeof(text), "较上次 持平");
            }
            lv_label_set_text(s_watch_delta[i], text);
        }
        lv_obj_set_style_text_color(s_watch_delta[i], lv_color_hex(color), 0);
    }
}

static void set_status_text(const cs_market_snapshot_t *snapshot,
                            const cs_net_status_t *net)
{
    if (!net->configured) {
        lv_label_set_text(s_net_wifi, "Wi-Fi: 未配置");
        lv_obj_set_style_text_color(s_net_wifi, lv_color_hex(CS_COLOR_WARN), 0);
        lv_label_set_text(s_net_ip, "IP: --");
    } else if (net->connected) {
        char text[64];
        snprintf(text, sizeof(text), "Wi-Fi: 已连接 %s", net->ssid);
        lv_label_set_text(s_net_wifi, text);
        lv_obj_set_style_text_color(s_net_wifi, lv_color_hex(CS_COLOR_DOWN), 0);
        snprintf(text, sizeof(text), "IP: %s (%d dBm)", net->ip, (int)net->rssi);
        lv_label_set_text(s_net_ip, text);
    } else {
        char text[64];
        snprintf(text, sizeof(text), "Wi-Fi: %s %s",
                 net->connecting ? "连接中" : "未连接", net->ssid);
        lv_label_set_text(s_net_wifi, text);
        lv_obj_set_style_text_color(s_net_wifi, lv_color_hex(CS_COLOR_WARN), 0);
        lv_label_set_text(s_net_ip, "IP: --");
    }

    if (net->has_key) {
        char text[40];
        snprintf(text, sizeof(text), "API Key: ****%s", net->key_tail);
        lv_label_set_text(s_net_key, text);
        lv_obj_set_style_text_color(s_net_key, lv_color_hex(CS_COLOR_TEXT), 0);
    } else {
        lv_label_set_text(s_net_key, "API Key: 未配置");
        lv_obj_set_style_text_color(s_net_key, lv_color_hex(CS_COLOR_WARN), 0);
    }

    char time_text[24];
    char text[64];
    if (snapshot->refreshed_at > 0) {
        cs_market_format_time(snapshot->refreshed_at, true, time_text, sizeof(time_text));
        snprintf(text, sizeof(text), "更新: %s", time_text);
    } else {
        snprintf(text, sizeof(text), "更新: --");
    }
    lv_label_set_text(s_net_refresh, text);

    if (snapshot->index_valid && snapshot->index_from_cache &&
        snapshot->index_update_time > 0) {
        cs_market_format_time(snapshot->index_update_time, true, time_text,
                              sizeof(time_text));
        snprintf(text, sizeof(text), "缓存: %s(离线快照)", time_text);
    } else if (snapshot->index_valid) {
        cs_market_format_time(snapshot->index_update_time, true, time_text,
                              sizeof(time_text));
        snprintf(text, sizeof(text), "缓存: 已同步 %s", time_text);
    } else {
        snprintf(text, sizeof(text), "缓存: 无");
    }
    lv_label_set_text(s_net_cache, text);

    if (snapshot->refreshing) {
        lv_label_set_text(s_net_error, "正在刷新行情…");
        lv_obj_set_style_text_color(s_net_error, lv_color_hex(CS_COLOR_ACCENT), 0);
    } else if (snapshot->error[0]) {
        lv_label_set_text(s_net_error, snapshot->error);
        lv_obj_set_style_text_color(s_net_error, lv_color_hex(CS_COLOR_WARN), 0);
    } else {
        lv_label_set_text(s_net_error, "运行正常");
        lv_obj_set_style_text_color(s_net_error, lv_color_hex(CS_COLOR_DOWN), 0);
    }
}

void cs_ui_update(const cs_market_snapshot_t *snapshot, const cs_net_status_t *net,
                  int battery_soc)
{
    if (!snapshot || !net) return;

    if (battery_soc < 0) {
        lv_label_set_text(s_battery_label, "--");
        lv_obj_set_style_text_color(s_battery_label, lv_color_hex(CS_COLOR_MUTED), 0);
    } else {
        char text[16];
        snprintf(text, sizeof(text), "%d%%", battery_soc);
        lv_label_set_text(s_battery_label, text);
        uint32_t color = CS_COLOR_TEXT;
        if (battery_soc <= 15) color = CS_COLOR_UP;
        else if (battery_soc <= 30) color = CS_COLOR_WARN;
        lv_obj_set_style_text_color(s_battery_label, lv_color_hex(color), 0);
    }

    uint32_t wifi_color = CS_COLOR_MUTED;
    if (!net->configured) wifi_color = CS_COLOR_MUTED;
    else if (net->connected) wifi_color = CS_COLOR_DOWN;
    else if (net->connecting) wifi_color = CS_COLOR_WARN;
    else wifi_color = CS_COLOR_UP;
    lv_obj_set_style_bg_color(s_wifi_dot, lv_color_hex(wifi_color), 0);

    for (int i = 0; i < CS_PAGE_COUNT; i++) {
        lv_obj_set_style_bg_color(s_page_dots[i],
            lv_color_hex(i == (int)s_current_page ? CS_COLOR_ACCENT : CS_COLOR_MUTED), 0);
    }

    set_index_text(snapshot);
    set_chart_text(snapshot);
    set_footer_text(snapshot);
    set_watch_text(snapshot);
    set_status_text(snapshot, net);
}
