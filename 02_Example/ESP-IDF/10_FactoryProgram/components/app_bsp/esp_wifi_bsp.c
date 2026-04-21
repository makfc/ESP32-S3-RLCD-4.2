#include <stdio.h>
#include "esp_wifi_bsp.h"
#include "esp_wifi.h"  
#include "esp_event.h" 
#include "nvs_flash.h" 
#include "esp_log.h"


#include "string.h" 
#include <time.h>
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_sntp.h"

EventGroupHandle_t wifi_even_ = NULL;

esp_bsp_t user_esp_bsp;

static esp_netif_t *net = NULL;
static const char *TAG = "wifiSta";
static volatile bool s_sntp_sync_seen = false;

static const char *SntpSyncStatusToStr(sntp_sync_status_t status)
{
    switch (status) {
        case SNTP_SYNC_STATUS_RESET:
            return "RESET";
        case SNTP_SYNC_STATUS_COMPLETED:
            return "COMPLETED";
        case SNTP_SYNC_STATUS_IN_PROGRESS:
            return "IN_PROGRESS";
        default:
            return "UNKNOWN";
    }
}

static void SntpSyncNotificationCb(struct timeval *tv)
{
    time_t now = 0;
    struct tm timeinfo = {0};
    char local_buf[40] = {0};

    s_sntp_sync_seen = true;
    if (tv != NULL) {
        now = (time_t)tv->tv_sec;
    } else {
        time(&now);
    }
    localtime_r(&now, &timeinfo);
    strftime(local_buf, sizeof(local_buf), "%Y-%m-%d %H:%M:%S", &timeinfo);

    ESP_LOGI(
        TAG,
        "SNTP callback: local=%s epoch=%lld reach=0x%02x",
        local_buf,
        (long long)now,
        (unsigned int)esp_sntp_getreachability(0)
    );
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void example_scan_wifi_task(void *arg);
void espwifi_init(void)
{
    memset(&user_esp_bsp,0,sizeof(esp_bsp_t));
    wifi_even_ = xEventGroupCreate();
    nvs_flash_init();                           // Initialize default NVS storage
    esp_netif_init();                           // Initialize TCP/IP stack
    esp_event_loop_create_default();            // Create default event loop
    net = esp_netif_create_default_wifi_sta();  // Add TCP/IP stack to the default event loop
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); // Default configuration
    esp_wifi_init(&cfg);                                 // Initialize WiFi
    esp_event_handler_instance_t Instance_WIFI_IP;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &Instance_WIFI_IP);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &Instance_WIFI_IP);
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "PDCN",
            .password = "1234567890",
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);               // Set mode to STA
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config); // Configure WiFi
    esp_wifi_start();                               // Start WiFi
    xTaskCreatePinnedToCore(example_scan_wifi_task, "example_scan_wifi_task", 3000, NULL, 2, NULL,0);   
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START)
    {
        xEventGroupSetBits(wifi_even_,0x01);
    }
    else if (event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip[25];
        uint32_t pxip = event->ip_info.ip.addr;
        sprintf(ip, "%d.%d.%d.%d", (uint8_t)(pxip), (uint8_t)(pxip >> 8), (uint8_t)(pxip >> 16), (uint8_t)(pxip >> 24));
        strncpy(user_esp_bsp._ip, ip, sizeof(user_esp_bsp._ip) - 1);
        ESP_LOGI("wifiSta", "IP: %s", ip);
        xEventGroupSetBits(wifi_even_, 0x04);   /* signal: has IP */
    }
    else if(event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGD("wifiSta","Disconnected");
    }
}

bool espwifi_wait_ip(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(wifi_even_, 0x04, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & 0x04) != 0;
}

bool espwifi_sync_time(const char *server, uint32_t timeout_ms)
{
    const char *ntp_server = (server != NULL && server[0] != '\0') ? server : "pool.ntp.org";
    const uint32_t sync_interval_ms = 60UL * 60UL * 1000UL; // 1 hour

    if (!espwifi_wait_ip(timeout_ms)) {
        ESP_LOGW(TAG, "SNTP skipped: no IP within %lu ms", (unsigned long)timeout_ms);
        return false;
    }

    ESP_LOGI(
        TAG,
        "SNTP start: server=%s timeout=%lu ms interval=%lu ms",
        ntp_server,
        (unsigned long)timeout_ms,
        (unsigned long)sync_interval_ms
    );

    s_sntp_sync_seen = false;
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    sntp_set_time_sync_notification_cb(SntpSyncNotificationCb);
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_set_sync_interval(sync_interval_ms);
    sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, ntp_server);
    esp_sntp_init();

    const int64_t start_us = esp_timer_get_time();
    while ((uint32_t)((esp_timer_get_time() - start_us) / 1000) < timeout_ms) {
        sntp_sync_status_t status = sntp_get_sync_status();
        if (s_sntp_sync_seen || (status == SNTP_SYNC_STATUS_COMPLETED)) {
            time_t now = 0;
            struct tm timeinfo = {0};

            time(&now);
            localtime_r(&now, &timeinfo);
            ESP_LOGI(
                TAG,
                "SNTP first sync ok: %04d-%02d-%02d %02d:%02d:%02d (epoch=%lld status=%s)",
                timeinfo.tm_year + 1900,
                timeinfo.tm_mon + 1,
                timeinfo.tm_mday,
                timeinfo.tm_hour,
                timeinfo.tm_min,
                timeinfo.tm_sec,
                (long long)now,
                SntpSyncStatusToStr(status)
            );
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    {
        time_t now = 0;
        struct tm timeinfo = {0};
        sntp_sync_status_t status = sntp_get_sync_status();
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGW(
            TAG,
            "SNTP timeout: current=%04d-%02d-%02d %02d:%02d:%02d (epoch=%lld status=%s reach=0x%02x)",
            timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1,
            timeinfo.tm_mday,
            timeinfo.tm_hour,
            timeinfo.tm_min,
            timeinfo.tm_sec,
            (long long)now,
            SntpSyncStatusToStr(status),
            (unsigned int)esp_sntp_getreachability(0)
        );
    }
    // Keep SNTP running. It can still sync later and continue periodic correction.
    return false;
}

void espwifi_deinit(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(net);
    esp_event_loop_delete_default();
    //esp_netif_deinit();
    //nvs_flash_deinit();
}



static void example_scan_wifi_task(void *arg)
{
    uint16_t rec = 0;
    EventBits_t even = xEventGroupWaitBits(wifi_even_,0x01,pdTRUE,pdTRUE,pdMS_TO_TICKS(15000));
    if( even & 0x01 ) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_start(NULL,true));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_scan_get_ap_num(&rec));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_clear_ap_list());
    }
    user_esp_bsp.apNum = rec;
    xEventGroupSetBits(wifi_even_, 0x02);   /* signal: scan done */

    /* Now attempt to connect so news fetch can get an IP */
    esp_wifi_connect();
    vTaskDelete(NULL);
}
