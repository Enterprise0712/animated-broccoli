#include "cs_net.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "cs_net";

#define CS_BASE_URL "https://open.steamdt.com"
#define CS_HTTP_MAX_RESPONSE (48 * 1024)
// 响应缓冲的起始容量。接口实测只返回几百到几千字节,按需增长可把
// 单次请求的堆占用从固定 48KB 降到 4KB 起步,避免接近 C3 的堆上限。
#define CS_HTTP_INITIAL_RESPONSE (4 * 1024)
#define CS_CONFIG_NAMESPACE "cs_config"
#define CS_KEY_SSID "wifi_ssid"
#define CS_KEY_PASS "wifi_pass"
#define CS_KEY_API "api_key"
#define CS_BATCH_MIN_INTERVAL_US (60LL * 1000 * 1000)

static char s_ssid[32];
static char s_password[64];
static char s_api_key[129];

static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static esp_timer_handle_t s_reconnect_timer;
static volatile bool s_connected;
static volatile bool s_connecting;
static volatile bool s_wifi_started;
static volatile bool s_initialized;
static volatile int s_reconnect_attempts;
static bool s_sntp_started;
static int64_t s_last_batch_attempt_us;

static void set_error(char *dst, size_t dst_size, const char *format, ...)
{
    if (!dst || dst_size == 0) return;
    va_list args;
    va_start(args, format);
    vsnprintf(dst, dst_size, format, args);
    va_end(args);
}

static void append_error(char *dst, size_t dst_size, const char *text)
{
    if (!dst || dst_size == 0 || !text || !text[0]) return;
    size_t used = strlen(dst);
    if (used >= dst_size - 1) return;
    if (used > 0) {
        snprintf(dst + used, dst_size - used, " / %s", text);
    } else {
        snprintf(dst, dst_size, "%s", text);
    }
}

static void start_sntp(void)
{
    if (s_sntp_started) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "已启动 SNTP 对时");
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_ssid[0] == 0 || s_connected || !s_wifi_started) return;
    ESP_LOGI(TAG, "重新连接 Wi-Fi(%s)", s_ssid);
    s_connecting = true;
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    if (!s_reconnect_timer || s_ssid[0] == 0) return;
    int attempt = ++s_reconnect_attempts;
    uint64_t delay_us = attempt <= 5 ? 5000000ULL : 30000000ULL;
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, delay_us);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_START) {
            if (s_ssid[0] != 0) {
                s_connecting = true;
                esp_wifi_connect();
            }
        } else if (id == WIFI_EVENT_STA_CONNECTED) {
            s_connecting = true;
            ESP_LOGI(TAG, "Wi-Fi 已关联,等待获取 IP");
        } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
            const wifi_event_sta_disconnected_t *event = data;
            s_connected = false;
            s_connecting = false;
            ESP_LOGW(TAG, "Wi-Fi 断开,reason=%d", event ? event->reason : -1);
            schedule_reconnect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_connected = true;
        s_connecting = false;
        s_reconnect_attempts = 0;
        if (s_reconnect_timer) esp_timer_stop(s_reconnect_timer);
        ESP_LOGI(TAG, "已获取 IP");
        start_sntp();
    }
}

static esp_err_t load_string(nvs_handle_t handle, const char *key, char *dst,
                             size_t dst_size)
{
    size_t length = dst_size;
    esp_err_t err = nvs_get_str(handle, key, dst, &length);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        dst[0] = 0;
        return ESP_OK;
    }
    return err;
}

static esp_err_t save_string(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(CS_CONFIG_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    load_string(handle, CS_KEY_SSID, s_ssid, sizeof(s_ssid));
    load_string(handle, CS_KEY_PASS, s_password, sizeof(s_password));
    load_string(handle, CS_KEY_API, s_api_key, sizeof(s_api_key));
    nvs_close(handle);
    if (s_ssid[0] != 0) ESP_LOGI(TAG, "已载入 Wi-Fi 配置(%s)", s_ssid);
    if (s_api_key[0] != 0) ESP_LOGI(TAG, "已载入 API Key");
}

esp_err_t cs_net_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 不自动擦除:该分区可能保存着用户的 Wi-Fi/Key 配置。
        ESP_LOGE(TAG, "NVS 初始化失败(%s),未自动擦除分区", esp_err_to_name(err));
        return err;
    }
    if (err != ESP_OK) return err;

    err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_WIFI_STA();
    s_sta_netif = esp_netif_new(&netif_config);
    if (!s_sta_netif) return ESP_ERR_NO_MEM;
    err = esp_netif_attach_wifi_station(s_sta_netif);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_default_wifi_sta_handlers();
    if (err != ESP_OK) return err;

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_config);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event_handler, NULL,
                                              &s_wifi_handler);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              wifi_event_handler, NULL, &s_ip_handler);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;

    load_config();
    if (s_ssid[0] != 0) {
        wifi_config_t station = { 0 };
        snprintf((char *)station.sta.ssid, sizeof(station.sta.ssid), "%s", s_ssid);
        snprintf((char *)station.sta.password, sizeof(station.sta.password), "%s",
                 s_password);
        err = esp_wifi_set_config(WIFI_IF_STA, &station);
        if (err != ESP_OK) return err;
    }

    esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_cb,
        .name = "cs_reconnect",
    };
    if (esp_timer_create(&timer_args, &s_reconnect_timer) != ESP_OK) {
        s_reconnect_timer = NULL;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    s_wifi_started = true;
    s_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi 已启动%s", s_ssid[0] ? ",开始连接" : "(未配置)");
    return ESP_OK;
}

bool cs_net_has_credentials(void)
{
    return s_ssid[0] != 0;
}

bool cs_net_has_api_key(void)
{
    return s_api_key[0] != 0;
}

esp_err_t cs_net_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) >= sizeof(s_ssid)) return ESP_ERR_INVALID_ARG;
    if (password && strlen(password) >= sizeof(s_password)) return ESP_ERR_INVALID_ARG;

    esp_err_t err = save_string(CS_KEY_SSID, ssid);
    if (err != ESP_OK) return err;
    err = save_string(CS_KEY_PASS, password ? password : "");
    if (err != ESP_OK) return err;

    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_password, sizeof(s_password), "%s", password ? password : "");
    s_reconnect_attempts = 0;

    if (!s_wifi_started) return ESP_OK;
    esp_wifi_disconnect();
    wifi_config_t station = { 0 };
    snprintf((char *)station.sta.ssid, sizeof(station.sta.ssid), "%s", s_ssid);
    snprintf((char *)station.sta.password, sizeof(station.sta.password), "%s",
             s_password);
    err = esp_wifi_set_config(WIFI_IF_STA, &station);
    if (err != ESP_OK) return err;
    s_connecting = s_ssid[0] != 0;
    if (s_ssid[0] != 0) esp_wifi_connect();
    return ESP_OK;
}

esp_err_t cs_net_set_api_key(const char *api_key)
{
    if (!api_key || strlen(api_key) >= sizeof(s_api_key)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = save_string(CS_KEY_API, api_key);
    if (err != ESP_OK) return err;
    snprintf(s_api_key, sizeof(s_api_key), "%s", api_key);
    return ESP_OK;
}

esp_err_t cs_net_clear_config(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);

    s_ssid[0] = 0;
    s_password[0] = 0;
    s_api_key[0] = 0;
    if (s_wifi_started) esp_wifi_disconnect();
    return err;
}

void cs_net_status(cs_net_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->configured = s_ssid[0] != 0;
    out->connected = s_connected;
    out->connecting = s_connecting;
    out->has_key = s_api_key[0] != 0;
    out->rssi = -127;
    if (out->configured) snprintf(out->ssid, sizeof(out->ssid), "%s", s_ssid);

    size_t key_length = strlen(s_api_key);
    if (key_length > 4) {
        snprintf(out->key_tail, sizeof(out->key_tail), "%s", s_api_key + key_length - 4);
    }

    if (s_sta_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            snprintf(out->ip, sizeof(out->ip), IPSTR, IP2STR(&ip_info.ip));
        }
    }
    if (s_connected) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) out->rssi = ap.rssi;
    }
}

static const char *http_status_text(int status)
{
    if (status == 401 || status == 403) return "API Key 无效或已失效";
    if (status == 429) return "请求频率超限，请稍后再试";
    if (status >= 500) return "行情服务暂时不可用";
    return NULL;
}

static esp_err_t http_request(esp_http_client_method_t method, const char *path,
                              const char *body, char **response, size_t *response_len,
                              char *error, size_t error_size)
{
    *response = NULL;
    *response_len = 0;
    char url[160];
    snprintf(url, sizeof(url), "%s%s", CS_BASE_URL, path);

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        set_error(error, error_size, "HTTP 客户端创建失败");
        return ESP_ERR_NO_MEM;
    }

    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);
    esp_http_client_set_header(client, "Authorization", auth);
    if (body) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    int write_length = body ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(client, write_length);
    if (err != ESP_OK) {
        set_error(error, error_size, "连接行情服务失败");
        esp_http_client_cleanup(client);
        return err;
    }
    if (body) {
        int written = esp_http_client_write(client, body, write_length);
        if (written != write_length) {
            set_error(error, error_size, "请求发送失败");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
    }

    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %s status=%d content_len=%lld free_heap=%u",
             path, status, (long long)content_length,
             (unsigned)esp_get_free_heap_size());
    if (status != 200) {
        const char *text = http_status_text(status);
        if (text) set_error(error, error_size, "%s", text);
        else set_error(error, error_size, "HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (content_length > CS_HTTP_MAX_RESPONSE) {
        set_error(error, error_size, "响应过大");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    // 按实际长度分配;content_length 未知(chunked 常返回 0)时从小块开始按需增长,
    // 避免为几百字节的响应预留 48KB,把峰值堆占用压到 TLS 缓冲区之外。
    size_t capacity = CS_HTTP_INITIAL_RESPONSE;
    if (content_length > 0 && (size_t)content_length + 1 > capacity) {
        capacity = (size_t)content_length + 1;
    }
    char *buffer = malloc(capacity);
    if (!buffer) {
        set_error(error, error_size, "内存不足");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }
    size_t used = 0;
    while (used < CS_HTTP_MAX_RESPONSE) {
        if (used + 1 >= capacity) {
            size_t grown = capacity * 2;
            if (grown > CS_HTTP_MAX_RESPONSE + 1) grown = CS_HTTP_MAX_RESPONSE + 1;
            if (grown <= capacity) break;
            char *next = realloc(buffer, grown);
            if (!next) {
                free(buffer);
                set_error(error, error_size, "内存不足");
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            buffer = next;
            capacity = grown;
        }
        int read = esp_http_client_read(client, buffer + used,
                                        (int)(capacity - used - 1));
        if (read <= 0) break;
        used += (size_t)read;
    }
    buffer[used] = 0;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    ESP_LOGI(TAG, "HTTP %s read=%zu free_heap=%u", path, used,
             (unsigned)esp_get_free_heap_size());

    if (used == 0) {
        free(buffer);
        set_error(error, error_size, "服务返回空响应");
        return ESP_ERR_INVALID_RESPONSE;
    }
    *response = buffer;
    *response_len = used;
    return ESP_OK;
}

static bool json_number(cJSON *object, const char *key, double *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valuedouble;
    return true;
}

static bool json_int64(cJSON *object, const char *key, int64_t *out)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = (int64_t)item->valuedouble;
    return true;
}

static int64_t normalize_time(int64_t value)
{
    if (value > 100000000000LL) return value / 1000;
    return value;
}

static void json_error_text(cJSON *root, char *error, size_t error_size)
{
    cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "errorCode");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "errorMsg");
    if (cJSON_IsNumber(code) && (int)code->valuedouble == 4002) {
        set_error(error, error_size, "API Key 未配置或无效");
    } else if (cJSON_IsNumber(code) && (int)code->valuedouble == 4005) {
        set_error(error, error_size, "接口请求超限");
    } else if (cJSON_IsString(message) && message->valuestring &&
               message->valuestring[0]) {
        set_error(error, error_size, "接口错误: %s", message->valuestring);
    } else {
        set_error(error, error_size, "接口返回失败");
    }
}

static bool is_rate_limit_error(const char *error)
{
    return error && error[0] &&
           (strstr(error, "超限") || strstr(error, "上限") ||
            strstr(error, "限制") || strstr(error, "频率"));
}

static cJSON *parse_response(char *body, char *error, size_t error_size)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        size_t len = strlen(body);
        char preview[81] = { 0 };
        size_t copy = len < sizeof(preview) - 1 ? len : sizeof(preview) - 1;
        memcpy(preview, body, copy);
        ESP_LOGW(TAG, "JSON 解析失败, body_len=%zu free_heap=%u preview=%.80s",
                 len, (unsigned)esp_get_free_heap_size(), preview);
        set_error(error, error_size, "行情数据解析失败");
        return NULL;
    }
    cJSON *success = cJSON_GetObjectItemCaseSensitive(root, "success");
    if (!cJSON_IsTrue(success)) {
        json_error_text(root, error, error_size);
        cJSON_Delete(root);
        return NULL;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!data) {
        set_error(error, error_size, "响应缺少数据字段");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static esp_err_t fetch_index(cs_market_snapshot_t *snapshot, int period, char *error,
                             size_t error_size)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_request(HTTP_METHOD_GET,
                                 "/open/cs2/broad/v1/index", NULL, &body, &body_len,
                                 error, error_size);
    if (err != ESP_OK) return err;

    cJSON *root = parse_response(body, error, error_size);
    if (!root) {
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    double index_value = 0;
    double diff_value = 0;
    double ratio_value = 0;
    int64_t update_time = 0;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!json_number(data, "broadMarketIndex", &index_value)) {
        set_error(error, error_size, "缺少大盘指数");
        cJSON_Delete(root);
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }
    json_number(data, "diffYesterday", &diff_value);
    json_number(data, "diffYesterdayRatio", &ratio_value);
    json_int64(data, "updateTime", &update_time);

    snapshot->index_centi = (int32_t)llround(index_value * 100.0);
    snapshot->diff_centi = (int32_t)llround(diff_value * 100.0);
    snapshot->ratio_x100 = cs_market_ratio_to_x100(ratio_value);
    snapshot->index_update_time = update_time > 0 ? normalize_time(update_time)
                                                  : (int64_t)time(NULL);
    snapshot->index_valid = true;
    snapshot->index_from_cache = false;

    // 用指数接口自带的小时历史走势填充图表(在 ESP32-C3 无 PSRAM 上稳定可用)。
    cJSON *history = cJSON_GetObjectItemCaseSensitive(data, "historyMarketIndexList");
    if (cJSON_IsArray(history)) {
        int total = cJSON_GetArraySize(history);
        int start = total > CS_MARKET_POINTS_MAX ? total - CS_MARKET_POINTS_MAX : 0;
        int count = 0;
        for (int i = start; i < total; i++) {
            cJSON *row = cJSON_GetArrayItem(history, i);
            if (!cJSON_IsArray(row) || cJSON_GetArraySize(row) < 2) continue;
            cJSON *t = cJSON_GetArrayItem(row, 0);
            cJSON *v = cJSON_GetArrayItem(row, 1);
            if (!cJSON_IsNumber(t) || !cJSON_IsNumber(v)) continue;
            snapshot->point_time[count] = normalize_time((int64_t)t->valuedouble);
            snapshot->point_value_centi[count] = (int32_t)llround(v->valuedouble * 100.0);
            count++;
            if (count >= CS_MARKET_POINTS_MAX) break;
        }
        if (count > 0) {
            snapshot->point_count = count;
            snapshot->period = period;
        }
    }

    cJSON_Delete(root);
    free(body);
    return ESP_OK;
}

static bool pick_price(cJSON *item, double *price, int64_t *update_time)
{
    cJSON *list = cJSON_GetObjectItemCaseSensitive(item, "dataList");
    if (!cJSON_IsArray(list)) return false;
    cJSON *fallback = NULL;
    int count = cJSON_GetArraySize(list);
    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_GetArrayItem(list, i);
        cJSON *platform = cJSON_GetObjectItemCaseSensitive(entry, "platform");
        cJSON *sell = cJSON_GetObjectItemCaseSensitive(entry, "sellPrice");
        if (!cJSON_IsNumber(sell) || sell->valuedouble <= 0) continue;
        if (cJSON_IsString(platform) && platform->valuestring &&
            strcmp(platform->valuestring, "BUFF") == 0) {
            fallback = entry;
            break;
        }
        if (!fallback) fallback = entry;
    }
    if (!fallback) return false;
    cJSON *sell = cJSON_GetObjectItemCaseSensitive(fallback, "sellPrice");
    *price = sell->valuedouble;
    cJSON *time_item = cJSON_GetObjectItemCaseSensitive(fallback, "updateTime");
    *update_time = cJSON_IsNumber(time_item) ? normalize_time((int64_t)time_item->valuedouble)
                                             : 0;
    return true;
}

static cJSON *find_quote(cJSON *data, const char *market_hash_name)
{
    int count = cJSON_GetArraySize(data);
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(data, i);
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "marketHashName");
        if (cJSON_IsString(name) && name->valuestring &&
            strcmp(name->valuestring, market_hash_name) == 0) {
            return item;
        }
    }
    return NULL;
}

static esp_err_t fetch_prices(cs_market_snapshot_t *snapshot, char *error,
                              size_t error_size)
{
    int64_t now_us = esp_timer_get_time();
    if (s_last_batch_attempt_us > 0 &&
        now_us - s_last_batch_attempt_us < CS_BATCH_MIN_INTERVAL_US) {
        set_error(error, error_size, "价格接口限流，先显示上次报价");
        return ESP_OK;
    }
    s_last_batch_attempt_us = now_us;

    cJSON *request = cJSON_CreateObject();
    if (!request) {
        set_error(error, error_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    cJSON *names = cJSON_AddArrayToObject(request, "marketHashNames");
    if (!names) {
        cJSON_Delete(request);
        set_error(error, error_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < CS_MARKET_WATCH_COUNT; i++) {
        cJSON_AddItemToArray(names, cJSON_CreateString(CS_MARKET_WATCH_DEFS[i].market_hash_name));
    }
    char *request_body = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    if (!request_body) {
        set_error(error, error_size, "内存不足");
        return ESP_ERR_NO_MEM;
    }

    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = http_request(HTTP_METHOD_POST,
                                 "/open/cs2/v1/price/batch", request_body, &body,
                                 &body_len, error, error_size);
    cJSON_free(request_body);
    if (err != ESP_OK) return err;

    cJSON *root = parse_response(body, error, error_size);
    if (!root) {
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (!cJSON_IsArray(data)) {
        set_error(error, error_size, "价格数据格式异常");
        cJSON_Delete(root);
        free(body);
        return ESP_ERR_INVALID_RESPONSE;
    }

    int matched = 0;
    int missing = 0;
    for (int i = 0; i < CS_MARKET_WATCH_COUNT; i++) {
        cJSON *item = find_quote(data, CS_MARKET_WATCH_DEFS[i].market_hash_name);
        double price = 0;
        int64_t update_time = 0;
        if (!item || !pick_price(item, &price, &update_time)) {
            missing++;
            continue;
        }
        cs_watch_quote_t *quote = &snapshot->watch[i];
        quote->has_prev = quote->valid && quote->price_cents > 0;
        quote->prev_price_cents = quote->has_prev ? quote->price_cents : 0;
        quote->price_cents = (int32_t)llround(price * 100.0);
        quote->update_time = update_time;
        quote->valid = quote->price_cents > 0;
        matched++;
    }

    cJSON_Delete(root);
    free(body);

    if (matched == 0) {
        set_error(error, error_size, "自选价格暂无数据");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (missing > 0) {
        set_error(error, error_size, "部分饰品无报价");
    }
    return ESP_OK;
}

esp_err_t cs_net_fetch(cs_market_snapshot_t *snapshot, int period)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    snapshot->error[0] = 0;

    cs_net_status_t status;
    cs_net_status(&status);
    if (!status.connected) {
        set_error(snapshot->error, sizeof(snapshot->error), "网络未连接");
        return ESP_ERR_INVALID_STATE;
    }
    if (!status.has_key) {
        set_error(snapshot->error, sizeof(snapshot->error), "API Key 未配置");
        return ESP_ERR_INVALID_STATE;
    }

    char combined[CS_MARKET_ERROR_MAX] = { 0 };
    char part[CS_MARKET_ERROR_MAX] = { 0 };
    bool rate_limited = false;
    bool index_ok = false;

    esp_err_t err = fetch_index(snapshot, period, part, sizeof(part));
    if (err == ESP_OK) {
        index_ok = true;
    } else if (is_rate_limit_error(part)) {
        // 速率限制:保留缓存指数,继续请求价格
        rate_limited = true;
        ESP_LOGW(TAG, "指数接口限流,使用缓存数据");
    } else {
        snprintf(snapshot->error, sizeof(snapshot->error), "%s",
                 part[0] ? part : "大盘指数更新失败");
        return err;
    }

    part[0] = 0;
    esp_err_t prices_err = fetch_prices(snapshot, part, sizeof(part));
    if (prices_err == ESP_OK) {
        // fetch_prices 在本地限流、或部分饰品无报价时仍返回 ESP_OK,但会写入 part。
        // 这些提示必须传给界面,否则旧报价会被显示成刚更新的数据。
        if (part[0]) {
            append_error(combined, sizeof(combined), part);
        }
    } else if (is_rate_limit_error(part)) {
        rate_limited = true;
        ESP_LOGW(TAG, "价格接口限流,使用缓存数据");
    } else if (part[0]) {
        append_error(combined, sizeof(combined), part);
    } else {
        append_error(combined, sizeof(combined), "自选价格更新失败");
    }

    if (combined[0]) {
        // 有实质提示(含限流、部分缺失):展示出来,并标注数据可能来自缓存。
        snprintf(snapshot->error, sizeof(snapshot->error), "%s", combined);
        snapshot->index_from_cache = snapshot->index_valid;
    } else if (rate_limited && snapshot->index_valid) {
        // 纯限流且有缓存:不显示错误,标注为缓存
        snapshot->error[0] = 0;
        snapshot->index_from_cache = true;
    } else {
        snapshot->error[0] = 0;
        snapshot->index_from_cache = false;
    }
    snapshot->refreshed_at = (int64_t)time(NULL);
    return (index_ok || rate_limited) ? ESP_OK : ESP_FAIL;
}
