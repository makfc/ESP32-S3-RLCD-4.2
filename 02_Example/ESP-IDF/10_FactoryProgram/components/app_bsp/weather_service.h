#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool weather_service_fetch(void);
bool weather_service_get(int *wong_tai_sin_temp_c, int *humidity_percent);
bool weather_service_get_condition(int *icon_code, const char **description);
bool weather_service_has_valid(void);

/* Current warning detail count from warningInfo API. */
size_t weather_service_warn_count(void);

/* Build warning headline text like:
 * 雷暴警告 天文台在... (4月24日01:25:00 | 20分鐘前)
 */
bool weather_service_warn_get_display(size_t idx, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
