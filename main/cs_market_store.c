#include "cs_market_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "cs_store";

#define CS_STORE_NAMESPACE "cs_market"
#define CS_STORE_KEY "snap"
#define CS_STORE_MAGIC 0x43534d31u // "CSM1"

typedef struct {
    uint32_t magic;
    uint32_t size;
    cs_market_snapshot_t snapshot;
} cs_store_blob_t;

esp_err_t cs_store_load(cs_market_snapshot_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CS_STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    cs_store_blob_t *blob = NULL;
    size_t size = 0;
    err = nvs_get_blob(handle, CS_STORE_KEY, NULL, &size);
    if (err == ESP_OK && size == sizeof(cs_store_blob_t)) {
        blob = malloc(size);
    }
    if (blob) {
        err = nvs_get_blob(handle, CS_STORE_KEY, blob, &size);
        if (err == ESP_OK && blob->magic == CS_STORE_MAGIC &&
            blob->size == sizeof(cs_market_snapshot_t)) {
            *out = blob->snapshot;
            out->refreshing = false;
        } else {
            err = ESP_ERR_INVALID_VERSION;
        }
        free(blob);
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_SIZE;
    }
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGI(TAG, "没有可用缓存: %s", esp_err_to_name(err));
    return err;
}

esp_err_t cs_store_save(const cs_market_snapshot_t *snapshot)
{
    if (!snapshot) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CS_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    cs_store_blob_t *blob = malloc(sizeof(cs_store_blob_t));
    if (!blob) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }
    blob->magic = CS_STORE_MAGIC;
    blob->size = sizeof(cs_market_snapshot_t);
    blob->snapshot = *snapshot;
    blob->snapshot.refreshing = false;

    err = nvs_set_blob(handle, CS_STORE_KEY, blob, sizeof(cs_store_blob_t));
    if (err == ESP_OK) err = nvs_commit(handle);
    free(blob);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGW(TAG, "缓存写入失败: %s", esp_err_to_name(err));
    return err;
}

esp_err_t cs_store_clear(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CS_STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
