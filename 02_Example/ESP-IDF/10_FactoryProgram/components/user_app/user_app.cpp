#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include "button_bsp.h"
#include "user_app.h"
#include "gui_guider.h"
#include "i2c_equipment.h"
#include "i2c_bsp.h"
#include "sdcard_bsp.h"
#include "codec_bsp.h"
#include "adc_bsp.h"
#include "esp_wifi_bsp.h"
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
CustomSDPort *sdcardPort = NULL;
Shtc3Port *shtc3port = NULL;
EventGroupHandle_t CodecGroups;
CodecPort *codecport = NULL;
static uint8_t *audio_ptr = NULL;
static bool is_Music = true;
static lv_obj_t *s_clock_char_labels[8] = {0};
static const char *TAG = "user_app";
static char s_clock_last_text[9] = {'\0'};
static const uint32_t CLOCK_POLL_MS = 50;
static const uint32_t STATUS_POLL_MS = 100;
static const uint32_t BATTERY_INTERVAL_MS = 2000;
static const uint32_t SENSOR_INTERVAL_MS = 5000;
static const uint32_t NEWS_REFRESH_INTERVAL_MS = 30UL * 60UL * 1000UL;
static const uint32_t NEWS_RETRY_INTERVAL_MS = 60UL * 1000UL;
static const uint32_t NEWS_LOOP_SLEEP_MS = 500;
static const int64_t UI_LOOP_GAP_WARN_MS = 250;
static const int64_t UI_LOOP_BODY_WARN_MS = 80;
static const int64_t ADC_BLOCK_WARN_MS = 15;
static const int64_t SENSOR_BLOCK_WARN_MS = 60;
static const int64_t NEWS_STEP_WARN_MS = 500;
static const int NEWS_BAR_X = 0;
static const int NEWS_BAR_Y = 260;
static const int NEWS_BAR_W = 400;
static const int NEWS_BAR_H = 40;
static lv_obj_t *s_news_anim_box = NULL;
static lv_obj_t *s_news_marquee_label = NULL;
static lv_obj_t *s_hko_bar = NULL;
static lv_obj_t *s_hko_label = NULL;
static lv_anim_t s_news_anim_template;
static lv_style_t s_news_anim_style;
static bool s_news_anim_style_inited = false;
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

static void NewsBarApplyTextAnimated(const char *text);
static bool TryLockLvgl(void);
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
    if (Lvgl_lock(30)) {
        return true;
    }
    ESP_LOGW(TAG, "LVGL lock timeout");
    return false;
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

static char *BuildNewsTickerText(size_t *used_items)
{
    static const char *sep = "   ｜   ";
    const size_t sep_len = strlen(sep);
    const size_t cnt = news_service_count();
    char title[NEWS_TITLE_LEN] = {0};
    size_t appended = 0;
    size_t total_len = 1; // trailing '\0'

    for (size_t i = 0; i < cnt; i++) {
        if (!news_service_get(i, title, sizeof(title))) {
            continue;
        }
        if (title[0] == '\0') {
            continue;
        }
        total_len += strlen(title);
        if (appended > 0) {
            total_len += sep_len;
        }
        appended++;
    }

    if (used_items != NULL) {
        *used_items = appended;
    }
    if (appended == 0) {
        return NULL;
    }

    char *out = (char *)heap_caps_malloc(total_len, MALLOC_CAP_SPIRAM);
    if (out == NULL) {
        out = (char *)malloc(total_len);
    }
    if (out == NULL) {
        ESP_LOGE(TAG, "NEWS ticker alloc failed len=%u", (unsigned int)total_len);
        return NULL;
    }

    out[0] = '\0';
    appended = 0;
    for (size_t i = 0; i < cnt; i++) {
        if (!news_service_get(i, title, sizeof(title))) {
            continue;
        }
        if (title[0] == '\0') {
            continue;
        }
        if (appended > 0) {
            strlcat(out, sep, total_len);
        }
        strlcat(out, title, total_len);
        appended++;
    }

    return out;
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
    ESP_LOGI(
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

static void NewsBarApplyTextAnimated(const char *text)
{
    if (text == NULL) {
        text = "";
    }

    if (s_news_marquee_label == NULL) {
        lv_label_set_text(init_ui.screen_news_label, text);
        return;
    }
    lv_label_set_text(s_news_marquee_label, text);
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
    lv_obj_set_style_bg_color(s_news_anim_box, lv_color_hex(0x1a3a5c), LV_PART_MAIN | LV_STATE_DEFAULT);
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
    lv_label_set_text(s_news_marquee_label, "");
    lv_label_set_long_mode(s_news_marquee_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_size(s_news_marquee_label, NEWS_BAR_W - 12, NEWS_BAR_H);
    lv_obj_set_pos(s_news_marquee_label, 6, 0);
    lv_obj_set_style_border_width(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(s_news_marquee_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(s_news_marquee_label, news_font, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(s_news_marquee_label, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(s_news_marquee_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_letter_space(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(s_news_marquee_label, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(s_news_marquee_label, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Follow LVGL label docs: apply an animation template to control circular scroll cadence.
    if (!s_news_anim_style_inited) {
        lv_anim_init(&s_news_anim_template);
        lv_anim_set_delay(&s_news_anim_template, 800);
        lv_anim_set_repeat_delay(&s_news_anim_template, 1400);
        lv_anim_set_repeat_count(&s_news_anim_template, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_time(&s_news_anim_template, 22000);
        lv_style_init(&s_news_anim_style);
        lv_style_set_anim(&s_news_anim_style, &s_news_anim_template);
        s_news_anim_style_inited = true;
    }
    lv_obj_add_style(s_news_marquee_label, &s_news_anim_style, LV_PART_MAIN | LV_STATE_DEFAULT);
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
    if (TryLockLvgl()) {
        lv_obj_clear_flag(init_ui.screen_label_1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_label_2, LV_OBJ_FLAG_HIDDEN);
        Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (TryLockLvgl()) {
        lv_obj_clear_flag(init_ui.screen_label_2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_label_1, LV_OBJ_FLAG_HIDDEN);
        Lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    if (TryLockLvgl()) {
        lv_obj_clear_flag(init_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
        Lvgl_unlock();
    }
    vTaskDelete(NULL); 
}

void Lvgl_UserTask(void *arg) {
    time_t last_display_epoch = (time_t)-1;
    int last_date_key = -1;
    int last_minute_key = -1;
    char lvgl_buffer[30] = {""};
    TickType_t last_wake_tick = xTaskGetTickCount();
    int64_t last_loop_start_ms = 0;

    for(;;) {
        const int64_t loop_start_ms = NowMs();
        if (last_loop_start_ms != 0) {
            const int64_t loop_gap_ms = loop_start_ms - last_loop_start_ms;
            if (loop_gap_ms >= UI_LOOP_GAP_WARN_MS) {
                ESP_LOGW(TAG, "UI loop gap=%lld ms (clock may freeze)", (long long)loop_gap_ms);
            }
        }
        last_loop_start_ms = loop_start_ms;

        bool need_refresh = false;
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

            if (battery_level >= 0) {
                snprintf(lvgl_buffer,30,"%d%%",battery_level);
                lv_label_set_text(init_ui.screen_label_7, lvgl_buffer);
            }
            if (sensor_humidity_percent >= 0) {
                snprintf(lvgl_buffer,30,"%d%%",sensor_humidity_percent);
                lv_label_set_text(init_ui.screen_label_11, lvgl_buffer);
            }
            if (sensor_temperature_c > -1000) {
                snprintf(lvgl_buffer,30,"%d°",sensor_temperature_c);
                lv_label_set_text(init_ui.screen_label_12, lvgl_buffer);
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
                if (s_hko_label != NULL) {
                    lv_label_set_text(s_hko_label, env_line);
                    if (s_hko_bar != NULL) {
                        lv_obj_move_foreground(s_hko_bar);
                    }
                } else {
                    lv_label_set_text(init_ui.screen_label_8, env_line);
                    lv_obj_move_foreground(init_ui.screen_label_8);
                }
            }
        }

        // Update clock once per second from system clock.
        {
            static const char * const weekday_zh[] = {
                "週日","週一","週二","週三","週四","週五","週六"
            };
            struct tm display_tm = {0};
            time_t now_epoch = 0;

            time(&now_epoch);

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
            ESP_LOGI(TAG, "NEWS apply #%lu text=\"%.64s\"", (unsigned long)s_news_post_seq, s_news_buf);
        }

        Lvgl_unlock();
        if (need_refresh) {
            Lvgl_request_refresh();
        }

        const int64_t loop_body_ms = NowMs() - loop_start_ms;
        if (loop_body_ms >= UI_LOOP_BODY_WARN_MS) {
            ESP_LOGW(TAG, "UI loop body took %lld ms", (long long)loop_body_ms);
        }
        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(CLOCK_POLL_MS));
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
            s_battery_level = (int)Adc_GetBatteryLevel();
            LogBlockingMs("Adc_GetBatteryLevel", NowMs() - t_start_ms, ADC_BLOCK_WARN_MS, false);
            s_status_dirty = true;
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

void Lvgl_SDcardTask(void *arg) {
    const char *str_write = "waveshare.com";
    char str_read[20] = {""};
    if(0 == sdcardPort->SDPort_GetStatus()) {
        if (TryLockLvgl()) {
            lv_label_set_text(init_ui.screen_label_6, "No Card");
            Lvgl_unlock();
        }
    } else {
        sdcardPort->SDPort_WriteFile("/sdcard/sdcard.txt",str_write,strlen(str_write));
        sdcardPort->SDPort_ReadFile("/sdcard/sdcard.txt",(uint8_t *)str_read,NULL);
        if (TryLockLvgl()) {
            if(!strcmp(str_write,str_read)) {
                lv_label_set_text(init_ui.screen_label_6, "passed");
            } else {
                lv_label_set_text(init_ui.screen_label_6, "failed");
            }
            Lvgl_unlock();
        }
    }
    vTaskDelete(NULL);
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
    espwifi_deinit();
    ble_scan_prepare();
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
            ok = news_service_count() > 0;
            ESP_LOGI(TAG, "News periodic refresh: got %u item(s)", (unsigned int)news_service_count());

            t_start_ms = NowMs();
            const bool weather_ok = weather_service_fetch();
            LogBlockingMs("weather_service_fetch", NowMs() - t_start_ms, NEWS_STEP_WARN_MS, true);
            if (weather_ok) {
                ApplyWeatherSnapshot("periodic");
            }
        } else {
            ESP_LOGW(TAG, "News periodic refresh: no IP");
        }

        t_start_ms = NowMs();
        espwifi_deinit();
        LogBlockingMs("espwifi_deinit", NowMs() - t_start_ms, NEWS_STEP_WARN_MS, true);
        ESP_LOGI(TAG, "News periodic refresh: done (%s)", ok ? "OK" : "FAIL");
        return ok;
    };

    /* Signal Lvgl_UserTask to show bar with loading message */
    PostNewsText("正在連接網絡，抓取新聞中...", "boot_wait", 0, 0);
    s_news_show  = true;

    /* Wait up to 60 s for news_service_fetch() to complete */
    uint8_t waited = 0;
    while (news_service_count() == 0 && waited < 60) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited++;
    }

    TickType_t next_refresh_tick = xTaskGetTickCount() + ((news_service_count() > 0) ? refresh_ticks : 0);
    size_t used_items = 0;
    char *ticker_text = BuildNewsTickerText(&used_items);
    ESP_LOGI(TAG, "NEWS ticker boot rss_cnt=%u ticker_items=%u", (unsigned int)news_service_count(), (unsigned int)used_items);
    if ((ticker_text != NULL) && (used_items > 0)) {
        PostNewsText(ticker_text, "boot_apply", 0, used_items);
        free(ticker_text);
    } else {
        PostNewsText("無法取得新聞（稍後會自動重試）", "boot_empty", 0, 0);
    }

    for (;;) {
        TickType_t now_tick = xTaskGetTickCount();
        if ((int32_t)(now_tick - next_refresh_tick) >= 0) {
            PostNewsText("新聞更新中...", "refresh_start", 0, news_service_count());

            const bool refreshed = periodic_fetch_news();
            const size_t new_cnt = news_service_count();
            ticker_text = BuildNewsTickerText(&used_items);
            ESP_LOGI(TAG, "NEWS ticker refresh rss_cnt=%u ticker_items=%u", (unsigned int)new_cnt, (unsigned int)used_items);
            if ((ticker_text != NULL) && (used_items > 0)) {
                PostNewsText(ticker_text, "refresh_apply", 0, used_items);
                free(ticker_text);
            } else {
                PostNewsText("無法取得新聞（稍後會自動重試）", "refresh_empty", 0, new_cnt);
            }
            now_tick = xTaskGetTickCount();
            next_refresh_tick = now_tick + (refreshed ? refresh_ticks : retry_ticks);
            ESP_LOGI(
                TAG,
                "NEWS refresh done refreshed=%d new_cnt=%u next_in_ms=%lu",
                refreshed ? 1 : 0,
                (unsigned int)new_cnt,
                (unsigned long)(refreshed ? NEWS_REFRESH_INTERVAL_MS : NEWS_RETRY_INTERVAL_MS)
            );
        }
        vTaskDelay(pdMS_TO_TICKS(NEWS_LOOP_SLEEP_MS));
    }
}

void UserApp_AppInit() {
    BootStatusPush("開機中...");
    audio_ptr = (uint8_t *)heap_caps_malloc(288 * 1000 * sizeof(uint8_t), MALLOC_CAP_SPIRAM);
    assert(audio_ptr);
    sdcardPort = new CustomSDPort("/sdcard");
    Adc_PortInit();
    Custom_ButtonInit();
    Rtc_Setup(&I2cbus,0x51);

    // Only reset RTC when year is clearly invalid; preserve time across reboots
    rtcTimeStruct_t rtcNow;
    Rtc_GetTime(&rtcNow);
    if (rtcNow.year < 2025 || rtcNow.year > 2099) {
        Rtc_SetTime(2026, 1, 5, 14, 30, 30);
        Rtc_GetTime(&rtcNow);
    }

    // Sync system clock from hardware RTC so time()/localtime_r() work correctly
    struct tm tm_rtc = {
        .tm_sec   = rtcNow.second,
        .tm_min   = rtcNow.minute,
        .tm_hour  = rtcNow.hour,
        .tm_mday  = rtcNow.day,
        .tm_mon   = rtcNow.month - 1,
        .tm_year  = rtcNow.year - 1900,
        .tm_wday  = rtcNow.week,
        .tm_yday  = 0,
        .tm_isdst = -1,
    };
    struct timeval tv = { .tv_sec = mktime(&tm_rtc), .tv_usec = 0 };
    settimeofday(&tv, NULL);
    shtc3port = new Shtc3Port(I2cbus);
    BootStatusPush("Wi-Fi 初始化中...");
    espwifi_init();
    BootStatusPush("Wi-Fi 連線中...");
    const bool wifi_ready = espwifi_wait_ip(20000);
    if (wifi_ready) {
        BootStatusPush("Wi-Fi 已連線: %s", (user_esp_bsp._ip[0] != '\0') ? user_esp_bsp._ip : "unknown");
    } else {
        BootStatusPush("Wi-Fi 連線超時");
    }

    setenv("TZ", "GMT-8", 1); // POSIX TZ reversed sign: GMT-8 means GMT+8.
    tzset();
    ESP_LOGI(TAG, "Timezone configured: %s (GMT+8)", getenv("TZ"));
    LogCurrentLocalTime("Local time before SNTP: ");

    bool is_ntp_synced = false;
    if (wifi_ready) {
        BootStatusPush("SNTP 同步中: time.asia.apple.com");
        is_ntp_synced = espwifi_sync_time("time.asia.apple.com", 30000);
    } else {
        BootStatusPush("SNTP 略過: 未連上 Wi-Fi");
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
        BootStatusPush("SNTP 同步失敗，使用 RTC");
        ESP_LOGW(TAG, "Use RTC time fallback");
        ESP_LOGW(
            TAG,
            "RTC fallback value: %04d-%02d-%02d %02d:%02d:%02d",
            rtcNow.year,
            rtcNow.month,
            rtcNow.day,
            rtcNow.hour,
            rtcNow.minute,
            rtcNow.second
        );
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
        lv_obj_set_style_text_align(s_hko_label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN|LV_STATE_DEFAULT);
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

    // News ticker: one-line marquee (right-to-left) in a dedicated bottom bar.
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
    xTaskCreatePinnedToCore(Lvgl_Cont1Task, "Lvgl_Cont1Task", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(Lvgl_UserTask, "Lvgl_UserTask", 5 * 1024, NULL, 4, NULL,1);
    xTaskCreatePinnedToCore(Status_PollTask, "Status_PollTask", 4 * 1024, NULL, 1, NULL,1);
    xTaskCreatePinnedToCore(Lvgl_SDcardTask, "Lvgl_SDcardTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(Lvgl_WfifBleScanTask, "Lvgl_WfifBleScanTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(BOOT_LoopTask, "BOOT_LoopTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(KEY_LoopTask, "KEY_LoopTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(Codec_LoopTask,   "Codec_LoopTask",   4 * 1024, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(News_TickerTask,  "News_TickerTask",  4 * 1024, NULL, 1, NULL, 1);
}
