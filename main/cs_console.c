#include "cs_console.h"

#include "cs_app.h"
#include "cs_market_store.h"
#include "cs_net.h"
#include "esp_console.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "cs_console";

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("用法: wifi <SSID> <密码>  (开放网络可省略密码)\n");
        return 1;
    }
    esp_err_t err = cs_net_set_credentials(argv[1], argc == 3 ? argv[2] : "");
    if (err != ESP_OK) {
        printf("保存失败: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("已保存 Wi-Fi 配置(%s),正在连接\n", argv[1]);
    return 0;
}

// SSID 含中文等非 ASCII 字符时,串口行编辑器会丢弃高位字节,
// 改用 SSID 的 UTF-8 十六进制编码传入,例如 wifihex e68891e59ca8 123456。
static int cmd_wifihex(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("用法: wifihex <SSID十六进制> <密码>\n");
        return 1;
    }
    size_t hex_len = strlen(argv[1]);
    if (hex_len == 0 || (hex_len % 2) != 0 || hex_len > 62) {
        printf("SSID 十六进制长度必须是 1~31 字节的偶数\n");
        return 1;
    }
    char ssid[32];
    for (size_t i = 0; i < hex_len; i += 2) {
        int hi = hex_nibble(argv[1][i]);
        int lo = hex_nibble(argv[1][i + 1]);
        if (hi < 0 || lo < 0) {
            printf("SSID 十六进制含非法字符\n");
            return 1;
        }
        ssid[i / 2] = (char)((hi << 4) | lo);
    }
    ssid[hex_len / 2] = 0;
    esp_err_t err = cs_net_set_credentials(ssid, argc == 3 ? argv[2] : "");
    if (err != ESP_OK) {
        printf("保存失败: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("已保存 Wi-Fi 配置(%s),正在连接\n", ssid);
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    if (argc != 2) {
        printf("用法: key <API_KEY>\n");
        return 1;
    }
    esp_err_t err = cs_net_set_api_key(argv[1]);
    if (err != ESP_OK) {
        printf("保存失败: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("已保存 API Key(末四位 ****%s)\n",
           strlen(argv[1]) > 4 ? argv[1] + strlen(argv[1]) - 4 : "");
    cs_app_request_refresh();
    return 0;
}

static int cmd_refresh(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    cs_app_request_refresh();
    printf("已请求刷新\n");
    return 0;
}

static int cmd_status(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    cs_net_status_t net;
    cs_net_status(&net);
    cs_market_snapshot_t snapshot;
    cs_app_get_snapshot(&snapshot);

    printf("Wi-Fi: %s\n", !net.configured ? "未配置"
                            : net.connected ? "已连接"
                            : net.connecting ? "连接中" : "未连接");
    if (net.configured) {
        printf("  SSID: %s\n", net.ssid);
        if (net.connected) printf("  IP: %s  RSSI: %d dBm\n", net.ip, (int)net.rssi);
    }
    if (net.has_key) {
        printf("API Key: 已配置(末四位 ****%s)\n", net.key_tail);
    } else {
        printf("API Key: 未配置\n");
    }

    if (snapshot.index_valid) {
        char value[24];
        char ratio[24];
        cs_market_format_price(snapshot.index_centi, value, sizeof(value));
        cs_market_format_ratio(snapshot.ratio_x100, ratio, sizeof(ratio));
        printf("大盘指数: %s (%s)%s\n", value, ratio,
               snapshot.index_from_cache ? " [缓存]" : "");
    } else {
        printf("大盘指数: 暂无数据\n");
    }

    int priced = 0;
    for (int i = 0; i < CS_MARKET_WATCH_COUNT; i++) {
        if (snapshot.watch[i].valid) priced++;
    }
    printf("自选报价: %d/%d\n", priced, CS_MARKET_WATCH_COUNT);
    if (snapshot.error[0]) printf("最近错误: %s\n", snapshot.error);
    if (snapshot.refreshing) printf("状态: 正在刷新\n");
    return 0;
}

static int cmd_reset(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t net_err = cs_net_clear_config();
    esp_err_t store_err = cs_store_clear();
    printf("已清除 Wi-Fi、API Key 与本地缓存(%s / %s)\n",
           esp_err_to_name(net_err), esp_err_to_name(store_err));
    return net_err == ESP_OK && store_err == ESP_OK ? 0 : 1;
}

static esp_err_t start_repl(void)
{
    static esp_console_repl_t *repl;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "cs>";
    repl_config.max_cmdline_length = 160;

    esp_console_dev_usb_serial_jtag_config_t hw_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();

    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
    if (err != ESP_OK) return err;

    esp_console_register_help_command();

    static const esp_console_cmd_t commands[] = {
        { .command = "wifi", .help = "配置 Wi-Fi: wifi <SSID> <密码>",
          .hint = "<SSID> <密码>", .func = cmd_wifi },
        { .command = "wifihex", .help = "中文 SSID 用十六进制配置: wifihex <HEX> <密码>",
          .hint = "<SSID_HEX> <密码>", .func = cmd_wifihex },
        { .command = "key", .help = "配置 SteamDT API Key: key <密钥>",
          .hint = "<API_KEY>", .func = cmd_key },
        { .command = "refresh", .help = "立即刷新行情", .func = cmd_refresh },
        { .command = "status", .help = "查看网络与行情状态", .func = cmd_status },
        { .command = "reset", .help = "清除 Wi-Fi、Key 与缓存", .func = cmd_reset },
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        esp_console_cmd_register(&commands[i]);
    }

    err = esp_console_start_repl(repl);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "USB 串口命令已就绪");
    return ESP_OK;
}

esp_err_t cs_console_init(void)
{
    esp_err_t err = start_repl();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "串口命令启动失败: %s", esp_err_to_name(err));
    }
    return err;
}
