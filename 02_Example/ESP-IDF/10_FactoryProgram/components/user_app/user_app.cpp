#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <inttypes.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include "button_bsp.h"
#include "user_app.h"
#include "gui_guider.h"
#include "i2c_equipment.h"
#include "i2c_bsp.h"
#include "codec_bsp.h"
#include "adc_bsp.h"
#include "esp_wifi_bsp.h"
#include "esp_wifi.h"
#include "ble_scan_bsp.h"
#include "news_service.h"
#include "weather_service.h"
#include "lvgl_bsp.h"
LV_FONT_DECLARE(lv_font_news_tc_16);

static lv_ui init_ui;
I2cMasterBus I2cbus(14,13,0);

/* News ticker inter-task communication (News_TickerTask → Lvgl_UserTask) */
static volatile bool s_news_show  = false;
static volatile bool s_news_dirty = false;
static const size_t NEWS_TICKER_BUF_INIT_LEN = 1024;
static char         *s_news_buf = NULL;
static size_t        s_news_buf_cap = 0;
static volatile uint32_t s_news_post_seq = 0;
Shtc3Port *shtc3port = NULL;
EventGroupHandle_t CodecGroups;
CodecPort *codecport = NULL;
static uint8_t *audio_ptr = NULL;
static bool is_Music = true;
static lv_obj_t *s_clock_char_labels[8] = {0};
static const char *TAG = "user_app";
static char s_clock_last_text[9] = {'\0'};
static const uint32_t CLOCK_POLL_MS = 200;
static const uint32_t STATUS_POLL_MS = 100;
static const int LVGL_LOCK_TIMEOUT_MS = 220;
static const int64_t LVGL_LOCK_LOG_INTERVAL_MS = 2000;
static const int64_t UI_LOOP_BODY_LOG_INTERVAL_MS = 2000;
static const uint32_t BATTERY_INTERVAL_MS = 2000;
static const uint32_t SENSOR_INTERVAL_MS = 5000;
static const uint32_t NEWS_REFRESH_INTERVAL_MS = 30UL * 60UL * 1000UL;
static const uint32_t NEWS_RETRY_INTERVAL_MS = 60UL * 1000UL;
static const uint32_t NEWS_LOOP_SLEEP_MS = 500;
static const uint32_t NEWS_HEADLINE_ROTATE_MS = 5000;
static const uint32_t NEWS_TICKER_STEP_MS = 20;
static const int64_t UI_LOOP_GAP_WARN_MS = 450;
static const int64_t UI_LOOP_GAP_LOG_INTERVAL_MS = 2000;
static const int64_t UI_LOOP_BODY_WARN_MS = 80;
static const int64_t ADC_BLOCK_WARN_MS = 15;
static const int64_t SENSOR_BLOCK_WARN_MS = 60;
static const int64_t NEWS_STEP_WARN_MS = 500;
static const int NEWS_BAR_X = 0;
static const int NEWS_BAR_Y = 260;
static const int NEWS_BAR_W = 400;
static const int NEWS_BAR_H = 40;
static const int NEWS_BAR_LABEL_X = 6;
static const int NEWS_BAR_LABEL_W = NEWS_BAR_W - 12;
static const lv_coord_t NEWS_TICKER_GAP_MIN_PX = 18;
static const lv_coord_t NEWS_TICKER_GAP_MAX_PX = 72;
static const uint16_t NEWS_LONG_HEADLINE_MARQUEE_SPEED_PX_S = 50;
static const lv_coord_t NEWS_LONG_HEADLINE_END_EXTRA_PX = 6;
static bool s_news_ticker_running = false;
static int64_t s_news_ticker_last_step_ms = 0;
static uint32_t s_news_ticker_subpx_accum = 0;
static lv_coord_t s_news_ticker_w1 = NEWS_BAR_LABEL_W;
static lv_coord_t s_news_ticker_w2 = NEWS_BAR_LABEL_W;
static char *s_news_next_buf = NULL;
static size_t s_news_next_buf_cap = 0;
static bool s_news_next_pending = false;
static uint8_t s_news_next_applied_mask = 0;
static lv_obj_t *s_news_anim_box = NULL;
static lv_obj_t *s_news_marquee_label = NULL;
static lv_obj_t *s_news_marquee_label_back = NULL;
static lv_obj_t *s_hko_bar = NULL;
static lv_obj_t *s_hko_label = NULL;
static volatile bool s_status_dirty = true;
static volatile int s_battery_level = -1;
static volatile int s_humidity_percent = -1;    // Display humidity
static volatile int s_temperature_c = -1000;    // Display temperature
static volatile int s_sensor_humidity_percent = -1;
static volatile int s_sensor_temperature_c = -1000;
static volatile int s_hko_humidity_percent = -1;
static volatile int s_hko_temperature_c = -1000;
static volatile int s_hko_icon_code = -1;
static char s_hko_desc[24] = "天氣";
static char s_boot_status_text[512] = {0};
static volatile bool s_main_screen_ready = false;
static const BaseType_t UI_TASK_CORE_ID = 1;
static const BaseType_t BG_TASK_CORE_ID = 0;
static volatile uint32_t s_lvgl_lock_timeout_count = 0;
static int64_t s_lvgl_lock_last_log_ms = 0;

static void PostNewsText(const char *text, const char *reason, size_t idx, size_t cnt);
static void NewsBarApplyTextAnimated(const char *text);
static bool TryLockLvgl(void);
static inline int64_t NowMs(void);

static lv_coord_t NewsHeadlineTextWidthPx(lv_obj_t *label, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    if (label == NULL) {
        return 0;
    }

    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (font == NULL) {
        return 0;
    }

    const lv_coord_t letter_space = lv_obj_get_style_text_letter_space(label, LV_PART_MAIN);
    const lv_coord_t text_w = lv_txt_get_width(text, strlen(text), font, letter_space, LV_TEXT_FLAG_NONE);
    if (text_w <= 0) {
        return 0;
    }
    return text_w;
}

static lv_coord_t NewsTickerGapPx(lv_coord_t text_w)
{
    if (text_w < 0) {
        text_w = 0;
    }
    lv_coord_t gap = text_w / 12;
    if (gap < NEWS_TICKER_GAP_MIN_PX) {
        gap = NEWS_TICKER_GAP_MIN_PX;
    }
    if (gap > NEWS_TICKER_GAP_MAX_PX) {
        gap = NEWS_TICKER_GAP_MAX_PX;
    }
    return gap;
}

static void NewsLabelAnimSetY(void *obj, int32_t value)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)value);
}

static void NewsLabelAnimSetX(void *obj, int32_t value)
{
    if (obj == NULL) {
        return;
    }
    lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)value);
}

static void SetupNewsHeadlineLabelStyle(lv_obj_t *label, const lv_font_t *news_font)
{
    if (label == NULL) {
        return;
    }

    lv_label_set_text(label, "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(label, NEWS_BAR_LABEL_W, NEWS_BAR_H);
    lv_obj_set_x(label, NEWS_BAR_LABEL_X);
    lv_obj_set_style_border_width(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(label, news_font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_line_space(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}
static void BootStatusPush(const char *fmt, ...)
{
    if ((fmt == NULL) || (fmt[0] == '\0')) {
        return;
    }

    char line[160] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    const size_t cur_len = strlen(s_boot_status_text);
    const size_t line_len = strlen(line);
    if ((cur_len + line_len + 2) >= sizeof(s_boot_status_text)) {
        strlcpy(s_boot_status_text, line, sizeof(s_boot_status_text));
    } else {
        if (cur_len > 0) {
            strlcat(s_boot_status_text, "\n", sizeof(s_boot_status_text));
        }
        strlcat(s_boot_status_text, line, sizeof(s_boot_status_text));
    }

    if (init_ui.screen_label_1 == NULL) {
        return;
    }
    if (!TryLockLvgl()) {
        return;
    }

    lv_obj_clear_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(init_ui.screen_label_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_2, LV_OBJ_FLAG_HIDDEN);

    // Use the TC news font for boot status so Traditional Chinese glyphs render correctly.
    lv_obj_set_style_text_font(init_ui.screen_label_1, &lv_font_news_tc_16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(init_ui.screen_label_1, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(init_ui.screen_label_1, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(init_ui.screen_label_1, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(init_ui.screen_label_1, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(init_ui.screen_label_1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_label_set_text(init_ui.screen_label_1, s_boot_status_text);

    Lvgl_unlock();
    Lvgl_request_refresh();
}

static inline int RoundFloatToInt(float value)
{
    return (value >= 0.0f) ? (int)(value + 0.5f) : (int)(value - 0.5f);
}

static void RefreshEnvDisplay(const char *reason, bool always_log)
{
    const int display_temp = s_sensor_temperature_c;
    const int display_humi = s_sensor_humidity_percent;
    bool changed = false;

    if ((display_temp > -1000) && (display_temp != s_temperature_c)) {
        s_temperature_c = display_temp;
        changed = true;
    }
    if ((display_humi >= 0) && (display_humi != s_humidity_percent)) {
        s_humidity_percent = display_humi;
        changed = true;
    }
    if (changed) {
        s_status_dirty = true;
    }

    if (always_log || changed) {
        ESP_LOGI(
            TAG,
            "ENV apply (%s): indoor=%dC/%d%% hko=%dC/%d%% icon=%d desc=%s",
            (reason != NULL) ? reason : "unknown",
            s_temperature_c,
            s_humidity_percent,
            s_hko_temperature_c,
            s_hko_humidity_percent,
            s_hko_icon_code,
            s_hko_desc
        );
    }
}

static void ApplyWeatherSnapshot(const char *reason)
{
    int wts_temp = 0;
    int humidity = 0;
    int icon_code = -1;
    const char *icon_desc = NULL;
    if (weather_service_get(&wts_temp, &humidity)) {
        bool changed = (s_hko_temperature_c != wts_temp) || (s_hko_humidity_percent != humidity);
        if (weather_service_get_condition(&icon_code, &icon_desc)) {
            if (icon_desc == NULL) {
                icon_desc = "天氣";
            }
            if (s_hko_icon_code != icon_code) {
                changed = true;
            }
            if (strcmp(s_hko_desc, icon_desc) != 0) {
                changed = true;
            }
            s_hko_icon_code = icon_code;
            strlcpy(s_hko_desc, icon_desc, sizeof(s_hko_desc));
        }
        s_hko_temperature_c = wts_temp;
        s_hko_humidity_percent = humidity;
        if (changed) {
            s_status_dirty = true;
        }
        RefreshEnvDisplay(reason, true);
    } else {
        ESP_LOGW(TAG, "WEATHER apply (%s): no valid data", (reason != NULL) ? reason : "unknown");
        RefreshEnvDisplay(reason, false);
    }
}

static bool TryLockLvgl(void)
{
    if (Lvgl_lock(LVGL_LOCK_TIMEOUT_MS)) {
        return true;
    }
    s_lvgl_lock_timeout_count++;
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s_lvgl_lock_last_log_ms) >= LVGL_LOCK_LOG_INTERVAL_MS) {
        const uint32_t timeout_count = s_lvgl_lock_timeout_count;
        s_lvgl_lock_timeout_count = 0;
        s_lvgl_lock_last_log_ms = now_ms;
        ESP_LOGW(
            TAG,
            "LVGL lock timeout x%lu in last %lld ms",
            (unsigned long)timeout_count,
            (long long)LVGL_LOCK_LOG_INTERVAL_MS
        );
    }
    return false;
}

static bool LockLvglForBootTransition(const char *stage)
{
    const int64_t t_start_ms = NowMs();
    if (!Lvgl_lock(-1)) {
        ESP_LOGE(TAG, "Boot transition lock failed at stage=%s", (stage != NULL) ? stage : "unknown");
        return false;
    }

    const int64_t wait_ms = NowMs() - t_start_ms;
    if (wait_ms >= LVGL_LOCK_TIMEOUT_MS) {
        ESP_LOGW(
            TAG,
            "Boot transition lock waited %lld ms at stage=%s",
            (long long)wait_ms,
            (stage != NULL) ? stage : "unknown"
        );
    }
    return true;
}

static inline int64_t NowMs(void)
{
    return esp_timer_get_time() / 1000;
}

static void LogBlockingMs(const char *name, int64_t dur_ms, int64_t warn_ms, bool always_info)
{
    if ((dur_ms >= warn_ms) || always_info) {
        if (dur_ms >= warn_ms) {
            ESP_LOGW(TAG, "BLOCK %s took %lld ms", name, (long long)dur_ms);
        } else {
            ESP_LOGI(TAG, "BLOCK %s took %lld ms", name, (long long)dur_ms);
        }
    }
}

static bool EnsureNewsBuffer(size_t need_len)
{
    if (need_len == 0) {
        need_len = 1;
    }
    if ((s_news_buf != NULL) && (need_len <= s_news_buf_cap)) {
        return true;
    }

    size_t new_cap = (s_news_buf_cap > 0) ? s_news_buf_cap : NEWS_TICKER_BUF_INIT_LEN;
    while (new_cap < need_len) {
        if (new_cap >= (64 * 1024)) {
            new_cap = need_len;
            break;
        }
        new_cap *= 2;
    }

    char *new_buf = (char *)heap_caps_realloc(s_news_buf, new_cap, MALLOC_CAP_SPIRAM);
    if (new_buf == NULL) {
        new_buf = (char *)realloc(s_news_buf, new_cap);
    }
    if (new_buf == NULL) {
        ESP_LOGE(TAG, "NEWS buffer alloc failed need=%u", (unsigned int)need_len);
        return false;
    }
    s_news_buf = new_buf;
    s_news_buf_cap = new_cap;
    return true;
}

static bool EnsureNewsNextBuffer(size_t need_len)
{
    if (need_len == 0) {
        need_len = 1;
    }
    if ((s_news_next_buf != NULL) && (need_len <= s_news_next_buf_cap)) {
        return true;
    }

    size_t new_cap = (s_news_next_buf_cap > 0) ? s_news_next_buf_cap : NEWS_TICKER_BUF_INIT_LEN;
    while (new_cap < need_len) {
        if (new_cap >= (64 * 1024)) {
            new_cap = need_len;
            break;
        }
        new_cap *= 2;
    }

    char *new_buf = (char *)heap_caps_realloc(s_news_next_buf, new_cap, MALLOC_CAP_SPIRAM);
    if (new_buf == NULL) {
        new_buf = (char *)realloc(s_news_next_buf, new_cap);
    }
    if (new_buf == NULL) {
        ESP_LOGE(TAG, "NEWS next buffer alloc failed need=%u", (unsigned int)need_len);
        return false;
    }
    s_news_next_buf = new_buf;
    s_news_next_buf_cap = new_cap;
    return true;
}

static size_t NewsTickerSourceCount(void)
{
    return news_service_count() + weather_service_warn_count();
}

static bool PostNewsHeadlineByIndex(size_t idx, const char *reason)
{
    char title[NEWS_TITLE_LEN] = {0};
    char age_text[24] = {0};
    char display_text[NEWS_TITLE_LEN + 48] = {0};
    char warn_display[NEWS_TITLE_LEN * 4] = {0};
    const size_t warn_cnt = weather_service_warn_count();
    const size_t news_cnt = news_service_count();
    const size_t cnt = warn_cnt + news_cnt;
    if (cnt == 0 || idx >= cnt) {
        return false;
    }

    if (idx < warn_cnt) {
        if (!weather_service_warn_get_display(idx, warn_display, sizeof(warn_display)) || warn_display[0] == '\0') {
            return false;
        }
        PostNewsText(warn_display, reason, idx + 1, cnt);
        return true;
    }

    const size_t news_idx = idx - warn_cnt;
    if (!news_service_get(news_idx, title, sizeof(title)) || title[0] == '\0') {
        return false;
    }
    if (news_service_get_relative_age(news_idx, age_text, sizeof(age_text)) && age_text[0] != '\0') {
        snprintf(display_text, sizeof(display_text), "%s（%s）", title, age_text);
        PostNewsText(display_text, reason, idx + 1, cnt);
    } else {
        PostNewsText(title, reason, idx + 1, cnt);
    }
    return true;
}

static void PostNewsText(const char *text, const char *reason, size_t idx, size_t cnt)
{
    if (text == NULL) {
        text = "";
    }
    const size_t text_len = strlen(text);
    if (!EnsureNewsBuffer(text_len + 1)) {
        return;
    }
    strlcpy(s_news_buf, text, s_news_buf_cap);
    s_news_post_seq++;
    s_news_dirty = true;
    ESP_LOGD(
        TAG,
        "NEWS post #%lu reason=%s idx=%u cnt=%u len=%u text=\"%.64s\"",
        (unsigned long)s_news_post_seq,
        (reason != NULL) ? reason : "unknown",
        (unsigned int)idx,
        (unsigned int)cnt,
        (unsigned int)text_len,
        s_news_buf
    );
}

static lv_coord_t NewsTickerLabelWidthPx(lv_coord_t text_w)
{
    if (text_w <= 0) {
        return NEWS_BAR_LABEL_W;
    }
    return text_w + NEWS_LONG_HEADLINE_END_EXTRA_PX;
}

static lv_coord_t NewsTickerApplyTextToLabel(lv_obj_t *label, const char *text)
{
    if (label == NULL) {
        return NEWS_BAR_LABEL_W;
    }
    if (text == NULL) {
        text = "";
    }

    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_label_set_text(label, text);
    const lv_coord_t text_w = NewsHeadlineTextWidthPx(label, text);
    const lv_coord_t label_w = NewsTickerLabelWidthPx(text_w);
    lv_obj_set_width(label, label_w);
    lv_obj_set_y(label, 0);
    return label_w;
}

static void NewsTickerReset(const char *text)
{
    if (text == NULL) {
        text = "";
    }
    if ((s_news_marquee_label == NULL) || (s_news_marquee_label_back == NULL)) {
        return;
    }

    lv_anim_del(s_news_marquee_label, NewsLabelAnimSetX);
    lv_anim_del(s_news_marquee_label_back, NewsLabelAnimSetX);
    lv_anim_del(s_news_marquee_label, NewsLabelAnimSetY);
    lv_anim_del(s_news_marquee_label_back, NewsLabelAnimSetY);

    s_news_ticker_w1 = NewsTickerApplyTextToLabel(s_news_marquee_label, text);
    s_news_ticker_w2 = NewsTickerApplyTextToLabel(s_news_marquee_label_back, text);

    const lv_coord_t gap = NewsTickerGapPx(s_news_ticker_w1);
    lv_obj_set_x(s_news_marquee_label, NEWS_BAR_LABEL_X);
    lv_obj_set_x(s_news_marquee_label_back, NEWS_BAR_LABEL_X + s_news_ticker_w1 + gap);

    s_news_ticker_running = true;
    s_news_ticker_last_step_ms = NowMs();
    s_news_ticker_subpx_accum = 0;
    s_news_next_pending = false;
    s_news_next_applied_mask = 0;
}

static void NewsTickerQueueText(const char *text)
{
    if (text == NULL) {
        text = "";
    }
    if ((s_news_marquee_label == NULL) || (s_news_marquee_label_back == NULL)) {
        return;
    }

    const char *t1 = lv_label_get_text(s_news_marquee_label);
    const char *t2 = lv_label_get_text(s_news_marquee_label_back);
    if (s_news_next_pending) {
        if ((s_news_next_buf != NULL) && (strcmp(s_news_next_buf, text) == 0)) {
            return;
        }
    } else if ((t1 != NULL) && (t2 != NULL) && (strcmp(t1, text) == 0) && (strcmp(t2, text) == 0)) {
        return;
    }

    const size_t text_len = strlen(text);
    if (!EnsureNewsNextBuffer(text_len + 1)) {
        return;
    }
    strlcpy(s_news_next_buf, text, s_news_next_buf_cap);
    s_news_next_pending = true;
    s_news_next_applied_mask = 0;
}

static void NewsTickerApplyPendingToLabelIfNeeded(lv_obj_t *label, lv_coord_t *label_w, uint8_t bit_mask)
{
    if (!s_news_next_pending || (label == NULL) || (label_w == NULL)) {
        return;
    }
    if ((s_news_next_applied_mask & bit_mask) != 0) {
        return;
    }
    *label_w = NewsTickerApplyTextToLabel(label, s_news_next_buf);
    s_news_next_applied_mask |= bit_mask;
    if (s_news_next_applied_mask == 0x03) {
        s_news_next_pending = false;
        s_news_next_applied_mask = 0;
    }
}

static bool NewsTickerStep(int64_t now_ms)
{
    if (!s_news_ticker_running || (s_news_marquee_label == NULL) || (s_news_marquee_label_back == NULL)) {
        return false;
    }

    int64_t dt_ms = now_ms - s_news_ticker_last_step_ms;
    if (dt_ms <= 0) {
        return false;
    }
    s_news_ticker_last_step_ms = now_ms;

    uint64_t pixel_scaled =
        ((uint64_t)NEWS_LONG_HEADLINE_MARQUEE_SPEED_PX_S * (uint64_t)dt_ms) + (uint64_t)s_news_ticker_subpx_accum;
    lv_coord_t move_px = (lv_coord_t)(pixel_scaled / 1000ULL);
    s_news_ticker_subpx_accum = (uint32_t)(pixel_scaled % 1000ULL);
    if (move_px <= 0) {
        return false;
    }

    lv_obj_set_x(s_news_marquee_label, lv_obj_get_x(s_news_marquee_label) - move_px);
    lv_obj_set_x(s_news_marquee_label_back, lv_obj_get_x(s_news_marquee_label_back) - move_px);

    for (int i = 0; i < 2; i++) {
        bool recycled = false;

        const lv_coord_t x1 = lv_obj_get_x(s_news_marquee_label);
        if ((x1 + s_news_ticker_w1) <= NEWS_BAR_LABEL_X) {
            NewsTickerApplyPendingToLabelIfNeeded(s_news_marquee_label, &s_news_ticker_w1, 0x01);
            const lv_coord_t x2 = lv_obj_get_x(s_news_marquee_label_back);
            const lv_coord_t gap = NewsTickerGapPx(s_news_ticker_w2);
            lv_obj_set_x(s_news_marquee_label, x2 + s_news_ticker_w2 + gap);
            recycled = true;
        }

        const lv_coord_t x2 = lv_obj_get_x(s_news_marquee_label_back);
        if ((x2 + s_news_ticker_w2) <= NEWS_BAR_LABEL_X) {
            NewsTickerApplyPendingToLabelIfNeeded(s_news_marquee_label_back, &s_news_ticker_w2, 0x02);
            const lv_coord_t x1_new = lv_obj_get_x(s_news_marquee_label);
            const lv_coord_t gap = NewsTickerGapPx(s_news_ticker_w1);
            lv_obj_set_x(s_news_marquee_label_back, x1_new + s_news_ticker_w1 + gap);
            recycled = true;
        }

        if (!recycled) {
            break;
        }
    }

    return true;
}

static void NewsBarApplyTextAnimated(const char *text)
{
    if (text == NULL) {
        text = "";
    }

    if (s_news_marquee_label == NULL) {
        lv_label_set_text(init_ui.screen_news_label, text);
        return;
    }

    if (!s_news_ticker_running) {
        NewsTickerReset(text);
    } else {
        NewsTickerQueueText(text);
    }
}

static void SetupNewsTickerAnimated(const lv_font_t *news_font)
{
    lv_obj_t *parent = lv_obj_get_parent(init_ui.screen_news_label);
    if (parent == NULL) {
        return;
    }

    lv_obj_add_flag(init_ui.screen_news_label, LV_OBJ_FLAG_HIDDEN);

    s_news_anim_box = lv_obj_create(parent);
    lv_obj_set_pos(s_news_anim_box, NEWS_BAR_X, NEWS_BAR_Y);
    lv_obj_set_size(s_news_anim_box, NEWS_BAR_W, NEWS_BAR_H);
    lv_obj_add_flag(s_news_anim_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_news_anim_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_news_anim_box, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(s_news_anim_box, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(s_news_anim_box, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_news_anim_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_news_anim_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_news_anim_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(s_news_anim_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(s_news_anim_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(s_news_anim_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(s_news_anim_box, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_clip_corner(s_news_anim_box, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_move_foreground(s_news_anim_box);

    s_news_marquee_label = lv_label_create(s_news_anim_box);
    SetupNewsHeadlineLabelStyle(s_news_marquee_label, news_font);
    lv_obj_set_y(s_news_marquee_label, 0);

    s_news_marquee_label_back = lv_label_create(s_news_anim_box);
    SetupNewsHeadlineLabelStyle(s_news_marquee_label_back, news_font);
    lv_obj_set_y(s_news_marquee_label_back, 0);
    lv_obj_set_x(s_news_marquee_label_back, NEWS_BAR_LABEL_X + NEWS_BAR_LABEL_W + NewsTickerGapPx(NEWS_BAR_LABEL_W));

    s_news_ticker_running = false;
    s_news_ticker_last_step_ms = 0;
    s_news_ticker_subpx_accum = 0;
    s_news_ticker_w1 = NEWS_BAR_LABEL_W;
    s_news_ticker_w2 = NEWS_BAR_LABEL_W;
    s_news_next_pending = false;
    s_news_next_applied_mask = 0;
}

static bool FontHasGlyph(const lv_font_t *font, uint32_t codepoint)
{
    if (font == NULL) {
        return false;
    }
    lv_font_glyph_dsc_t dsc = {};
    return lv_font_get_glyph_dsc(font, &dsc, codepoint, 0);
}

static bool FontHasGlyphs(const lv_font_t *font, const uint32_t *codepoints, size_t count)
{
    if ((font == NULL) || (codepoints == NULL)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (!FontHasGlyph(font, codepoints[i])) {
            return false;
        }
    }
    return true;
}

static void LogCurrentLocalTime(const char *prefix)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    char time_str[40] = {0};

    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "%s%s (epoch=%lld)", prefix, time_str, (long long)now);
}

static void SetClockTimeText(const char *time_text)
{
    if ((time_text == NULL) || (s_clock_char_labels[0] == NULL)) {
        lv_label_set_text(init_ui.screen_label_3, (time_text != NULL) ? time_text : "--:--:--");
        return;
    }

    char normalized[9] = {' ', ' ', ':', ' ', ' ', ':', ' ', ' ', '\0'};
    for (int i = 0; i < 8; i++) {
        if (time_text[i] == '\0') {
            break;
        }
        normalized[i] = time_text[i];
    }

    for (int i = 0; i < 8; i++) {
        if (normalized[i] == s_clock_last_text[i]) {
            continue;
        }
        if (s_clock_char_labels[i] != NULL) {
            char one_char[2] = {' ', '\0'};
            one_char[0] = normalized[i];
            lv_label_set_text(s_clock_char_labels[i], one_char);
        }
        s_clock_last_text[i] = normalized[i];
    }
}

static void InvalidateClockRow(void)
{
    if (init_ui.screen_label_3 != NULL) {
        lv_obj_invalidate(init_ui.screen_label_3);
    }
    for (int i = 0; i < 8; i++) {
        if (s_clock_char_labels[i] != NULL) {
            lv_obj_invalidate(s_clock_char_labels[i]);
        }
    }
}

static void SetupClockTimeSlots(void)
{
    const int digit_width = 56;
    const int colon_width = 24;
    const int start_x = 8;
    const int start_y = 0;
    const int slot_height = 100;
    const char *placeholder = "00:00:00";
    int x = start_x;

    // Use screen_label_3 as a solid white background bar for the clock row.
    lv_obj_clear_flag(init_ui.screen_label_3, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(init_ui.screen_label_3, 0, start_y);
    lv_obj_set_size(init_ui.screen_label_3, 400, slot_height);
    lv_label_set_text(init_ui.screen_label_3, "");
    lv_obj_set_style_text_opa(init_ui.screen_label_3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(init_ui.screen_label_3, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(init_ui.screen_label_3, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(init_ui.screen_label_3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(init_ui.screen_label_3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(init_ui.screen_label_3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(init_ui.screen_label_3, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    for (int i = 0; i < 8; i++) {
        const int slot_width = ((i == 2) || (i == 5)) ? colon_width : digit_width;
        lv_obj_t *slot = lv_label_create(init_ui.screen_cont_2);
        s_clock_char_labels[i] = slot;

        lv_obj_set_pos(slot, x, start_y);
        lv_obj_set_size(slot, slot_width, slot_height);
        lv_label_set_long_mode(slot, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(slot, &lv_font_MISANSMEDIUM_100, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_opa(slot, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(slot, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_letter_space(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_line_space(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(slot, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);
        // Draw opaque white background per slot to fully clear old glyph pixels.
        lv_obj_set_style_bg_opa(slot, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(slot, lv_color_hex(0xffffff), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(slot, -2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        char one_char[2] = {placeholder[i], '\0'};
        lv_label_set_text(slot, one_char);
        x += slot_width;
    }
}

void Lvgl_Cont1Task(void *arg) {
    if (LockLvglForBootTransition("show_boot_line_1")) {
        lv_obj_clear_flag(init_ui.screen_label_1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_label_2, LV_OBJ_FLAG_HIDDEN);
        Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (LockLvglForBootTransition("show_boot_line_2")) {
        lv_obj_clear_flag(init_ui.screen_label_2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_label_1, LV_OBJ_FLAG_HIDDEN);
        Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (LockLvglForBootTransition("enter_main_screen")) {
        lv_obj_clear_flag(init_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
        Lvgl_unlock();
        s_main_screen_ready = true;
        Lvgl_request_refresh();
        ESP_LOGI(TAG, "Boot transition: entered main screen");
    }
    vTaskDelete(NULL); 
}

void Lvgl_UserTask(void *arg) {
    time_t last_display_epoch = (time_t)-1;
    int last_date_key = -1;
    int last_minute_key = -1;
    char lvgl_buffer[30] = {""};
    int ui_last_battery_level = -101;
    int ui_last_sensor_humidity = -1;
    int ui_last_sensor_temperature = -1000;
    char ui_last_hko_line[128] = {0};
    TickType_t last_wake_tick = xTaskGetTickCount();
    int64_t last_loop_start_ms = 0;
    int64_t last_loop_gap_warn_ms = 0;
    int64_t last_loop_body_warn_ms = 0;

    for(;;) {
        if (!s_main_screen_ready) {
            vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(100));
            continue;
        }

        const int64_t loop_start_ms = NowMs();
        if (last_loop_start_ms != 0) {
            const int64_t loop_gap_ms = loop_start_ms - last_loop_start_ms;
            if ((loop_gap_ms >= UI_LOOP_GAP_WARN_MS) &&
                ((loop_start_ms - last_loop_gap_warn_ms) >= UI_LOOP_GAP_LOG_INTERVAL_MS)) {
                last_loop_gap_warn_ms = loop_start_ms;
                ESP_LOGW(TAG, "UI loop gap=%lld ms (clock may freeze)", (long long)loop_gap_ms);
            }
        }
        last_loop_start_ms = loop_start_ms;

        bool need_refresh = false;
        const bool time_valid = espwifi_is_time_synced();
        time_t now_epoch = 0;
        bool clock_needs_update = false;

        if (!time_valid) {
            clock_needs_update = (last_display_epoch != 0 || last_minute_key != -1 || last_date_key != -1);
        } else {
            time(&now_epoch);
            clock_needs_update = (now_epoch != last_display_epoch);
        }

        const bool ticker_step_due =
            s_news_ticker_running && ((loop_start_ms - s_news_ticker_last_step_ms) >= NEWS_TICKER_STEP_MS);
        const bool has_pending_ui_work =
            s_status_dirty || s_news_show || s_news_dirty || clock_needs_update || ticker_step_due;
        if (!has_pending_ui_work) {
            vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(CLOCK_POLL_MS));
            continue;
        }

        if (!TryLockLvgl()) {
            vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(CLOCK_POLL_MS));
            continue;
        }

        if (s_status_dirty) {
            s_status_dirty = false;
            const int battery_level = s_battery_level;
            const int sensor_humidity_percent = s_sensor_humidity_percent;
            const int sensor_temperature_c = s_sensor_temperature_c;
            const int hko_humidity_percent = s_hko_humidity_percent;
            const int hko_temperature_c = s_hko_temperature_c;
            const char *hko_desc = (s_hko_desc[0] != '\0') ? s_hko_desc : "天氣";

            if ((battery_level >= 0) && (battery_level != ui_last_battery_level)) {
                snprintf(lvgl_buffer,30,"%d%%",battery_level);
                lv_label_set_text(init_ui.screen_label_7, lvgl_buffer);
                ui_last_battery_level = battery_level;
            }
            if ((sensor_humidity_percent >= 0) && (sensor_humidity_percent != ui_last_sensor_humidity)) {
                snprintf(lvgl_buffer,30,"%d%%",sensor_humidity_percent);
                lv_label_set_text(init_ui.screen_label_11, lvgl_buffer);
                ui_last_sensor_humidity = sensor_humidity_percent;
            }
            if ((sensor_temperature_c > -1000) && (sensor_temperature_c != ui_last_sensor_temperature)) {
                snprintf(lvgl_buffer,30,"%d°",sensor_temperature_c);
                lv_label_set_text(init_ui.screen_label_12, lvgl_buffer);
                ui_last_sensor_temperature = sensor_temperature_c;
            }

            {
                char hko_temp_str[16] = "-°";
                char hko_humi_str[16] = "-%";
                char env_line[128] = {0};

                if (hko_temperature_c > -1000) {
                    snprintf(hko_temp_str, sizeof(hko_temp_str), "%d°", hko_temperature_c);
                }
                if (hko_humidity_percent >= 0) {
                    snprintf(hko_humi_str, sizeof(hko_humi_str), "%d%%", hko_humidity_percent);
                }

                snprintf(
                    env_line,
                    sizeof(env_line),
                    "%s %s %s",
                    hko_desc,
                    hko_temp_str,
                    hko_humi_str
                );
                if (strcmp(ui_last_hko_line, env_line) != 0) {
                    if (s_hko_label != NULL) {
                        lv_label_set_text(s_hko_label, env_line);
                    } else {
                        lv_label_set_text(init_ui.screen_label_8, env_line);
                    }
                    strlcpy(ui_last_hko_line, env_line, sizeof(ui_last_hko_line));
                }
            }
        }

        // Update clock once per second from system clock.
        // Gate on SNTP: show placeholders until first real sync to avoid
        // displaying a misleading time based on RTC or uninitialised clock.
        {
            static const char * const weekday_zh[] = {
                "週日","週一","週二","週三","週四","週五","週六"
            };
            if (!time_valid) {
                if (last_display_epoch != 0 || last_minute_key != -1 || last_date_key != -1) {
                    last_display_epoch = 0;
                    last_minute_key = -1;
                    last_date_key = -1;
                    memset(s_clock_last_text, 0, sizeof(s_clock_last_text));
                    InvalidateClockRow();
                    SetClockTimeText("--:--:--");
                    lv_label_set_text(init_ui.screen_label_4, "同步時間中...");
                    need_refresh = true;
                }
            } else {
                struct tm display_tm = {0};
                if (now_epoch != last_display_epoch) {
                    last_display_epoch = now_epoch;
                    localtime_r(&now_epoch, &display_tm);

                    const int minute_key = (display_tm.tm_hour * 60) + display_tm.tm_min;
                    if (minute_key != last_minute_key) {
                        last_minute_key = minute_key;
                        memset(s_clock_last_text, 0, sizeof(s_clock_last_text));
                        InvalidateClockRow();
                    }

                    snprintf(
                        lvgl_buffer,
                        sizeof(lvgl_buffer),
                        "%02d:%02d:%02d",
                        display_tm.tm_hour,
                        display_tm.tm_min,
                        display_tm.tm_sec
                    );
                    SetClockTimeText(lvgl_buffer);

                    const int date_key = (display_tm.tm_year + 1900) * 10000
                                       + (display_tm.tm_mon + 1) * 100
                                       + display_tm.tm_mday;
                    if (date_key != last_date_key) {
                        last_date_key = date_key;
                        snprintf(lvgl_buffer, 30, "%d月%d日 %s",
                                 display_tm.tm_mon + 1, display_tm.tm_mday,
                                 weekday_zh[display_tm.tm_wday]);
                        lv_label_set_text(init_ui.screen_label_4, lvgl_buffer);
                    }
                    need_refresh = true;
                }
            }
        }

        if (s_news_show) {
            s_news_show = false;
            if (s_news_anim_box != NULL) {
                lv_obj_clear_flag(s_news_anim_box, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(init_ui.screen_news_label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_news_dirty) {
            s_news_dirty = false;
            NewsBarApplyTextAnimated(s_news_buf);
            ESP_LOGD(TAG, "NEWS apply #%lu text=\"%.64s\"", (unsigned long)s_news_post_seq, s_news_buf);
        }
        if (s_news_ticker_running && NewsTickerStep(NowMs())) {
            need_refresh = true;
        }

        Lvgl_unlock();
        if (need_refresh) {
            Lvgl_request_refresh();
        }

        const int64_t loop_body_ms = NowMs() - loop_start_ms;
        const int64_t now_ms = NowMs();
        if ((loop_body_ms >= UI_LOOP_BODY_WARN_MS) &&
            ((now_ms - last_loop_body_warn_ms) >= UI_LOOP_BODY_LOG_INTERVAL_MS)) {
            last_loop_body_warn_ms = now_ms;
            ESP_LOGW(TAG, "UI loop body took %lld ms", (long long)loop_body_ms);
        }
        const uint32_t loop_sleep_ms = s_news_ticker_running ? NEWS_TICKER_STEP_MS : CLOCK_POLL_MS;
        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(loop_sleep_ms));
    }
}

void Status_PollTask(void *arg) {
    TickType_t last_wake_tick = xTaskGetTickCount();
    TickType_t next_adc_tick = last_wake_tick;
    TickType_t next_sensor_tick = last_wake_tick;

    for (;;) {
        TickType_t now_tick = xTaskGetTickCount();

        if ((int32_t)(now_tick - next_adc_tick) >= 0) {
            next_adc_tick = now_tick + pdMS_TO_TICKS(BATTERY_INTERVAL_MS);
            const int64_t t_start_ms = NowMs();
            const int battery_level = (int)Adc_GetBatteryLevel();
            LogBlockingMs("Adc_GetBatteryLevel", NowMs() - t_start_ms, ADC_BLOCK_WARN_MS, false);
            if (s_battery_level != battery_level) {
                s_battery_level = battery_level;
                s_status_dirty = true;
            }
        }

        if ((int32_t)(now_tick - next_sensor_tick) >= 0) {
            next_sensor_tick = now_tick + pdMS_TO_TICKS(SENSOR_INTERVAL_MS);
            if (shtc3port != NULL) {
                float temp_c = 0.0f;
                float humi_percent = 0.0f;
                const int64_t t_start_ms = NowMs();
                const uint8_t err = shtc3port->Shtc3_ReadTempHumi(&temp_c, &humi_percent);
                LogBlockingMs("Shtc3_ReadTempHumi", NowMs() - t_start_ms, SENSOR_BLOCK_WARN_MS, false);
                if (err == 0) {
                    int sensor_temp = RoundFloatToInt(temp_c);
                    int sensor_humi = RoundFloatToInt(humi_percent);
                    if (sensor_humi < 0) sensor_humi = 0;
                    if (sensor_humi > 100) sensor_humi = 100;
                    const bool changed = (s_sensor_temperature_c != sensor_temp) || (s_sensor_humidity_percent != sensor_humi);
                    s_sensor_temperature_c = sensor_temp;
                    s_sensor_humidity_percent = sensor_humi;
                    if (changed) {
                        s_status_dirty = true;
                    }
                    ESP_LOGI(TAG, "SHTC3 sample: %dC %d%%", sensor_temp, sensor_humi);
                    RefreshEnvDisplay("sensor", false);
                } else {
                    ESP_LOGW(TAG, "Shtc3_ReadTempHumi failed err=%u", (unsigned int)err);
                }
            }
        }

        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(STATUS_POLL_MS));
    }
}

void Lvgl_WfifBleScanTask(void *srg) {
    char send_lvgl[10] = {""};
    uint8_t ble_scan_count = 0;
    uint8_t ble_mac[6];
    EventBits_t even = xEventGroupWaitBits(wifi_even_,0x02,pdTRUE,pdTRUE,pdMS_TO_TICKS(30000));

    /* Fetch news while Wi-Fi is still up (after scan, connect was started) */
    if (espwifi_wait_ip(12000)) {
        news_service_fetch();
        if (weather_service_fetch()) {
            ApplyWeatherSnapshot("boot");
        }
    }

    /* Do not tear down Wi-Fi until SNTP has produced at least one real sync;
     * otherwise the clock UI stays stuck on placeholders for this session. */
    if (!espwifi_is_time_synced()) {
        ESP_LOGW(TAG, "SNTP still unsynced before BLE phase; waiting up to 30s");
        const int64_t wait_start = NowMs();
        while (!espwifi_is_time_synced() && (NowMs() - wait_start) < 30000) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        ESP_LOGI(TAG, "SNTP wait done synced=%d elapsed=%lld ms",
                 (int)espwifi_is_time_synced(), (long long)(NowMs() - wait_start));
    }

    espwifi_deinit();
    ble_scan_prepare();
    ESP_LOGI(TAG, "Heap before BLE init: free=%" PRIu32 " min_internal=%" PRIu32 " largest_internal=%" PRIu32,
             (uint32_t)esp_get_free_heap_size(),
             (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    ble_stack_init();
    ble_scan_start();
    for(;xQueueReceive(ble_queue,ble_mac,3500) == pdTRUE;) {
        ble_scan_count++;
        if(ble_scan_count >= 20)
        break;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    if (TryLockLvgl()) {
        if(get_bit_data(even,1)) {
            snprintf(send_lvgl,9,"%d",user_esp_bsp.apNum);
            lv_label_set_text(init_ui.screen_label_14, send_lvgl);
        } else {
            lv_label_set_text(init_ui.screen_label_14, "P");
        }
        snprintf(send_lvgl,10,"%d",ble_scan_count);
        lv_label_set_text(init_ui.screen_label_13, send_lvgl);
        Lvgl_unlock();
    }
    ble_stack_deinit();    //释放BLE
    vTaskDelete(NULL);
}

void BOOT_LoopTask(void *arg) {
    bool is_cont4en = 0;
    for(;;) {
        EventBits_t even = xEventGroupWaitBits(BootButtonGroups,(0x01 | 0x02 | 0x04),pdTRUE,pdFALSE,pdMS_TO_TICKS(2000));
        if(even & 0x04) {
            if(0 == is_cont4en) {
                is_cont4en = 1;
                if (TryLockLvgl()) {
                    lv_obj_clear_flag(init_ui.screen_cont_4,LV_OBJ_FLAG_HIDDEN); 
                    lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
                    Lvgl_unlock();
                }
            } else {
                is_cont4en = 0;
                if (TryLockLvgl()) {
                    lv_obj_clear_flag(init_ui.screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
                    lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
                    Lvgl_unlock();
                }
            }
        } else if(even & 0x01) {
            xEventGroupSetBits(CodecGroups,0x02);
        } else if(even & 0x02) {
            xEventGroupSetBits(CodecGroups,0x01);
        }
    }
}

void KEY_LoopTask(void *arg) {
    bool is_cont3en = 0;
    for(;;) {
        EventBits_t even = xEventGroupWaitBits(GP18ButtonGroups,(0x01 | 0x02 | 0x04),pdTRUE,pdFALSE,pdMS_TO_TICKS(2000));
        if(even & 0x01) {
            is_Music = false;
        } else if(even & 0x02) {
            is_Music = true;
            xEventGroupSetBits(CodecGroups,0x04);
        } else if(even & 0x04) {
            if(0 == is_cont3en) {
                is_cont3en = 1;
                if (TryLockLvgl()) {
                    lv_obj_clear_flag(init_ui.screen_cont_3,LV_OBJ_FLAG_HIDDEN); 
                    lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
                    Lvgl_unlock();
                }
            } else {
                is_cont3en = 0;
                if (TryLockLvgl()) {
                    lv_obj_clear_flag(init_ui.screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
                    lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
                    Lvgl_unlock();
                }
            }
        }
    }
}

void Codec_LoopTask(void *arg) {
    bool is_eco = 0;
    for(;;) {
        EventBits_t even = xEventGroupWaitBits(CodecGroups,(0x01 | 0x02 | 0x04),pdTRUE,pdFALSE,pdMS_TO_TICKS(8 * 1000));
		if(even & 0x01)
		{
            if (TryLockLvgl()) {
			    lv_label_set_text(init_ui.screen_label_15, "正在录音");
			    lv_label_set_text(init_ui.screen_label_17, "Recording...");
                Lvgl_unlock();
            }
			codecport->CodecPort_EchoRead(audio_ptr,192 * 1000);
            if (TryLockLvgl()) {
			    lv_label_set_text(init_ui.screen_label_15, "录音完成");
			    lv_label_set_text(init_ui.screen_label_17, "Rec Done");
                Lvgl_unlock();
            }
            is_eco = 1;
		}
		else if(even & 0x02)
		{
            if(1 == is_eco) {
                is_eco = 0;
                if (TryLockLvgl()) {
                    lv_label_set_text(init_ui.screen_label_15, "正在播放");
			        lv_label_set_text(init_ui.screen_label_17, "Playing...");
                    Lvgl_unlock();
                }
			    codecport->CodecPort_PlayWrite(audio_ptr,192 * 1000);
                if (TryLockLvgl()) {
			        lv_label_set_text(init_ui.screen_label_15, "播放完成");
			        lv_label_set_text(init_ui.screen_label_17, "Play Done");
                    Lvgl_unlock();
                }
            }
		}
		else if(even & 0x04)
		{
            if (TryLockLvgl()) {
			    lv_label_set_text(init_ui.screen_label_15, "正在播放音乐");
			    lv_label_set_text(init_ui.screen_label_17, "Play Music");
                Lvgl_unlock();
            }
			codecport->CodecPort_SetSpeakerVol(90);
			uint32_t bytes_sizt;
			size_t bytes_write = 0;
			uint8_t *data_ptr = codecport->CodecPort_GetPcmData(&bytes_sizt);
			while (bytes_write < bytes_sizt)
            {
                codecport->CodecPort_PlayWrite(data_ptr, 256);
                data_ptr += 256;
                bytes_write += 256;
				if(!is_Music)
				break;
			}
			codecport->CodecPort_SetSpeakerVol(100);
            if (TryLockLvgl()) {
			    lv_label_set_text(init_ui.screen_label_15, "播放完成");
			    lv_label_set_text(init_ui.screen_label_17, "Play Done");
                Lvgl_unlock();
            }
		}
		else
		{
            if (TryLockLvgl()) {
			    lv_label_set_text(init_ui.screen_label_15, "等待操作");
			    lv_label_set_text(init_ui.screen_label_17, "Idle");
                Lvgl_unlock();
            }
		}
    }
}

void News_TickerTask(void *arg) {
    const TickType_t refresh_ticks = pdMS_TO_TICKS(NEWS_REFRESH_INTERVAL_MS);
    const TickType_t retry_ticks = pdMS_TO_TICKS(NEWS_RETRY_INTERVAL_MS);
    const TickType_t rotate_ticks = pdMS_TO_TICKS(NEWS_HEADLINE_ROTATE_MS);

    auto periodic_fetch_news = []() -> bool {
        ESP_LOGI(TAG, "News periodic refresh: start");

        int64_t t_start_ms = NowMs();
        espwifi_init();
        LogBlockingMs("espwifi_init", NowMs() - t_start_ms, NEWS_STEP_WARN_MS, true);

        bool ok = false;
        t_start_ms = NowMs();
        const bool got_ip = espwifi_wait_ip(20000);
        LogBlockingMs("espwifi_wait_ip", NowMs() - t_start_ms, NEWS_STEP_WARN_MS, true);

        if (got_ip) {
            t_start_ms = NowMs();
            news_service_fetch();
            LogBlockingMs("news_service_fetch", NowMs() - t_start_ms, NEWS_STEP_WARN_MS, true);
            const size_t rss_cnt = news_service_count();
            ESP_LOGI(TAG, "News periodic refresh: rss got %u item(s)", (unsigned int)rss_cnt);

            t_start_ms = NowMs();
            const bool weather_ok = weather_service_fetch();
            LogBlockingMs("weather_service_fetch", NowMs() - t_start_ms, NEWS_STEP_WARN_MS, true);
            const size_t warn_cnt = weather_service_warn_count();
            if (weather_ok) {
                ApplyWeatherSnapshot("periodic");
            }
            const size_t total_cnt = rss_cnt + warn_cnt;
            ok = total_cnt > 0;
            ESP_LOGI(
                TAG,
                "News periodic refresh: warn=%u total_source=%u",
                (unsigned int)warn_cnt,
                (unsigned int)total_cnt
            );
        } else {
            ESP_LOGW(TAG, "News periodic refresh: no IP");
        }

        t_start_ms = NowMs();
        espwifi_deinit();
        LogBlockingMs("espwifi_deinit", NowMs() - t_start_ms, NEWS_STEP_WARN_MS, true);
        ESP_LOGI(TAG, "News periodic refresh: done (%s)", ok ? "OK" : "FAIL");
        return ok;
    };

    /* Do not start ticker before boot transition enters main screen. */
    while (!s_main_screen_ready) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "NEWS ticker: main screen ready, waiting headline data");

    /* Ticker should only start after we have at least one fetched headline. */
    while (NewsTickerSourceCount() == 0) {
        ESP_LOGW(TAG, "NEWS ticker: no headline yet, retry fetch");
        (void)periodic_fetch_news();
        if (NewsTickerSourceCount() == 0) {
            vTaskDelay(retry_ticks);
        }
    }

    s_news_show = true;

    TickType_t next_refresh_tick = xTaskGetTickCount() + refresh_ticks;
    TickType_t next_rotate_tick = xTaskGetTickCount();
    size_t rotate_idx = 0;
    const size_t boot_cnt = NewsTickerSourceCount();
    ESP_LOGI(TAG, "NEWS ticker boot source_cnt=%u", (unsigned int)boot_cnt);
    if ((boot_cnt > 0) && PostNewsHeadlineByIndex(rotate_idx, "boot_apply")) {
        next_rotate_tick = xTaskGetTickCount() + rotate_ticks;
    } else {
        PostNewsText("無法取得新聞（稍後會自動重試）", "boot_empty", 0, 0);
    }

    for (;;) {
        TickType_t now_tick = xTaskGetTickCount();
        if ((int32_t)(now_tick - next_refresh_tick) >= 0) {
            PostNewsText("新聞更新中...", "refresh_start", 0, NewsTickerSourceCount());

            const bool refreshed = periodic_fetch_news();
            const size_t new_cnt = NewsTickerSourceCount();
            ESP_LOGI(TAG, "NEWS ticker refresh source_cnt=%u", (unsigned int)new_cnt);
            if (new_cnt > 0) {
                rotate_idx = 0;
                PostNewsHeadlineByIndex(rotate_idx, "refresh_apply");
                next_rotate_tick = xTaskGetTickCount() + rotate_ticks;
            } else {
                PostNewsText("無法取得新聞（稍後會自動重試）", "refresh_empty", 0, new_cnt);
            }
            next_refresh_tick = xTaskGetTickCount() + (refreshed ? refresh_ticks : retry_ticks);
            ESP_LOGI(
                TAG,
                "NEWS refresh done refreshed=%d new_cnt=%u next_in_ms=%lu",
                refreshed ? 1 : 0,
                (unsigned int)new_cnt,
                (unsigned long)(refreshed ? NEWS_REFRESH_INTERVAL_MS : NEWS_RETRY_INTERVAL_MS)
            );
        }

        now_tick = xTaskGetTickCount();
        const size_t cnt = NewsTickerSourceCount();
        if ((cnt > 1) && ((int32_t)(now_tick - next_rotate_tick) >= 0)) {
            rotate_idx = (rotate_idx + 1) % cnt;
            if (PostNewsHeadlineByIndex(rotate_idx, "rotate")) {
                next_rotate_tick = now_tick + rotate_ticks;
            } else {
                rotate_idx = 0;
                next_rotate_tick = now_tick + pdMS_TO_TICKS(1000);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NEWS_LOOP_SLEEP_MS));
    }
}

void UserApp_AppInit() {
    BootStatusPush("開機中...");
    audio_ptr = (uint8_t *)heap_caps_malloc(288 * 1000 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(audio_ptr);
    Adc_PortInit();
    Custom_ButtonInit();
    Rtc_Setup(&I2cbus,0x51);

    // RTC hardware time is intentionally NOT used to seed the system clock.
    // UI must only display time once SNTP has confirmed a real sync; until then
    // the clock row shows placeholders via espwifi_is_time_synced().
    rtcTimeStruct_t rtcNow;
    Rtc_GetTime(&rtcNow);
    if (rtcNow.year < 2025 || rtcNow.year > 2099) {
        Rtc_SetTime(2026, 1, 5, 14, 30, 30);
        Rtc_GetTime(&rtcNow);
    }
    shtc3port = new Shtc3Port(I2cbus);
    BootStatusPush("Wi-Fi 初始化中...");
    espwifi_init();
    BootStatusPush("Wi-Fi 連線中...");
    bool wifi_ready = espwifi_wait_ip(20000);
    if (wifi_ready) {
        BootStatusPush("Wi-Fi 已連線: %s", (user_esp_bsp._ip[0] != '\0') ? user_esp_bsp._ip : "unknown");
    } else {
        BootStatusPush("Wi-Fi 連線超時");
    }

    setenv("TZ", "GMT-8", 1); // POSIX TZ reversed sign: GMT-8 means GMT+8.
    tzset();
    ESP_LOGI(TAG, "Timezone configured: %s (GMT+8)", getenv("TZ"));
    LogCurrentLocalTime("Local time before SNTP: ");

    const int MAX_SNTP_ATTEMPTS = 6;
    bool is_ntp_synced = false;
    for (int attempt = 1; attempt <= MAX_SNTP_ATTEMPTS && !is_ntp_synced; attempt++) {
        if (!wifi_ready) {
            BootStatusPush("Wi-Fi 重連中 (%d/%d)", attempt, MAX_SNTP_ATTEMPTS);
            ESP_LOGW(TAG, "SNTP attempt %d: Wi-Fi not ready, retrying connect", attempt);
            esp_wifi_connect();
            wifi_ready = espwifi_wait_ip(15000);
        }
        if (wifi_ready) {
            BootStatusPush("SNTP 同步中 (%d/%d)", attempt, MAX_SNTP_ATTEMPTS);
            is_ntp_synced = espwifi_sync_time("time.asia.apple.com", 30000);
            if (!is_ntp_synced) {
                ESP_LOGW(TAG, "SNTP attempt %d/%d failed", attempt, MAX_SNTP_ATTEMPTS);
            }
        } else {
            BootStatusPush("Wi-Fi 未連線，稍候重試 (%d/%d)", attempt, MAX_SNTP_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
    ESP_LOGI(TAG, "SNTP result: %s", is_ntp_synced ? "SUCCESS" : "FAILED");
    LogCurrentLocalTime("Local time after SNTP: ");

    if (is_ntp_synced) {
        BootStatusPush("SNTP 同步成功");
        time_t now = 0;
        struct tm timeinfo = {0};
        time(&now);
        localtime_r(&now, &timeinfo);
        Rtc_SetTime(
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec
        );
        ESP_LOGI(TAG, "RTC updated from SNTP time");
    } else {
        BootStatusPush("SNTP 屢次失敗，將重新開機...");
        ESP_LOGE(TAG, "SNTP sync failed after %d attempts, rebooting", MAX_SNTP_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    CodecGroups = xEventGroupCreate();
    codecport = new CodecPort(I2cbus,"S3_RLCD_4_2");
    codecport->CodecPort_SetInfo("es8311 & es7210",1,16000,2,16);
    codecport->CodecPort_SetSpeakerVol(100);
    codecport->CodecPort_SetMicGain(35);
    BootStatusPush("開機完成，進入主畫面...");
}

void UserApp_UiInit() {
    setup_ui(&init_ui);

    // Use fixed slots per character to eliminate horizontal jitter on second updates.
    SetupClockTimeSlots();

    static const uint32_t date_probe[] = {0x9031, 0x6708, 0x65E5, 0x4E8C}; // 週月日二
    static const uint32_t news_probe[] = {0x65B0, 0x805E, 0x9023, 0x7DB2}; // 新聞連網
    const lv_font_t *date_font = &lv_font_notosans_tc_50;
    const lv_font_t *news_font = &lv_font_news_tc_16;

    if (!FontHasGlyphs(date_font, date_probe, sizeof(date_probe) / sizeof(date_probe[0]))) {
        ESP_LOGW(TAG, "Date font missing Chinese glyphs, keep lv_font_notosans_tc_50");
    } else {
        ESP_LOGI(TAG, "Date font ready: lv_font_notosans_tc_50");
    }
    if (!FontHasGlyphs(news_font, news_probe, sizeof(news_probe) / sizeof(news_probe[0]))) {
        ESP_LOGW(TAG, "News font missing Chinese glyphs, keep lv_font_news_tc_16");
    } else {
        ESP_LOGI(TAG, "News font ready: lv_font_news_tc_16");
    }

    // Date line under clock.
    // lv_font_notosans_tc_50 has large line metrics (line_height=95, baseline=28),
    // so compensate by moving the label up and giving enough height.
    lv_obj_set_pos(init_ui.screen_label_4, 0, 108);
    lv_obj_set_size(init_ui.screen_label_4, 400, 70);
    lv_label_set_long_mode(init_ui.screen_label_4, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(init_ui.screen_label_4, date_font, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(init_ui.screen_label_4, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(init_ui.screen_label_4, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(init_ui.screen_label_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(init_ui.screen_label_4, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(init_ui.screen_label_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(init_ui.screen_label_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(init_ui.screen_label_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(init_ui.screen_label_4, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Indoor weather + battery row (moved below HKO row).
    // Swap indoor temperature/humidity positions: temp on left, humidity on right.
    lv_obj_set_pos(init_ui.screen_img_4, 118, 228);
    lv_obj_set_pos(init_ui.screen_label_12, 152, 232);
    lv_obj_set_size(init_ui.screen_label_12, 70, 24);
    lv_label_set_text(init_ui.screen_label_12, "--°");

    lv_obj_set_pos(init_ui.screen_img_3, 222, 228);
    lv_obj_set_pos(init_ui.screen_label_11, 256, 232);
    lv_obj_set_size(init_ui.screen_label_11, 70, 24);
    lv_label_set_text(init_ui.screen_label_11, "--%");

    // Keep battery icon/value on the same row as indoor weather.
    lv_obj_clear_flag(init_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(init_ui.screen_img_1, 304, 228);
    lv_obj_set_pos(init_ui.screen_label_7, 336, 232);
    lv_obj_set_size(init_ui.screen_label_7, 60, 24);
    lv_obj_set_style_text_align(init_ui.screen_label_7, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Wong Tai Sin API row (moved above indoor row).
    lv_obj_add_flag(init_ui.screen_label_8, LV_OBJ_FLAG_HIDDEN);
    if (s_hko_bar == NULL) {
        s_hko_bar = lv_obj_create(init_ui.screen_cont_2);
        lv_obj_set_pos(s_hko_bar, 0, 178);
        lv_obj_set_size(s_hko_bar, 400, 48);
        lv_obj_clear_flag(s_hko_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scrollbar_mode(s_hko_bar, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_bg_opa(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(s_hko_bar, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(s_hko_bar, lv_color_hex(0xffffff), LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_radius(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_width(s_hko_bar, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

        s_hko_label = lv_label_create(s_hko_bar);
        lv_obj_set_pos(s_hko_label, 6, 0);
        lv_obj_set_size(s_hko_label, 388, 48);
        lv_label_set_long_mode(s_hko_label, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(s_hko_label, date_font, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(s_hko_label, lv_color_hex(0x000000), LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(s_hko_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_text_letter_space(s_hko_label, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_text_line_space(s_hko_label, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(s_hko_label, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(s_hko_label, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(s_hko_label, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(s_hko_label, 0, LV_PART_MAIN|LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(s_hko_label, 0, LV_PART_MAIN|LV_STATE_DEFAULT);

        ESP_LOGI(TAG, "HKO bar created at (0,178) size=400x48");
    }
    lv_label_set_text(s_hko_label, "天氣 -° -%");
    lv_obj_move_foreground(s_hko_bar);

    // News ticker: rotate headlines one-by-one in a dedicated bottom bar.
    SetupNewsTickerAnimated(news_font);

    // Hide unrelated status/test labels on the clock page.
    lv_obj_add_flag(init_ui.screen_label_5, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_6, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_9, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_10, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_13, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_14, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(init_ui.screen_label_15, "等待操作");
}

void UserApp_TaskInit() {
    xTaskCreatePinnedToCore(Lvgl_Cont1Task, "Lvgl_Cont1Task", 4 * 1024, NULL, 2, NULL, UI_TASK_CORE_ID);
    xTaskCreatePinnedToCore(Lvgl_UserTask, "Lvgl_UserTask", 5 * 1024, NULL, 4, NULL, UI_TASK_CORE_ID);

    xTaskCreatePinnedToCore(Status_PollTask, "Status_PollTask", 4 * 1024, NULL, 1, NULL, BG_TASK_CORE_ID);
    xTaskCreatePinnedToCore(Lvgl_WfifBleScanTask, "Lvgl_WfifBleScanTask", 4 * 1024, NULL, 2, NULL, BG_TASK_CORE_ID);
    xTaskCreatePinnedToCore(BOOT_LoopTask, "BOOT_LoopTask", 4 * 1024, NULL, 2, NULL, BG_TASK_CORE_ID);
    xTaskCreatePinnedToCore(KEY_LoopTask, "KEY_LoopTask", 4 * 1024, NULL, 2, NULL, BG_TASK_CORE_ID);
    xTaskCreatePinnedToCore(Codec_LoopTask, "Codec_LoopTask", 4 * 1024, NULL, 2, NULL, BG_TASK_CORE_ID);
    xTaskCreatePinnedToCore(News_TickerTask, "News_TickerTask", 4 * 1024, NULL, 1, NULL, BG_TASK_CORE_ID);
}
