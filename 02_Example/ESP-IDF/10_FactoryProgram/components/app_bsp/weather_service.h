#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool weather_service_fetch(void);
bool weather_service_get(int *wong_tai_sin_temp_c, int *humidity_percent);
bool weather_service_get_condition(int *icon_code, const char **description);
bool weather_service_has_valid(void);

#ifdef __cplusplus
}
#endif
