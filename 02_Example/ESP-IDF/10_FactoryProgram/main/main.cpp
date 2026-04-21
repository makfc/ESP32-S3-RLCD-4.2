
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <esp_timer.h>
#include <esp_log.h>

#include "display_bsp.h"
#include "lvgl_bsp.h"
#include "user_app.h"

DisplayPort RlcdPort(12,11,5,40,41,400,300);
static const char *TAG = "lcd_flush";
static uint32_t s_flush_count = 0;
static TickType_t s_last_flush_tick = 0;

static void Lvgl_FlushCallback(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    const TickType_t flush_start_tick = xTaskGetTickCount();
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
	lv_disp_flush_ready(drv);

    const TickType_t flush_end_tick = xTaskGetTickCount();
    const uint32_t flush_ms = (uint32_t)((flush_end_tick - flush_start_tick) * portTICK_PERIOD_MS);
    const uint32_t flush_gap_ms = (s_last_flush_tick == 0)
        ? 0
        : (uint32_t)((flush_start_tick - s_last_flush_tick) * portTICK_PERIOD_MS);
    s_last_flush_tick = flush_start_tick;
    s_flush_count++;
    ESP_LOGI(
        TAG,
        "flush #%lu tick=%lu gap=%lu ms dur=%lu ms area=(%d,%d)-(%d,%d)",
        (unsigned long)s_flush_count,
        (unsigned long)flush_start_tick,
        (unsigned long)flush_gap_ms,
        (unsigned long)flush_ms,
        area->x1,
        area->y1,
        area->x2,
        area->y2
    );
}

extern "C" void app_main(void)
{
	UserApp_AppInit();
	RlcdPort.RLCD_Init();
	Lvgl_PortInit(400,300,Lvgl_FlushCallback);
	if(Lvgl_lock(-1)) {
		UserApp_UiInit();
  	  	Lvgl_unlock();
        Lvgl_request_refresh();
  	}
	UserApp_TaskInit();
}
