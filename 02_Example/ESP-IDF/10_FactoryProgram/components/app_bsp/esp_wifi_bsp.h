#ifndef ESP_WIFI_BSP_H
#define ESP_WIFI_BSP_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
extern EventGroupHandle_t wifi_even_;


typedef struct
{
  char _ip[25];
  int8_t rssi;
  int8_t apNum;
}esp_bsp_t;
extern esp_bsp_t user_esp_bsp;

#ifdef __cplusplus
extern "C" {
#endif

void espwifi_init(void);
void espwifi_deinit(void);

/* Block until STA gets an IP (bit 0x04) or timeout. Returns true if connected. */
bool espwifi_wait_ip(uint32_t timeout_ms);

/* Sync system time from NTP and wait for first confirmed sync.
 * SNTP remains running for periodic re-sync. */
bool espwifi_sync_time(const char *server, uint32_t timeout_ms);


#ifdef __cplusplus
}
#endif

#endif
