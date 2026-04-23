
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_system.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_app.h"

DisplayPort RlcdPort(12,11,5,40,41,400,300);
static const char *FLUSH_TAG = "lcd_flush";
static const char *APP_TAG = "app_main";
static uint32_t s_flush_seq = 0;
static int64_t s_last_flush_end_us = 0;
static const int64_t FLUSH_GAP_WARN_MS = 300;
static const int64_t FLUSH_DUR_WARN_MS = 140;
static const int64_t FLUSH_WARN_RATE_LIMIT_MS = 2000;
static int64_t s_last_flush_gap_warn_ms = 0;
static int64_t s_last_flush_dur_warn_ms = 0;
static const int64_t FLUSH_PERF_REPORT_INTERVAL_MS = 3000;
static int64_t s_last_flush_perf_report_ms = 0;

static uint32_t s_frame_chunk_count = 0;
static uint32_t s_frame_px_count = 0;
static int64_t s_frame_copy_us = 0;

static uint32_t s_perf_frame_count = 0;
static uint32_t s_perf_chunk_sum = 0;
static uint64_t s_perf_px_sum = 0;
static int64_t s_perf_copy_us_sum = 0;
static int64_t s_perf_tx_us_sum = 0;
static int64_t s_perf_total_us_sum = 0;
static int64_t s_perf_copy_us_max = 0;
static int64_t s_perf_tx_us_max = 0;
static int64_t s_perf_total_us_max = 0;
static uint32_t s_perf_chunk_max = 0;

static const char *ResetReasonToStr(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_UNKNOWN:   return "UNKNOWN";
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT_PIN";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        case ESP_RST_USB:       return "USB";
        case ESP_RST_JTAG:      return "JTAG";
        case ESP_RST_EFUSE:     return "EFUSE";
        case ESP_RST_PWR_GLITCH:return "PWR_GLITCH";
        case ESP_RST_CPU_LOCKUP:return "CPU_LOCKUP";
        default:                return "OTHER";
    }
}

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    const int64_t start_us = esp_timer_get_time();
    const bool is_last = lv_disp_flush_is_last(drv);
    const uint32_t chunk_px = (uint32_t)(area->x2 - area->x1 + 1) * (uint32_t)(area->y2 - area->y1 + 1);

    if (is_last && s_last_flush_end_us != 0) {
        const int64_t gap_ms = (start_us - s_last_flush_end_us) / 1000;
        const int64_t now_ms = start_us / 1000;
        if ((gap_ms >= FLUSH_GAP_WARN_MS) &&
            ((now_ms - s_last_flush_gap_warn_ms) >= FLUSH_WARN_RATE_LIMIT_MS)) {
            s_last_flush_gap_warn_ms = now_ms;
            ESP_LOGW(
                FLUSH_TAG,
                "flush gap=%lld ms area=(%d,%d)-(%d,%d)",
                (long long)gap_ms,
                area->x1, area->y1, area->x2, area->y2
            );
        }
    }

  	uint16_t *buffer = (uint16_t *)color_map;
  	for(int y = area->y1; y <= area->y2; y++) 
  	{
  	 	for(int x = area->x1; x <= area->x2; x++) 
  	 	{
  	 	   	uint8_t color = (*buffer < 0x7fff) ? ColorBlack : ColorWhite;
  	 	   	RlcdPort.RLCD_SetPixel(x, y, color);
  	 	   	buffer++;
  	 	}
  	}
    const int64_t copy_end_us = esp_timer_get_time();
    const int64_t copy_us = copy_end_us - start_us;
    s_frame_copy_us += copy_us;
    s_frame_px_count += chunk_px;
    s_frame_chunk_count++;

    // LVGL can call flush_cb multiple times per frame.
    // Push to panel only on the last chunk to avoid redundant full-screen transfers.
    int64_t tx_us = 0;
    if (is_last) {
        const int64_t tx_start_us = esp_timer_get_time();
  	    RlcdPort.RLCD_Display();
        tx_us = esp_timer_get_time() - tx_start_us;
    }

    const int64_t end_us = esp_timer_get_time();
    if (is_last) {
        s_last_flush_end_us = end_us;
        s_flush_seq++;
        const int64_t frame_total_us = s_frame_copy_us + tx_us;
        const int64_t dur_ms = frame_total_us / 1000;
        const int64_t now_ms = end_us / 1000;
        if ((dur_ms >= FLUSH_DUR_WARN_MS) &&
            ((now_ms - s_last_flush_dur_warn_ms) >= FLUSH_WARN_RATE_LIMIT_MS)) {
            s_last_flush_dur_warn_ms = now_ms;
            ESP_LOGW(
                FLUSH_TAG,
                "flush#%lu dur=%lld ms copy=%lld ms tx=%lld ms chunks=%lu px=%lu area=(%d,%d)-(%d,%d)",
                (unsigned long)s_flush_seq,
                (long long)dur_ms,
                (long long)(s_frame_copy_us / 1000),
                (long long)(tx_us / 1000),
                (unsigned long)s_frame_chunk_count,
                (unsigned long)s_frame_px_count,
                area->x1, area->y1, area->x2, area->y2
            );
        }

        s_perf_frame_count++;
        s_perf_chunk_sum += s_frame_chunk_count;
        s_perf_px_sum += s_frame_px_count;
        s_perf_copy_us_sum += s_frame_copy_us;
        s_perf_tx_us_sum += tx_us;
        s_perf_total_us_sum += frame_total_us;
        if (s_frame_copy_us > s_perf_copy_us_max) s_perf_copy_us_max = s_frame_copy_us;
        if (tx_us > s_perf_tx_us_max) s_perf_tx_us_max = tx_us;
        if (frame_total_us > s_perf_total_us_max) s_perf_total_us_max = frame_total_us;
        if (s_frame_chunk_count > s_perf_chunk_max) s_perf_chunk_max = s_frame_chunk_count;

        if ((now_ms - s_last_flush_perf_report_ms) >= FLUSH_PERF_REPORT_INTERVAL_MS) {
            if (s_perf_frame_count > 0) {
                ESP_LOGI(
                    FLUSH_TAG,
                    "perf %lus: frames=%lu avg_total=%lldms avg_copy=%lldms avg_tx=%lldms avg_chunks=%lu max_total=%lldms max_copy=%lldms max_tx=%lldms max_chunks=%lu avg_px=%lu",
                    (unsigned long)(FLUSH_PERF_REPORT_INTERVAL_MS / 1000),
                    (unsigned long)s_perf_frame_count,
                    (long long)((s_perf_total_us_sum / s_perf_frame_count) / 1000),
                    (long long)((s_perf_copy_us_sum / s_perf_frame_count) / 1000),
                    (long long)((s_perf_tx_us_sum / s_perf_frame_count) / 1000),
                    (unsigned long)(s_perf_chunk_sum / s_perf_frame_count),
                    (long long)(s_perf_total_us_max / 1000),
                    (long long)(s_perf_copy_us_max / 1000),
                    (long long)(s_perf_tx_us_max / 1000),
                    (unsigned long)s_perf_chunk_max,
                    (unsigned long)(s_perf_px_sum / s_perf_frame_count)
                );
            }
            s_last_flush_perf_report_ms = now_ms;
            s_perf_frame_count = 0;
            s_perf_chunk_sum = 0;
            s_perf_px_sum = 0;
            s_perf_copy_us_sum = 0;
            s_perf_tx_us_sum = 0;
            s_perf_total_us_sum = 0;
            s_perf_copy_us_max = 0;
            s_perf_tx_us_max = 0;
            s_perf_total_us_max = 0;
            s_perf_chunk_max = 0;
        }

        s_frame_chunk_count = 0;
        s_frame_px_count = 0;
        s_frame_copy_us = 0;
    }
	lv_disp_flush_ready(drv);
}

extern "C" void app_main(void)
{
    const esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGW(
        APP_TAG,
        "Boot reset reason: %s (%d)",
        ResetReasonToStr(reset_reason),
        (int)reset_reason
    );

	RlcdPort.RLCD_Init();
	Lvgl_PortInit(400,300,Lvgl_FlushCallback);
	if(Lvgl_lock(-1)) {
		UserApp_UiInit();
  	  	Lvgl_unlock();
        Lvgl_request_refresh();
  	}
	UserApp_AppInit();
	UserApp_TaskInit();
}
