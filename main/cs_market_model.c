#include "cs_market_model.h"

#include <stdio.h>
#include <string.h>

const cs_watch_def_t CS_MARKET_WATCH_DEFS[CS_MARKET_WATCH_COUNT] = {
    { "AK 红线", "AK-47 | Redline (Field-Tested)" },
    { "AWP 二西莫夫", "AWP | Asiimov (Field-Tested)" },
    { "M4 印花集", "M4A1-S | Printstream (Field-Tested)" },
    { "蝴蝶刀 渐变", "★ Butterfly Knife | Fade (Factory New)" },
    { "沙鹰 烈焰", "Desert Eagle | Blaze (Factory New)" },
};

void cs_market_snapshot_clear(cs_market_snapshot_t *snapshot)
{
    if (snapshot) memset(snapshot, 0, sizeof(*snapshot));
}

int32_t cs_market_ratio_to_x100(double ratio_percent)
{
    double scaled = ratio_percent * 100.0;
    if (scaled >= 0.0) return (int32_t)(scaled + 0.5);
    return (int32_t)(scaled - 0.5);
}

int32_t cs_market_ratio_from_x100(int32_t ratio_x100)
{
    return ratio_x100;
}

int32_t cs_market_price_diff(int32_t new_cents, int32_t old_cents, bool old_valid,
                             bool *known)
{
    if (known) *known = old_valid && old_cents > 0 && new_cents > 0;
    if (!known || !*known) return 0;
    return new_cents - old_cents;
}

void cs_market_format_price(int32_t cents, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    int32_t value = cents;
    const char *sign = "";
    if (value < 0) {
        sign = "-";
        value = -value;
    }
    snprintf(out, out_size, "%s%ld.%02ld", sign, (long)(value / 100),
             (long)(value % 100));
}

void cs_market_format_ratio(int32_t ratio_x100, char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    int32_t value = ratio_x100;
    const char *sign = "";
    if (value > 0) sign = "+";
    else if (value < 0) {
        sign = "-";
        value = -value;
    }
    snprintf(out, out_size, "%s%ld.%02ld%%", sign, (long)(value / 100),
             (long)(value % 100));
}

// 自行换算北京时间,不依赖目标系统的时区数据库(localtime 在部分主机平台不可用)。
static void civil_from_days(int64_t days, int *year, int *month, int *day)
{
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int y = (int)yoe + (int)(era * 400);
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    *year = y + (m <= 2);
    *month = (int)m;
    *day = (int)d;
}

void cs_market_format_time(int64_t unix_seconds, bool with_date, char *out,
                           size_t out_size)
{
    if (!out || out_size == 0) return;
    if (unix_seconds <= 0) {
        snprintf(out, out_size, "--");
        return;
    }
    int64_t beijing = unix_seconds + 8 * 3600;
    int64_t days = beijing / 86400;
    int seconds_of_day = (int)(beijing % 86400);
    int year = 0;
    int month = 0;
    int day = 0;
    civil_from_days(days, &year, &month, &day);
    int hour = seconds_of_day / 3600;
    int minute = (seconds_of_day % 3600) / 60;
    if (with_date) {
        snprintf(out, out_size, "%02d-%02d %02d:%02d", month, day, hour, minute);
    } else {
        snprintf(out, out_size, "%02d:%02d", hour, minute);
    }
}

bool cs_market_chart_bounds(const int32_t *values, int count, int32_t *min_value,
                            int32_t *max_value)
{
    if (!values || count <= 0 || !min_value || !max_value) return false;
    int32_t lo = INT32_MAX;
    int32_t hi = INT32_MIN;
    for (int i = 0; i < count; i++) {
        if (values[i] <= 0) continue;
        if (values[i] < lo) lo = values[i];
        if (values[i] > hi) hi = values[i];
    }
    if (lo == INT32_MAX) return false;
    int32_t padding;
    if (hi == lo) {
        padding = lo / 100 + 1;
    } else {
        padding = (hi - lo) / 20 + 1;
    }
    *min_value = lo - padding;
    *max_value = hi + padding;
    return true;
}

int cs_market_period_next(int period)
{
    if (period == 1) return 2;
    if (period == 2) return 3;
    return 1;
}

const char *cs_market_period_label(int period)
{
    if (period == 1) return "时K";
    if (period == 3) return "周K";
    return "日K";
}
