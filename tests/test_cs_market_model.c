#include <assert.h>
#include <string.h>

#include "cs_market_model.h"

int main(void)
{
    char text[32];

    cs_market_format_price(0, text, sizeof(text));
    assert(strcmp(text, "0.00") == 0);
    cs_market_format_price(91388, text, sizeof(text));
    assert(strcmp(text, "913.88") == 0);
    cs_market_format_price(-120, text, sizeof(text));
    assert(strcmp(text, "-1.20") == 0);
    cs_market_format_price(5, text, sizeof(text));
    assert(strcmp(text, "0.05") == 0);

    assert(cs_market_ratio_to_x100(1.5761) == 158);
    assert(cs_market_ratio_to_x100(-0.3049) == -30);
    assert(cs_market_ratio_to_x100(0.0) == 0);

    cs_market_format_ratio(158, text, sizeof(text));
    assert(strcmp(text, "+1.58%") == 0);
    cs_market_format_ratio(-30, text, sizeof(text));
    assert(strcmp(text, "-0.30%") == 0);
    cs_market_format_ratio(0, text, sizeof(text));
    assert(strcmp(text, "0.00%") == 0);

    bool known = false;
    assert(cs_market_price_diff(100, 90, true, &known) == 10);
    assert(known);
    assert(cs_market_price_diff(100, 0, false, &known) == 0);
    assert(!known);

    // 2023-11-14 22:13:20 UTC = 2023-11-15 06:13 北京时间。
    cs_market_format_time(1700000000LL, true, text, sizeof(text));
    assert(strcmp(text, "11-15 06:13") == 0);
    cs_market_format_time(1700000000LL, false, text, sizeof(text));
    assert(strcmp(text, "06:13") == 0);
    cs_market_format_time(0, true, text, sizeof(text));
    assert(strcmp(text, "--") == 0);

    int32_t values[] = { 0, 90000, 0, 92000, 91000 };
    int32_t min_value = 0;
    int32_t max_value = 0;
    assert(cs_market_chart_bounds(values, 5, &min_value, &max_value));
    assert(min_value < 90000);
    assert(max_value > 92000);
    assert(!cs_market_chart_bounds(values, 0, &min_value, &max_value));

    int32_t flat[] = { 90000, 90000 };
    assert(cs_market_chart_bounds(flat, 2, &min_value, &max_value));
    assert(min_value < 90000 && max_value > 90000);

    assert(cs_market_period_next(1) == 2);
    assert(cs_market_period_next(2) == 3);
    assert(cs_market_period_next(3) == 1);
    assert(strcmp(cs_market_period_label(1), "时K") == 0);
    assert(strcmp(cs_market_period_label(2), "日K") == 0);
    assert(strcmp(cs_market_period_label(3), "周K") == 0);

    cs_market_snapshot_t snapshot;
    memset(&snapshot, 0xff, sizeof(snapshot));
    cs_market_snapshot_clear(&snapshot);
    assert(!snapshot.index_valid);
    assert(snapshot.point_count == 0);
    assert(snapshot.error[0] == 0);

    return 0;
}
