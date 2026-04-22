
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
    if (s_last_flush_end_us != 0) {
        const int64_t gap_ms = (start_us - s_last_flush_end_us) / 1000;
        if (gap_ms >= 250) {
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
  	RlcdPort.RLCD_Display();
    const int64_t end_us = esp_timer_get_time();
    s_last_flush_end_us = end_us;
    s_flush_seq++;
    const int64_t dur_ms = (end_us - start_us) / 1000;
    if (dur_ms >= 80) {
        ESP_LOGW(
            FLUSH_TAG,
            "flush#%lu dur=%lld ms area=(%d,%d)-(%d,%d)",
            (unsigned long)s_flush_seq,
            (long long)dur_ms,
            area->x1, area->y1, area->x2, area->y2
        );
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
