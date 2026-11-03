// CS2 饰品大盘的数据模型与纯逻辑(不依赖 ESP-IDF/LVGL,可主机测试)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CS_MARKET_WATCH_COUNT 5
#define CS_MARKET_POINTS_MAX 60
#define CS_MARKET_ERROR_MAX 96
#define CS_MARKET_NAME_MAX 72
#define CS_MARKET_ALIAS_MAX 24

typedef struct {
    const char *alias;            // 界面显示用的中文短名
    const char *market_hash_name; // Steam 市场完整英文名
} cs_watch_def_t;

typedef struct {
    int32_t price_cents;      // 在售价,单位分;valid=false 时无意义
    int32_t prev_price_cents; // 上一次成功取值,用于“较上次”
    int64_t update_time;      // 平台报价时间(Unix 秒)
    bool valid;
    bool has_prev;
} cs_watch_quote_t;

typedef struct {
    bool index_valid;
    bool index_from_cache; // true 表示首屏/当前指数来自本地缓存
    int32_t index_centi;   // 大盘指数 ×100
    int32_t diff_centi;    // 较昨日涨跌值 ×100
    int32_t ratio_x100;    // 较昨日涨跌幅(百分比)×100,1.5761% -> 158
    int64_t index_update_time;
    int period; // 1=时K 2=日K 3=周K
    int point_count;
    int64_t point_time[CS_MARKET_POINTS_MAX];
    int32_t point_value_centi[CS_MARKET_POINTS_MAX];
    cs_watch_quote_t watch[CS_MARKET_WATCH_COUNT];
    int64_t refreshed_at; // 本机最近一次成功刷新(Unix 秒)
    bool refreshing;
    char error[CS_MARKET_ERROR_MAX];
} cs_market_snapshot_t;

extern const cs_watch_def_t CS_MARKET_WATCH_DEFS[CS_MARKET_WATCH_COUNT];

void cs_market_snapshot_clear(cs_market_snapshot_t *snapshot);

// 百分比数值(1.5761 表示 1.5761%)转内部 ×100 存储。
int32_t cs_market_ratio_to_x100(double ratio_percent);
int32_t cs_market_ratio_from_x100(int32_t ratio_x100);

// 返回 new-old;old 无效时 *known=false。
int32_t cs_market_price_diff(int32_t new_cents, int32_t old_cents, bool old_valid,
                             bool *known);

// “1234.56” 或 “--”;缓冲区至少 24 字节。
void cs_market_format_price(int32_t cents, char *out, size_t out_size);
// “+1.58%” / “-0.30%” / “0.00%”;缓冲区至少 24 字节。
void cs_market_format_ratio(int32_t ratio_x100, char *out, size_t out_size);
// 本地时间 “HH:MM” 或 “MM-DD HH:MM”;缓冲区至少 24 字节。
void cs_market_format_time(int64_t unix_seconds, bool with_date, char *out,
                           size_t out_size);

// 走势图纵轴范围:过滤无效点后留出约 5% 余量。空数据返回 false。
bool cs_market_chart_bounds(const int32_t *values, int count, int32_t *min_value,
                            int32_t *max_value);
// 1 -> 2 -> 3 -> 1。
int cs_market_period_next(int period);
const char *cs_market_period_label(int period);
