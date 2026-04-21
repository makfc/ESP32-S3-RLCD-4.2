#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
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
#include "lvgl_bsp.h"

static lv_ui init_ui;
I2cMasterBus I2cbus(14,13,0);

/* News ticker inter-task communication (News_TickerTask → Lvgl_UserTask) */
static volatile bool s_news_show  = false;
static volatile bool s_news_dirty = false;
static char          s_news_buf[NEWS_TITLE_LEN];
CustomSDPort *sdcardPort = NULL;
Shtc3Port *shtc3port = NULL;
EventGroupHandle_t CodecGroups;
CodecPort *codecport = NULL;
static uint8_t *audio_ptr = NULL;
static bool is_Music = true;
static lv_obj_t *s_clock_char_labels[8] = {0};
static const char *TAG = "user_app";
static const int32_t CLOCK_PIPELINE_COMP_US = 150000; // compensate LCD flush latency (~100ms+)
static const uint32_t CLOCK_POLL_MS = 20;

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

    for (int i = 0; i < 8; i++) {
        if (s_clock_char_labels[i] != NULL) {
            char one_char[2] = {' ', '\0'};
            if (time_text[i] != '\0') {
                one_char[0] = time_text[i];
            }
            lv_label_set_text(s_clock_char_labels[i], one_char);
        }
    }
}

static void SetupClockTimeSlots(void)
{
    const int digit_width = 56;
    const int colon_width = 24;
    const int start_x = 8;
    const int start_y = 0;
    const int slot_height = 112;
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
        lv_obj_set_style_bg_opa(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(slot, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

        char one_char[2] = {placeholder[i], '\0'};
        lv_label_set_text(slot, one_char);
        x += slot_width;
    }
}

void Lvgl_Cont1Task(void *arg) {
    lv_obj_clear_flag(init_ui.screen_label_1,LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(init_ui.screen_label_2, LV_OBJ_FLAG_HIDDEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    lv_obj_clear_flag(init_ui.screen_label_2,LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(init_ui.screen_label_1, LV_OBJ_FLAG_HIDDEN);
    vTaskDelay(pdMS_TO_TICKS(1500));
    lv_obj_clear_flag(init_ui.screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
    lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
    vTaskDelete(NULL); 
}

void Lvgl_UserTask(void *arg) {
    time_t last_display_epoch = (time_t)-1;
    char lvgl_buffer[30] = {""};
    TickType_t last_wake_tick = xTaskGetTickCount();
    TickType_t next_adc_tick = last_wake_tick + pdMS_TO_TICKS(2000);
    TickType_t next_shtc3_tick = last_wake_tick + pdMS_TO_TICKS(5000);

    for(;;) {
        TickType_t now_tick = xTaskGetTickCount();

        if ((int32_t)(now_tick - next_adc_tick) >= 0) {
            next_adc_tick = now_tick + pdMS_TO_TICKS(2000);
            uint8_t level = Adc_GetBatteryLevel();
            snprintf(lvgl_buffer,30,"%d%%",level);
            lv_label_set_text(init_ui.screen_label_7, lvgl_buffer);
        }

        // Update clock on second boundary with a small pre-advance to hide LCD pipeline latency.
        {
            static const char * const weekday_zh[] = {
                "週日","週一","週二","週三","週四","週五","週六"
            };
            struct timeval tv_now = {0};
            struct tm raw_tm = {0};
            struct tm display_tm = {0};

            gettimeofday(&tv_now, NULL);
            localtime_r(&tv_now.tv_sec, &raw_tm);

            const bool pre_advance = (tv_now.tv_usec >= (1000000 - CLOCK_PIPELINE_COMP_US));
            const time_t display_epoch = tv_now.tv_sec + (pre_advance ? 1 : 0);

            if (display_epoch != last_display_epoch) {
                last_display_epoch = display_epoch;
                localtime_r(&display_epoch, &display_tm);
                snprintf(
                    lvgl_buffer,
                    sizeof(lvgl_buffer),
                    "%02d:%02d:%02d",
                    display_tm.tm_hour,
                    display_tm.tm_min,
                    display_tm.tm_sec
                );
                SetClockTimeText(lvgl_buffer);
                snprintf(lvgl_buffer, 30, "%d月%d日 %s",
                         display_tm.tm_mon + 1, display_tm.tm_mday,
                         weekday_zh[display_tm.tm_wday]);
                lv_label_set_text(init_ui.screen_label_4, lvgl_buffer);
                Lvgl_request_refresh();
                ESP_LOGI(
                    TAG,
                    "CLK draw raw=%02d:%02d:%02d.%03ld ui=%02d:%02d:%02d pre=%d",
                    raw_tm.tm_hour,
                    raw_tm.tm_min,
                    raw_tm.tm_sec,
                    (long)(tv_now.tv_usec / 1000),
                    display_tm.tm_hour,
                    display_tm.tm_min,
                    display_tm.tm_sec,
                    pre_advance ? 1 : 0
                );
            }
        }

        if (s_news_show) {
            s_news_show = false;
            lv_obj_clear_flag(init_ui.screen_news_label, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_news_dirty) {
            s_news_dirty = false;
            lv_label_set_text(init_ui.screen_news_label, s_news_buf);
        }

        if ((int32_t)(now_tick - next_shtc3_tick) >= 0)
        {
            next_shtc3_tick = now_tick + pdMS_TO_TICKS(5000);
            float rh,temp;
            shtc3port->Shtc3_ReadTempHumi(&temp,&rh);
            snprintf(lvgl_buffer,30,"%d%%",(int)rh);
            lv_label_set_text(init_ui.screen_label_11, lvgl_buffer);
            snprintf(lvgl_buffer,30,"%d°",(int)temp);
            lv_label_set_text(init_ui.screen_label_12, lvgl_buffer);
        }
        vTaskDelayUntil(&last_wake_tick, pdMS_TO_TICKS(CLOCK_POLL_MS));
    }
}

void Lvgl_SDcardTask(void *arg) {
    const char *str_write = "waveshare.com";
    char str_read[20] = {""};
    if(0 == sdcardPort->SDPort_GetStatus()) {
        lv_label_set_text(init_ui.screen_label_6, "No Card");
    } else {
        sdcardPort->SDPort_WriteFile("/sdcard/sdcard.txt",str_write,strlen(str_write));
        sdcardPort->SDPort_ReadFile("/sdcard/sdcard.txt",(uint8_t *)str_read,NULL);
        if(!strcmp(str_write,str_read)) {
            lv_label_set_text(init_ui.screen_label_6, "passed");
        } else {
            lv_label_set_text(init_ui.screen_label_6, "failed");
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
    if(get_bit_data(even,1)) {
        snprintf(send_lvgl,9,"%d",user_esp_bsp.apNum);
        lv_label_set_text(init_ui.screen_label_14, send_lvgl);
    } else {
        lv_label_set_text(init_ui.screen_label_14, "P");
    }
    snprintf(send_lvgl,10,"%d",ble_scan_count);
    lv_label_set_text(init_ui.screen_label_13, send_lvgl);
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
                lv_obj_clear_flag(init_ui.screen_cont_4,LV_OBJ_FLAG_HIDDEN); 
                lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
            } else {
                is_cont4en = 0;
                lv_obj_clear_flag(init_ui.screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
                lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
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
                lv_obj_clear_flag(init_ui.screen_cont_3,LV_OBJ_FLAG_HIDDEN); 
                lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_2, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
            } else {
                is_cont3en = 0;
                lv_obj_clear_flag(init_ui.screen_cont_2,LV_OBJ_FLAG_HIDDEN); 
                lv_obj_add_flag(init_ui.screen_cont_1, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_3, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(init_ui.screen_cont_4, LV_OBJ_FLAG_HIDDEN);
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
			lv_label_set_text(init_ui.screen_label_15, "正在录音");
			lv_label_set_text(init_ui.screen_label_17, "Recording...");
			codecport->CodecPort_EchoRead(audio_ptr,192 * 1000);
			lv_label_set_text(init_ui.screen_label_15, "录音完成");
			lv_label_set_text(init_ui.screen_label_17, "Rec Done");
            is_eco = 1;
		}
		else if(even & 0x02)
		{
            if(1 == is_eco) {
                is_eco = 0;
                lv_label_set_text(init_ui.screen_label_15, "正在播放");
			    lv_label_set_text(init_ui.screen_label_17, "Playing...");
			    codecport->CodecPort_PlayWrite(audio_ptr,192 * 1000);
			    lv_label_set_text(init_ui.screen_label_15, "播放完成");
			    lv_label_set_text(init_ui.screen_label_17, "Play Done");
            }
		}
		else if(even & 0x04)
		{
			lv_label_set_text(init_ui.screen_label_15, "正在播放音乐");
			lv_label_set_text(init_ui.screen_label_17, "Play Music");
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
			lv_label_set_text(init_ui.screen_label_15, "播放完成");
			lv_label_set_text(init_ui.screen_label_17, "Play Done");
		}
		else
		{
			lv_label_set_text(init_ui.screen_label_15, "等待操作");
			lv_label_set_text(init_ui.screen_label_17, "Idle");
		}
    }
}

void News_TickerTask(void *arg) {
    /* Signal Lvgl_UserTask to show bar with loading message */
    strncpy(s_news_buf, "正在連接網絡，抓取新聞中...", sizeof(s_news_buf) - 1);
    s_news_buf[sizeof(s_news_buf) - 1] = '\0';
    s_news_dirty = true;
    s_news_show  = true;

    /* Wait up to 60 s for news_service_fetch() to complete */
    uint8_t waited = 0;
    while (news_service_count() == 0 && waited < 60) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        waited++;
    }

    if (news_service_count() == 0) {
        strncpy(s_news_buf, "無法取得新聞（請檢查 Wi-Fi 設定）", sizeof(s_news_buf) - 1);
        s_news_buf[sizeof(s_news_buf) - 1] = '\0';
        s_news_dirty = true;
        vTaskDelete(NULL);
        return;
    }

    char buf[NEWS_TITLE_LEN];
    uint8_t idx = 0;
    for (;;) {
        uint8_t cnt = news_service_count();
        if (idx >= cnt) idx = 0;
        if (news_service_get(idx, buf, sizeof(buf))) {
            strncpy(s_news_buf, buf, sizeof(s_news_buf) - 1);
            s_news_buf[sizeof(s_news_buf) - 1] = '\0';
            s_news_dirty = true;
        }
        idx++;
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void UserApp_AppInit() {
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
    espwifi_init();

    setenv("TZ", "GMT-8", 1); // POSIX TZ reversed sign: GMT-8 means GMT+8.
    tzset();
    ESP_LOGI(TAG, "Timezone configured: %s (GMT+8)", getenv("TZ"));
    LogCurrentLocalTime("Local time before SNTP: ");

    const bool is_ntp_synced = espwifi_sync_time("time.asia.apple.com", 30000);
    ESP_LOGI(TAG, "SNTP result: %s", is_ntp_synced ? "SUCCESS" : "FAILED");
    LogCurrentLocalTime("Local time after SNTP: ");

    if (is_ntp_synced) {
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
    lv_obj_set_pos(init_ui.screen_label_4, 0, 94);
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

    // Keep humidity/temperature in one row
    lv_obj_set_pos(init_ui.screen_img_3, 118, 198);
    lv_obj_set_pos(init_ui.screen_label_11, 152, 202);
    lv_obj_set_size(init_ui.screen_label_11, 70, 24);
    lv_obj_set_pos(init_ui.screen_img_4, 222, 198);
    lv_obj_set_pos(init_ui.screen_label_12, 256, 202);
    lv_obj_set_size(init_ui.screen_label_12, 70, 24);

    // Keep battery icon/value on the same row as humidity/temperature.
    lv_obj_clear_flag(init_ui.screen_img_1, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_img_2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(init_ui.screen_img_1, 304, 198);
    lv_obj_set_pos(init_ui.screen_label_7, 336, 202);
    lv_obj_set_size(init_ui.screen_label_7, 60, 24);
    lv_obj_set_style_text_align(init_ui.screen_label_7, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN|LV_STATE_DEFAULT);

    // News label should stick to bottom edge
    lv_obj_set_pos(init_ui.screen_news_label, 0, 236);
    lv_obj_set_size(init_ui.screen_news_label, 400, 64);
    lv_obj_set_style_text_font(init_ui.screen_news_label, news_font, LV_PART_MAIN|LV_STATE_DEFAULT);

    // Hide unrelated status/test labels on the clock page.
    lv_obj_add_flag(init_ui.screen_label_5, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_6, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_8, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_9, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_10, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_13, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(init_ui.screen_label_14, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(init_ui.screen_label_15, "等待操作");
}

void UserApp_TaskInit() {
    xTaskCreatePinnedToCore(Lvgl_Cont1Task, "Lvgl_Cont1Task", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(Lvgl_UserTask, "Lvgl_UserTask", 5 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(Lvgl_SDcardTask, "Lvgl_SDcardTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(Lvgl_WfifBleScanTask, "Lvgl_WfifBleScanTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(BOOT_LoopTask, "BOOT_LoopTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(KEY_LoopTask, "KEY_LoopTask", 4 * 1024, NULL, 2, NULL,1);
    xTaskCreatePinnedToCore(Codec_LoopTask,   "Codec_LoopTask",   4 * 1024, NULL, 4, NULL, 1);
    xTaskCreatePinnedToCore(News_TickerTask,  "News_TickerTask",  4 * 1024, NULL, 2, NULL, 1);
}
