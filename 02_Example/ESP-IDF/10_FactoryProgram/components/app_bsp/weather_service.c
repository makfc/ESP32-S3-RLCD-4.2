#include "weather_service.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#define TAG "weather"
#define WEATHER_URL "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=rhrread&lang=tc"
#define WARNING_INFO_URL "https://data.weather.gov.hk/weatherAPI/opendata/weather.php?dataType=warningInfo&lang=tc"
#define WEATHER_PLACE_WTS "黃大仙"
#define BUF_INIT_SIZE (8 * 1024)
#define BUF_MAX_SIZE  (128 * 1024)
#define WARN_MAX_ITEMS 24
#define WARN_HEADLINE_LEN 384

static char *s_buf = NULL;
static int s_buf_len = 0;
static int s_buf_cap = 0;
static bool s_buf_truncated = false;

static int s_temp_wts = INT_MIN;
static int s_humidity = -1;
static int s_icon_code = -1;
static const char *s_icon_desc = "天氣";
static bool s_has_valid = false;
static char s_warn_headlines[WARN_MAX_ITEMS][WARN_HEADLINE_LEN] = {{0}};
static time_t s_warn_update_epochs[WARN_MAX_ITEMS] = {0};
static size_t s_warn_count = 0;

static esp_err_t http_event_cb(esp_http_client_event_t *evt);

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

static void reset_http_buf(void)
{
    if (s_buf != NULL) {
        free(s_buf);
        s_buf = NULL;
    }
    s_buf_len = 0;
    s_buf_cap = 0;
    s_buf_truncated = false;
}

static bool http_fetch_json(const char *url, int timeout_ms, int rx_buf_size, int tx_buf_size)
{
    reset_http_buf();

    s_buf = (char *)heap_caps_malloc(BUF_INIT_SIZE, MALLOC_CAP_SPIRAM);
    if (s_buf == NULL) {
        s_buf = (char *)malloc(BUF_INIT_SIZE);
    }
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "Buffer alloc failed url=%s", (url != NULL) ? url : "null");
        return false;
    }
    memset(s_buf, 0, BUF_INIT_SIZE);
    s_buf_cap = BUF_INIT_SIZE;

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_cb,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = rx_buf_size,
        .buffer_size_tx = tx_buf_size,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "esp_http_client_init failed url=%s", (url != NULL) ? url : "null");
        reset_http_buf();
        return false;
    }

    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    esp_err_t err = esp_http_client_perform(client);
    bool ok = false;
    if (err == ESP_OK) {
        const int status = esp_http_client_get_status_code(client);
        s_buf[s_buf_len] = '\0';
        ESP_LOGI(
            TAG,
            "HTTP ok url=%s status=%d bytes=%d",
            (url != NULL) ? url : "null",
            status,
            s_buf_len
        );
        ok = (status == 200);
        if (!ok) {
            ESP_LOGW(TAG, "Unexpected HTTP status=%d url=%s", status, (url != NULL) ? url : "null");
        }
        if (s_buf_truncated) {
            ESP_LOGW(TAG, "Payload truncated bytes=%d cap=%d url=%s", s_buf_len, s_buf_cap, (url != NULL) ? url : "null");
        }
    } else {
        ESP_LOGE(TAG, "HTTP error url=%s: %s", (url != NULL) ? url : "null", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return ok;
}

static void clear_warn_items(void)
{
    for (size_t i = 0; i < WARN_MAX_ITEMS; i++) {
        s_warn_headlines[i][0] = '\0';
        s_warn_update_epochs[i] = 0;
    }
    s_warn_count = 0;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day)
{
    year -= (month <= 2);
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = (unsigned)(year - era * 400);
    const unsigned doy = (153U * (month + (month > 2 ? (unsigned)-3 : 9U)) + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

static bool parse_iso8601_to_epoch(const char *iso_time, time_t *out_epoch)
{
    if ((iso_time == NULL) || (out_epoch == NULL)) {
        return false;
    }

    int year = 0;
    int mon = 0;
    int day = 0;
    int hour = 0;
    int min = 0;
    int sec = 0;
    char sign = '+';
    int tz_h = 0;
    int tz_m = 0;

    int matched = sscanf(
        iso_time,
        "%d-%d-%dT%d:%d:%d%c%d:%d",
        &year,
        &mon,
        &day,
        &hour,
        &min,
        &sec,
        &sign,
        &tz_h,
        &tz_m
    );
    if (matched != 9) {
        if (sscanf(iso_time, "%d-%d-%dT%d:%d:%dZ", &year, &mon, &day, &hour, &min, &sec) == 6) {
            sign = '+';
            tz_h = 0;
            tz_m = 0;
        } else {
            return false;
        }
    }

    if ((mon < 1) || (mon > 12) || (day < 1) || (day > 31)) {
        return false;
    }
    if ((hour < 0) || (hour > 23) || (min < 0) || (min > 59) || (sec < 0) || (sec > 60)) {
        return false;
    }
    if ((sign != '+') && (sign != '-')) {
        return false;
    }

    const int64_t days = days_from_civil(year, (unsigned)mon, (unsigned)day);
    int64_t epoch = days * 86400LL + (int64_t)hour * 3600LL + (int64_t)min * 60LL + (int64_t)sec;
    int tz_offset_sec = tz_h * 3600 + tz_m * 60;
    if (sign == '+') {
        epoch -= tz_offset_sec;
    } else {
        epoch += tz_offset_sec;
    }
    if (epoch < 0) {
        return false;
    }

    *out_epoch = (time_t)epoch;
    return true;
}

static void compact_whitespace_utf8(const char *src, char *dst, size_t dst_len)
{
    if ((dst == NULL) || (dst_len == 0)) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    size_t out = 0;
    bool prev_space = true;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        const unsigned char c = *p;
        if (isspace(c) != 0) {
            if (!prev_space && (out + 1 < dst_len)) {
                dst[out++] = ' ';
            }
            prev_space = true;
            continue;
        }
        if (out + 1 >= dst_len) {
            break;
        }
        dst[out++] = (char)c;
        prev_space = false;
    }
    if ((out > 0) && (dst[out - 1] == ' ')) {
        out--;
    }
    dst[out] = '\0';
}

static void build_warning_info_headline(cJSON *contents, char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0)) {
        return;
    }
    buf[0] = '\0';
    if (!cJSON_IsArray(contents)) {
        return;
    }

    cJSON *part0 = cJSON_GetArrayItem(contents, 0);
    cJSON *part1 = cJSON_GetArrayItem(contents, 1);
    if (!cJSON_IsString(part0) || (part0->valuestring == NULL)) {
        return;
    }

    char first[WARN_HEADLINE_LEN] = {0};
    char second[WARN_HEADLINE_LEN] = {0};
    compact_whitespace_utf8(part0->valuestring, first, sizeof(first));
    if (first[0] == '\0') {
        return;
    }

    if (cJSON_IsString(part1) && (part1->valuestring != NULL)) {
        compact_whitespace_utf8(part1->valuestring, second, sizeof(second));
    }

    if (second[0] != '\0') {
        snprintf(buf, len, "%s %s", first, second);
    } else {
        strlcpy(buf, first, len);
    }
}

static bool parse_warning_info_json(const char *json_text)
{
    clear_warn_items();
    if (json_text == NULL) {
        return false;
    }

    cJSON *root = cJSON_Parse(json_text);
    if (root == NULL) {
        ESP_LOGE(TAG, "warningInfo JSON parse failed");
        return false;
    }

    cJSON *details = cJSON_GetObjectItemCaseSensitive(root, "details");
    if (!cJSON_IsArray(details)) {
        cJSON_Delete(root);
        ESP_LOGW(TAG, "warningInfo missing details array");
        return false;
    }

    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, details) {
        if (!cJSON_IsObject(entry)) {
            continue;
        }
        if (s_warn_count >= WARN_MAX_ITEMS) {
            ESP_LOGW(TAG, "warningInfo exceeds max items=%u, truncating", (unsigned int)WARN_MAX_ITEMS);
            break;
        }

        cJSON *contents = cJSON_GetObjectItemCaseSensitive(entry, "contents");
        cJSON *update = cJSON_GetObjectItemCaseSensitive(entry, "updateTime");

        char headline[WARN_HEADLINE_LEN] = {0};
        build_warning_info_headline(contents, headline, sizeof(headline));
        if (headline[0] == '\0') {
            continue;
        }

        time_t update_epoch = 0;
        if (cJSON_IsString(update) && (update->valuestring != NULL) && (update->valuestring[0] != '\0')) {
            if (!parse_iso8601_to_epoch(update->valuestring, &update_epoch)) {
                ESP_LOGW(TAG, "warningInfo updateTime parse failed: %s", update->valuestring);
            }
        }

        strlcpy(s_warn_headlines[s_warn_count], headline, sizeof(s_warn_headlines[s_warn_count]));
        s_warn_update_epochs[s_warn_count] = update_epoch;
        s_warn_count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "warningInfo items=%u", (unsigned int)s_warn_count);
    return true;
}

static void format_warn_update_local(time_t update_epoch, char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0)) {
        return;
    }
    if (update_epoch <= 0) {
        strlcpy(buf, "--", len);
        return;
    }

    struct tm tm_local = {0};
    localtime_r(&update_epoch, &tm_local);
    snprintf(
        buf,
        len,
        "%d月%d日%02d:%02d:%02d",
        tm_local.tm_mon + 1,
        tm_local.tm_mday,
        tm_local.tm_hour,
        tm_local.tm_min,
        tm_local.tm_sec
    );
}

static void format_warn_relative_age(time_t update_epoch, char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0)) {
        return;
    }
    if (update_epoch <= 0) {
        strlcpy(buf, "--", len);
        return;
    }

    time_t now = 0;
    time(&now);
    int64_t age_sec = (int64_t)now - (int64_t)update_epoch;
    if (age_sec < 0) {
        age_sec = 0;
    }

    if (age_sec < 60) {
        strlcpy(buf, "剛剛", len);
        return;
    }
    if (age_sec < 3600) {
        snprintf(buf, len, "%d分鐘前", (int)(age_sec / 60));
        return;
    }
    snprintf(buf, len, "%d小時前", (int)(age_sec / 3600));
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
    bool ok = false;
    if (http_fetch_json(WEATHER_URL, 15000, 4096, 1024)) {
        ok = parse_weather_json(s_buf);
    } else {
        ESP_LOGW(TAG, "Failed to fetch weather JSON");
    }
    reset_http_buf();

    if (http_fetch_json(WARNING_INFO_URL, 12000, 4096, 1024)) {
        (void)parse_warning_info_json(s_buf);
    } else {
        clear_warn_items();
        ESP_LOGW(TAG, "Failed to fetch warningInfo JSON");
    }
    reset_http_buf();

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

size_t weather_service_warn_count(void)
{
    return s_warn_count;
}

bool weather_service_warn_get_display(size_t idx, char *buf, size_t len)
{
    if ((buf == NULL) || (len == 0) || (idx >= s_warn_count)) {
        return false;
    }

    char issue_text[40] = {0};
    char rel_text[24] = {0};
    format_warn_update_local(s_warn_update_epochs[idx], issue_text, sizeof(issue_text));
    format_warn_relative_age(s_warn_update_epochs[idx], rel_text, sizeof(rel_text));
    snprintf(buf, len, "%s(%s | %s)", s_warn_headlines[idx], issue_text, rel_text);
    return true;
}
