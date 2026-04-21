#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEWS_MAX_COUNT   10
#define NEWS_TITLE_LEN   192   /* UTF-8 bytes; one TC char = 3 bytes */

/* Fetch RSS headlines (blocking, call while Wi-Fi has IP). */
void    news_service_fetch(void);

/* Number of headlines successfully fetched. */
uint8_t news_service_count(void);

/* Copy headline at idx (0-based) into buf. Returns false if idx out of range. */
bool    news_service_get(uint8_t idx, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
