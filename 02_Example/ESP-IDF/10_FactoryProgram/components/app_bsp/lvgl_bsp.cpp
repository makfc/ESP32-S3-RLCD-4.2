#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <esp_log.h>
#include <esp_timer.h>
#include "lvgl_bsp.h"

static lv_disp_draw_buf_t disp_buf; 		// contains internal graphic buffer(s) called draw buffer(s)
static lv_disp_drv_t disp_drv;      		// contains callback functions
static SemaphoreHandle_t lvgl_mux = NULL;
static TaskHandle_t lvgl_task_handle = NULL;

static const char *TAG = "LvglPort";
static const int64_t LVGL_HANDLER_WARN_MS = 220;
static const int64_t LVGL_HANDLER_WARN_RATE_LIMIT_MS = 2000;
static int64_t s_last_handler_warn_ms = 0;
static const BaseType_t LVGL_TASK_CORE_ID = 1;

static void Increase_lvgl_tick(void *arg)
{
  	lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

bool Lvgl_lock(int timeout_ms)
{
  	const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  	return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;       
}

void Lvgl_unlock(void)
{
  	assert(lvgl_mux && "bsp_display_start must be called first");
  	xSemaphoreGive(lvgl_mux);
}

void Lvgl_request_refresh(void)
{
    if (lvgl_task_handle != NULL) {
        xTaskNotifyGive(lvgl_task_handle);
    }
}

static void Lvgl_port_task(void *arg)
{
  	uint32_t task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	for(;;)
  	{
  	  	if (Lvgl_lock(-1)) 
  	  	{
            const int64_t handler_start_us = esp_timer_get_time();
  	  	  	task_delay_ms = lv_timer_handler();
            const int64_t handler_dur_ms = (esp_timer_get_time() - handler_start_us) / 1000;
            const int64_t now_ms = handler_start_us / 1000;
            if ((handler_dur_ms >= LVGL_HANDLER_WARN_MS) &&
                ((now_ms - s_last_handler_warn_ms) >= LVGL_HANDLER_WARN_RATE_LIMIT_MS)) {
                s_last_handler_warn_ms = now_ms;
                ESP_LOGW(TAG, "lv_timer_handler blocked %lld ms", (long long)handler_dur_ms);
            }
  	  	  	//Release the mutex
  	  	  	Lvgl_unlock();
  	  	}
  	  	if (task_delay_ms > LVGL_TASK_MAX_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MAX_DELAY_MS;
  	  	} else if (task_delay_ms < LVGL_TASK_MIN_DELAY_MS)
  	  	{
  	  	  	task_delay_ms = LVGL_TASK_MIN_DELAY_MS;
  	  	}
  	  	(void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(task_delay_ms));
  	}
}


void Lvgl_PortInit(int width, int height,DispFlushCb flush_cb) {
    lvgl_mux = xSemaphoreCreateMutex();
    lv_init();
    lv_color_t *buffer1 = (lv_color_t *)heap_caps_malloc(width * height * sizeof(lv_color_t) , MALLOC_CAP_SPIRAM);
  	assert(buffer1);
	lv_color_t *buffer2 = (lv_color_t *)heap_caps_malloc(width * height * sizeof(lv_color_t) , MALLOC_CAP_SPIRAM);
  	assert(buffer2);

    lv_disp_draw_buf_init(&disp_buf, buffer1, buffer2, width * height);
    ESP_LOGI(TAG, "Register display driver to LVGL");

    lv_disp_drv_init(&disp_drv);
  	disp_drv.hor_res = width;
  	disp_drv.ver_res = height;
  	disp_drv.flush_cb = flush_cb;
	// Keep partial refresh enabled to reduce redraw latency for small clock updates.
	disp_drv.full_refresh = 0;
  	disp_drv.draw_buf = &disp_buf;
  	lv_disp_drv_register(&disp_drv);

    ESP_LOGI(TAG, "Install LVGL tick timer");
  	esp_timer_create_args_t lvgl_tick_timer_args = {};
  	lvgl_tick_timer_args.callback = &Increase_lvgl_tick;
  	lvgl_tick_timer_args.name = "lvgl_tick";
    esp_timer_handle_t lvgl_tick_timer = NULL;
  	ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  	ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer,LVGL_TICK_PERIOD_MS * 1000));

    ESP_LOGI(TAG, "Create LVGL task on core %ld", (long)LVGL_TASK_CORE_ID);
    xTaskCreatePinnedToCore(Lvgl_port_task, "LVGL", 8 * 1024, NULL, 5, &lvgl_task_handle, LVGL_TASK_CORE_ID);
}
