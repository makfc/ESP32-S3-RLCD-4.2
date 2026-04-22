#include "weather_service.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define TAG "weather"
#define WEATHER_URL "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=tc"
#define WEATHER_PLACE_WTS "黃大仙"
#define BUF_INIT_SIZE (8 * 1024)
#define BUF_MAX_SIZE  (128 * 1024)

static char *s_buf = NULL;
static int s_buf_len = 0;
static int s_buf_cap = 0;
static bool s_buf_truncated = false;

static int s_temp_wts = INT_MIN;
static int s_humidity = -1;
static int s_icon_code = -1;
static const char *s_icon_desc = "天氣";
static bool s_has_valid = false;

typedef struct {
    int code;
    const char *desc;
} weather_icon_desc_t;

static const weather_icon_desc_t k_icon_desc_table[] = {
    {50, "陽光充沛"},
    {51, "間有陽光"},
    {52, "短暫陽光"},
    {53, "幾陣驟雨"},
    {54, "有驟雨"},
    {60, "多雲"},
    {61, "密雲"},
    {62, "微雨"},
    {63, "雨"},
    {64, "大雨"},
    {65, "雷暴"},
    /* 70~77 descriptions without parenthesis notes */
    {70, "天色良好"},
    {71, "天色良好"},
    {72, "天色良好"},
    {73, "天色良好"},
    {74, "天色良好"},
    {75, "天色良好"},
    {76, "大致多雲"},
    {77, "天色大致良好"},
    {80, "大風"},
    {81, "乾燥"},
    {82, "潮濕"},
    {83, "霧"},
    {84, "薄霧"},
    {85, "煙霞"},
    {90, "熱"},
    {91, "暖"},
    {92, "涼"},
    {93, "冷"},
};

static const char *icon_desc_from_code(int code)
{
    for (size_t i = 0; i < (sizeof(k_icon_desc_table) / sizeof(k_icon_desc_table[0])); i++) {
        if (k_icon_desc_table[i].code == code) {
            return k_icon_desc_table[i].desc;
        }
    }
    return "天氣";
}

static bool ensure_buf_capacity(int need_len)
{
    if (need_len <= s_buf_cap) {
        return true;
    }

    int new_cap = (s_buf_cap > 0) ? s_buf_cap : BUF_INIT_SIZE;
    while (new_cap < need_len && new_cap < BUF_MAX_SIZE) {
        new_cap *= 2;
    }
    if (new_cap < need_len) {
        return false;
    }

    char *new_buf = (char *)heap_caps_realloc(s_buf, new_cap, MALLOC_CAP_SPIRAM);
    if (new_buf == NULL) {
        new_buf = (char *)realloc(s_buf, new_cap);
    }
    if (new_buf == NULL) {
        return false;
    }
    s_buf = new_buf;
    s_buf_cap = new_cap;
    return true;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if ((evt->event_id == HTTP_EVENT_ON_DATA) && (evt->data_len > 0) && (s_buf != NULL)) {
        const int need_len = s_buf_len + evt->data_len + 1;
        if (ensure_buf_capacity(need_len)) {
            memcpy(s_buf + s_buf_len, evt->data, evt->data_len);
            s_buf_len += evt->data_len;
        } else {
            s_buf_truncated = true;
        }
    }
    return ESP_OK;
}

static bool parse_weather_json(const char *json_text)
{
    if (json_text == NULL) {
        return false;
    }

    int parsed_temp = INT_MIN;
    int parsed_humidity = -1;
    int parsed_icon = -1;

    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON parse failed");
        return false;
    }

    cJSON *temp = cJSON_GetObjectItemCaseSensitive(root, "temperature");
    cJSON *temp_data = temp ? cJSON_GetObjectItemCaseSensitive(temp, "data") : NULL;
    if (cJSON_IsArray(temp_data)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, temp_data) {
            cJSON *place = cJSON_GetObjectItemCaseSensitive(item, "place");
            cJSON *value = cJSON_GetObjectItemCaseSensitive(item, "value");
            if (cJSON_IsString(place) && cJSON_IsNumber(value) && (place->valuestring != NULL)) {
                if (strcmp(place->valuestring, WEATHER_PLACE_WTS) == 0) {
                    parsed_temp = value->valueint;
                    break;
                }
            }
        }
    }

    cJSON *hum = cJSON_GetObjectItemCaseSensitive(root, "humidity");
    cJSON *hum_data = hum ? cJSON_GetObjectItemCaseSensitive(hum, "data") : NULL;
    if (cJSON_IsArray(hum_data)) {
        cJSON *first = cJSON_GetArrayItem(hum_data, 0);
        cJSON *value = first ? cJSON_GetObjectItemCaseSensitive(first, "value") : NULL;
        if (cJSON_IsNumber(value)) {
            parsed_humidity = value->valueint;
        }
    }

    cJSON *icon = cJSON_GetObjectItemCaseSensitive(root, "icon");
    if (cJSON_IsArray(icon)) {
        cJSON *first_icon = cJSON_GetArrayItem(icon, 0);
        if (cJSON_IsNumber(first_icon)) {
            parsed_icon = first_icon->valueint;
        }
    }

    cJSON_Delete(root);

    if (parsed_temp == INT_MIN) {
        ESP_LOGW(TAG, "Missing Wong Tai Sin temperature in API response");
        return false;
    }
    if (parsed_humidity < 0) {
        ESP_LOGW(TAG, "Missing humidity in API response");
        return false;
    }

    s_temp_wts = parsed_temp;
    s_humidity = parsed_humidity;
    s_icon_code = parsed_icon;
    s_icon_desc = icon_desc_from_code(parsed_icon);
    s_has_valid = true;

    ESP_LOGI(
        TAG,
        "Parsed weather: 黃大仙=%dC humidity=%d%% icon=%d desc=%s",
        s_temp_wts,
        s_humidity,
        s_icon_code,
        s_icon_desc
    );
    return true;
}

bool weather_service_fetch(void)
{
    if (s_buf != NULL) {
        free(s_buf);
        s_buf = NULL;
    }

    s_buf_len = 0;
    s_buf_cap = 0;
    s_buf_truncated = false;

    s_buf = (char *)heap_caps_malloc(BUF_INIT_SIZE, MALLOC_CAP_SPIRAM);
    if (s_buf == NULL) {
        s_buf = (char *)malloc(BUF_INIT_SIZE);
    }
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "Buffer alloc failed");
        return false;
    }
    memset(s_buf, 0, BUF_INIT_SIZE);
    s_buf_cap = BUF_INIT_SIZE;

    esp_http_client_config_t cfg = {
        .url = WEATHER_URL,
        .event_handler = http_event_cb,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        free(s_buf);
        s_buf = NULL;
        s_buf_cap = 0;
        ESP_LOGE(TAG, "esp_http_client_init failed");
        return false;
    }
    /* Keep payload plain text JSON so parser can consume it directly. */
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    esp_err_t err = esp_http_client_perform(client);
    bool ok = false;
    if (err == ESP_OK) {
        const int status = esp_http_client_get_status_code(client);
        s_buf[s_buf_len] = '\0';
        ESP_LOGI(TAG, "HTTP ok status=%d bytes=%d", status, s_buf_len);
        if (status == 200) {
            ok = parse_weather_json(s_buf);
        } else {
            ESP_LOGW(TAG, "Unexpected HTTP status=%d", status);
        }
        if (s_buf_truncated) {
            ESP_LOGW(TAG, "Weather payload truncated bytes=%d cap=%d", s_buf_len, s_buf_cap);
        }
    } else {
        ESP_LOGE(TAG, "HTTP error: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(s_buf);
    s_buf = NULL;
    s_buf_cap = 0;

    return ok;
}

bool weather_service_get(int *wong_tai_sin_temp_c, int *humidity_percent)
{
    if (!s_has_valid) {
        return false;
    }
    if (wong_tai_sin_temp_c != NULL) {
        *wong_tai_sin_temp_c = s_temp_wts;
    }
    if (humidity_percent != NULL) {
        *humidity_percent = s_humidity;
    }
    return true;
}

bool weather_service_get_condition(int *icon_code, const char **description)
{
    if (!s_has_valid) {
        return false;
    }
    if (icon_code != NULL) {
        *icon_code = s_icon_code;
    }
    if (description != NULL) {
        *description = s_icon_desc;
    }
    return true;
}

bool weather_service_has_valid(void)
{
    return s_has_valid;
}
